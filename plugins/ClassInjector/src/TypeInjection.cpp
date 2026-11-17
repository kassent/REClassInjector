#include "TypeInjection.h"
#include "ClassInjector.h"
#include "TypeDatabaseBuilder.h"
#include "ClassProviderContext.h"
#include "ClassProviders.h"
#include "ClassVirtualTable.h"
#include "MethodTrampolines.h"
#include <HookUtils.h>
#include <BranchTrampoline.h>
#include <LogUtils.h>
#include <FileUtils.h>
#include <array>
#include <atomic>
#include <stdexcept>

// 启动接入层：HIDWrapper 触发 TOML / DLL 登记和 hook；首次 builder 消费已核对的声明。
// 原 relocator 返回时扩展并发布 TDB，原 builder 返回时安装托管虚表入口。
// 这里控制注入时机和运行时查找；表结构构建在 TypeDatabaseBuilder，入口生成在 MethodTrampolines / InvokeAdapters。
namespace ci {
    std::unique_ptr<TypeDatabase> g_database;
} // namespace ci

// 以下 namespace 按本地 hook 职责归组；入口名为行为别名，不冒充已恢复的引擎符号。
namespace ci::hooks::VM {
    using FnInitializeManagedTypes = void (*)();
    using FnBuildTypeTables = uint32_t (*)(RE::VM* vm, RE::TDB* tdb);
    using FnRegisterTypeMethods = void (*)(RE::VM* vm, RE::RETypeDefinition** outTypeDefinition,
                                          uint32_t typeIndex, void* descriptor,
                                          void** virtualMethodEntries, uint32_t virtualMethodCount,
                                          void** memberMethodEntries, uint32_t memberMethodCount, void* auxiliary);
    using FnFindRuntimeTypeNarrow = void* (*)(void* context, const char* name, uint32_t length);
    using FnFindRuntimeTypeWide = void* (*)(void* context, const wchar_t* name, uint32_t length);

    // LookupCoreFunctions 确认后赋值；原初始化入口同时供 CALL 桥和 invoke table 定位使用。
    static FnInitializeManagedTypes RE_InitManagedTypes{};
    static FnBuildTypeTables RE_BuildTypeTables{};
    static FnRegisterTypeMethods RE_RegisterTypeMethods{};
    static FnFindRuntimeTypeNarrow RE_FindRuntimeTypeNarrow{};
    static FnFindRuntimeTypeWide RE_FindRuntimeTypeWide{};
    // 引擎全局 VM 指针槽；每次解引用取得当前对象，不保存安装期的空快照。
    static RE::VM** g_vmWrapper{};
} // namespace ci::hooks::VM

namespace ci::hooks::TDB {
    using FnRelocate = uint32_t (*)(RE::TDB* tdb);
    using FnFindTypeDefinition = RE::RETypeDefinition* (*)(void* context, const char* name);
    using FnFindGenericTypeDefinition = RE::RETypeDefinition* (*)(void* context, uint32_t definitionHash,
                                                                 uint32_t instanceHash);

    static FnRelocate RE_Relocate{};
    static FnFindTypeDefinition RE_FindTypeDefinition{};
    static FnFindGenericTypeDefinition RE_FindGenericTypeDefinition{};

    static void LimitOriginalNameLookupRange(uint32_t originalCount) {
        const auto findTypeDefinitionAddress = uintptr_t(RE_FindTypeDefinition);
        // 已核对三处数量引用在目标函数内各自唯一，直接按指令签名定位。
        constexpr std::array<std::string_view, 3> signatures{
            "8B 15 ? ? ? ? FF CA",       // mov edx, [count]：二分查找上界。
            "3B 1D ? ? ? ? 73",          // cmp ebx, [count]：同 hash 扫描的起始检查。
            "8B 05 ? ? ? ? 49 83 C4 50", // mov eax, [count]：同 hash 扫描的循环上界。
        };
        struct Reference {
            uintptr_t displacement;
            int32_t value;
        };
        std::array<Reference, signatures.size()> references;
        // 新类型不满足原名称查找的排序规则；近址存储由 trampoline 持有，供原指令读取旧上界。
        auto* originalCountStorage = static_cast<uint32_t*>(g_branchTrampoline.Allocate(sizeof(uint32_t)));
        if (!originalCountStorage) {
            throw std::runtime_error("name lookup count storage allocation failed");
        }
        *originalCountStorage = originalCount;
        // 三条指令都是 6 字节，disp32 位于 +2；全部确认后再写入，避免只改了一部分。
        for (size_t i = 0; i < signatures.size(); ++i) {
            auto instruction =
                HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(findTypeDefinitionAddress), signatures[i]);
            if (!instruction) {
                throw std::runtime_error("name lookup count reference signature not found");
            }
            auto difference = int64_t(uintptr_t(originalCountStorage)) - int64_t(instruction + 6);
            if (difference < INT32_MIN || difference > INT32_MAX) {
                throw std::runtime_error("name bound storage is outside RIP-relative range");
            }
            references[i] = {.displacement = instruction + 2, .value = int32_t(difference)};
        }
        for (auto& reference : references) {
            HookUtils::SafeWrite32(reference.displacement, uint32_t(reference.value));
        }
    }
} // namespace ci::hooks::TDB

namespace ci::TypeInjection {
    // metadata 发布、类型表完成、方法入口就绪分别由 database、runtimeTypesReady、g_ready 表示。
    std::atomic_bool g_ready{};
    static std::vector<CustomTypeConfig> g_typeConfigs;
    static std::vector<ClassProviderNotification> g_providerCallbacks;

    // 返回 VM 创建的 System.RuntimeType 对象，而非 TDB 类型定义记录。
    static void* FindCustomRuntimeType(std::string_view name) {
        if (!g_database) {
            return nullptr;
        }
        auto* type = g_database->FindType(name);
        if (!type) {
            return nullptr;
        }
        auto** runtimeTypes = g_database->vm->runtimeTypes;
        return runtimeTypes ? runtimeTypes[type->index] : nullptr;
    }

    // 输入为已 relocation 的 TDB：构建扩展、准备方法入口，再发布给后续原生 builder。
    // 发布后的数据库与代码地址保留至进程退出。
    static void InjectTDB(RE::TDB& tdb) {
        using namespace hooks::VM;
        using namespace hooks::TDB;

        try {
            if (tdb.version != 81) {
                throw std::runtime_error("expected TDB81");
            }
            auto methodBaseLoadAddr = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(RE_RegisterTypeMethods),
                "4C 8D 35 ? ? ? ? 45 31 FF", 0x140);
            if (!methodBaseLoadAddr) {
                throw std::runtime_error("method encoding base load is missing");
            }
            // 从 LEA R14, [RIP+disp32] 解析方法地址基址：方法入口 = methodBaseAddr + MethodDef::encodedOffset。
            const auto methodBaseAddr = HookUtils::ReadOffsetData(methodBaseLoadAddr, 3, 7);
            _MESSAGE("Metadata injection method base=%p", reinterpret_cast<void*>(methodBaseAddr));
            auto database = BuildTDB(tdb, std::move(g_typeConfigs));
            database->encodingBase = methodBaseAddr;
            database->vm = *g_vmWrapper;
            CreateMethodTrampolines(*database);
            PrepareInvokeAdapters(*database);
            if (database->invokeDispatchId && !InstallInvokeDispatch(*database, reinterpret_cast<void*>(RE_InitManagedTypes))) {
                throw std::runtime_error("invoke dispatch installation failed");
            }
            if (!database->customTypes.empty()) {
                LimitOriginalNameLookupRange(database->originalCount);
            }
            g_database = std::move(database);
            g_database->Publish(tdb);
            _MESSAGE("Published metadata: %u -> %u types, %zu custom methods", g_database->originalCount,
                     tdb.numTypes, g_database->customMethods.size());
        } catch (const std::exception& error) {
            _MESSAGE("Metadata injection failed before publish: %s", error.what());
        }
    }

    // VM 类型表构建返回：检查布局、首次更新虚表，只发布结构就绪，不通知 Provider。
    bool OnVmTypeTablesBuilt() {
        // 扩展没有新类型身份；确认原 builder 保留普通托管字段起点，并按新字段区大小分配。
        for (auto& extension : g_database->extensions) {
            auto& definition = g_database->types[extension.index];
            auto& impl = g_database->typeImpls[definition.implIndex];
            if (!definition.objectInfo || definition.objectInfo[0] != &definition ||
                intptr_t(definition.objectInfo[-1]) != 0x10 || definition.instanceSize != 0x10 + impl.fieldSize) {
                _MESSAGE("Runtime field extension incomplete: %s",
                         g_database->GetFullName(extension.index).c_str());
                return false;
            }
            _MESSAGE("Extended %s: id=%u, fields=%u -> %u, instance=%u",
                     g_database->GetFullName(extension.index).c_str(), extension.index, extension.originalFieldSize,
                     uint32_t(impl.fieldSize), definition.instanceSize);
        }
        for (auto& type : g_database->customTypes) {
            auto& definition = g_database->types[type.index];
            // 开放身份记录走原 builder 的 sentinel 路径，不要求独立 TypeInfo 或 ObjectInfo。
            if (definition.typeFlags & 0x08000000) {
                if (definition.objectInfo) {
                    _MESSAGE("Generic definition unexpectedly has ObjectInfo: %s", type.name.c_str());
                    return false;
                }
                continue;
            }
            if (!g_database->IsRuntimeTypeReady(type)) {
                _MESSAGE("Runtime registration incomplete: %s", type.name.c_str());
                return false;
            }
        }
        for (const auto& type : g_database->customTypes) UpdateVirtualTable(*g_database, type);
        // Original field sizes are only needed until the builder's final layout has been checked.
        std::vector<RuntimeExtension>{}.swap(g_database->extensions);
        g_database->runtimeTypesReady = true;
        _MESSAGE("Runtime shapes ready: custom types=%zu; native method entries pending; providers not notified",
                 g_database->customTypes.size());
        for (auto& type : g_database->customTypes) {
            _MESSAGE("Registered %s: id=%u, TypeInfo=%p, ObjectInfo=%p", type.name.c_str(), type.index,
                     g_database->types[type.index].typeInfo, g_database->types[type.index].objectInfo);
        }
        return true;
    }

    // 原生方法登记完成后：更新继承入口、准备 Context、发布 ready，再通知 Provider。
    // cctor 前和外层返回两处调用共用 ready 门禁，不表示引擎整体初始化完成。
    bool OnVmMethodsRegistered() {
        if (g_ready.load(std::memory_order_acquire)) {
            return true;
        }
        if (!g_database || !g_database->runtimeTypesReady) {
            _MESSAGE("Runtime entries FAILED: metadata=%d shapes ready=%d; ready remains false; providers not notified",
                     bool(g_database), g_database && g_database->runtimeTypesReady);
            return false;
        }
        // 首次通知在全局 cctor 前执行；外层返回的重复调用由上面的 ready 门禁短路。
        for (const auto& type : g_database->customTypes) UpdateVirtualTable(*g_database, type);
        PrepareContext();
        _MESSAGE("Runtime entries ready; notifying providers=%zu", g_providerCallbacks.size());
        g_ready.store(true, std::memory_order_release);
        NotifyClassProviders(g_providerCallbacks, GetContext());
        _MESSAGE("CI provider initialization complete; injection ready=1");
        return true;
    }
} // namespace ci::TypeInjection

namespace ci::hooks::TDB {
    // 原 relocation 成功返回后，在首次初始化窗口内注入。
    static bool HookRelocate() {
        using namespace TypeInjection;
        using namespace VM;

        return HookLambda(RE_Relocate, [](RE::TDB* tdb) -> uint32_t {
            auto result = original(tdb);
            auto* vm = *g_vmWrapper;
            _MESSAGE("Metadata relocated: success=%u, TDB=%p, version=%u, types=%u, VM=%p, current TDB=%p",
                     result & 0xFF, tdb, tdb->version, tdb->numTypes, vm, vm ? vm->tdb : nullptr);
            // 原 builder 在 relocation 成功后才设置 vm->tdb；非空表示已离开首次注入窗口。
            if (!(result & 0xFF) || !vm || vm->tdb || g_database) {
                return result;
            }
            if (g_typeConfigs.empty() && g_providerCallbacks.empty()) {
                _MESSAGE("Metadata injection skipped: no type declarations or CI initialization providers loaded");
                return result;
            }
            InjectTDB(*tdb);
            return result;
        });
    }

    // 新类型优先查自有数据库，原类型继续走引擎查找。
    static bool HookFindTypeDefinition() {
        return HookLambda(RE_FindTypeDefinition, [](void* context, const char* name) -> RE::RETypeDefinition* {
            if (g_database) {
                if (auto* type = g_database->FindType(name)) {
                    return &g_database->types[type->index];
                }
            }
            return original(context, name);
        });
    }

    // 返回闭合泛型定义记录；身份由 generic definition 和 instance 两个 hash 共同确定。
    static bool HookFindGenericTypeDefinition() {
        return HookLambda(RE_FindGenericTypeDefinition,
            [](void* context, uint32_t definitionHash, uint32_t instanceHash) -> RE::RETypeDefinition* {
                if (g_database) {
                    for (auto& type : g_database->customTypes) {
                        if (type.genericDefinition &&
                            g_database->types[type.genericDefinition].fqnHash == definitionHash &&
                            g_database->types[type.index].fqnHash == instanceHash) {
                            return &g_database->types[type.index];
                        }
                    }
                }
                return original(context, definitionHash, instanceHash);
            });
    }

    static void InstallHooks() {
        if (!HookRelocate()) {
            throw std::runtime_error("metadata relocator hook failed");
        }
        if (!HookFindTypeDefinition()) {
            throw std::runtime_error("type name lookup hook failed");
        }
        if (!HookFindGenericTypeDefinition()) {
            throw std::runtime_error("closed type lookup hook failed");
        }
    }
} // namespace ci::hooks::TDB

namespace ci::hooks::VM {
    // 只让追加类型通过原分区判断；不放行原先被排除的既有类型。
    static bool HookInitializeTypeInfo() {
        // 保存匹配到的检查代码地址，而非模式对象：
        // +0: IMUL ECX, ECX, 0xCCCCCCCD；+6: CMP dword ptr [RIP+disp32], ECX。
        // +12 位于 CMP 之后，在后续 JBE 消费标志位之前安装 mid-hook。
        auto typeInfoCheckAddress = HookUtils::ScanIDAPattern("69 C9 CD CC CC CC 39 0D ? ? ? ?");
        if (!typeInfoCheckAddress) {
            throw std::runtime_error("TypeInfo partition signature not found");
        }
        static auto initializeTypeInfoHook = safetyhook::create_mid(
            reinterpret_cast<void*>(typeInfoCheckAddress + 12), [](SafetyHookContext& context) {
                if (g_database && uint32_t(context.rcx) >= g_database->originalCount &&
                    uint32_t(context.rcx) < g_database->types.size()) {
                    // 清除 CF/ZF，使后续 JBE 不跳转，继续初始化新增类型。
                    context.rflags &= ~uintptr_t(0x41);
                }
            });
        return static_cast<bool>(initializeTypeInfoHook);
    }

    // 原生方法登记后，保留既有的全量自定义虚表更新范围。
    static bool HookRegisterTypeMethods() {
        return HookLambda(RE_RegisterTypeMethods,
            [](RE::VM* vm, RE::RETypeDefinition** outTypeDefinition, uint32_t typeIndex, void* descriptor,
               void** virtualMethodEntries, uint32_t virtualMethodCount,
               void** memberMethodEntries, uint32_t memberMethodCount, void* auxiliary) {
                original(vm, outTypeDefinition, typeIndex, descriptor, virtualMethodEntries, virtualMethodCount,
                         memberMethodEntries, memberMethodCount, auxiliary);
                if (g_database && g_database->runtimeTypesReady) {
                    for (auto& type : g_database->customTypes) {
                        if (type.parent == typeIndex) {
                            for (const auto& child : g_database->customTypes) UpdateVirtualTable(*g_database, child);
                            break;
                        }
                    }
                }
            });
    }

    static bool HookRunStaticConstructors() {
        using namespace TypeInjection;

        // 原函数尾部按表执行全局cctor，EnemyUtil等静态数组在此被构造。
        // 在外层CALL返回后通知已太晚：即使改了cctor机器码，现有Type[]仍只有150。
        // 只在已核实的cctor循环之前通知，不覆盖RF扫描的入口/原生entry注册循环。
        const auto staticConstructorLoopPattern = HookUtils::LookupFunctionPattern(
            reinterpret_cast<void*>(RE_InitManagedTypes),
            "C6 05 ? ? ? ? 00 31 DB 4C 8D 35 ? ? ? ? 48 89 F9 42 FF 14 33");
        if (!staticConstructorLoopPattern) {
            _MESSAGE("Native initialization hook FAILED: pre-cctor entry-ready boundary missing");
            return false;
        }
        static auto runStaticConstructorsHook = safetyhook::create_mid(
            reinterpret_cast<void*>(staticConstructorLoopPattern + 7), [](SafetyHookContext&) {
                _MESSAGE("Native entries registered; notifying providers before native static constructors");
                OnVmMethodsRegistered();
            });
        if (!runStaticConstructorsHook) {
            _MESSAGE("Native initialization hook FAILED: pre-cctor entry-ready midhook");
            return false;
        }
        return true;
    }

    // 上一级 CALL 桥保持原初始化函数体不变，供 RF 反汇编发现 invoke table。
    // 返回后保留既有重复阶段处理；这不是 cctor 前通知的失败替代路径。
    static bool HookInitializeManagedTypes() {
        using namespace TypeInjection;

        // 两个初始化调用后保存返回值并平衡栈；TU5/当前 EXE 全 executable segments 各唯一。
        // 20B 前缀当前有3处，含栈平衡的21B唯一；首个 CALL 必须指向已定位的原初始化入口。
        auto initializeManagedTypesCall = HookUtils::ScanIDAPattern(
            "E8 ? ? ? ? E8 ? ? ? ? 48 89 05 ? ? ? ? 48 83 C4 28");
        if (!initializeManagedTypesCall ||
            HookUtils::ReadOffsetData(initializeManagedTypesCall, 1, 5) != uintptr_t(RE_InitManagedTypes)) {
            throw std::runtime_error("native entry initialization caller CALL signature/target mismatch");
        }
        // RF 从原函数入口反汇编寻找 invoke table。只改上一级 CALL，绝不覆盖该入口/函数体。
        const auto original = g_branchTrampoline.Write5Call(initializeManagedTypesCall, +[]() {
            _MESSAGE("Native entry initialization entered (caller CALL bridge; native body unchanged)");
            RE_InitManagedTypes();
            _MESSAGE("Native entry initialization returned");
            OnVmMethodsRegistered();
        });
        _MESSAGE("Native initialization CALL hook: call=%p, target=%p",
                 reinterpret_cast<void*>(initializeManagedTypesCall), RE_InitManagedTypes);
        return original == RE_InitManagedTypes;
    }

    // 窄字符串和 UTF-16 查询均返回 VM 的 System.RuntimeType 对象。
    static bool HookFindRuntimeTypeNarrow() {
        return HookLambda(RE_FindRuntimeTypeNarrow, [](void* context, const char* name, uint32_t length) -> void* {
            if (auto* type = TypeInjection::FindCustomRuntimeType(std::string_view(name, length))) {
                return type;
            }
            return original(context, name, length);
        });
    }

    static bool HookFindRuntimeTypeWide() {
        return HookLambda(RE_FindRuntimeTypeWide, [](void* context, const wchar_t* name, uint32_t length) -> void* {
            if (g_database) {
                if (auto* type = TypeInjection::FindCustomRuntimeType(RE::narrow(std::wstring_view(name, length)))) {
                    return type;
                }
            }
            return original(context, name, length);
        });
    }

    // 最后启用的 builder hook：首次构建成功返回后处理类型表就绪。
    static bool HookBuildTypeTables() {
        using namespace TypeInjection;

        return HookLambda(RE_BuildTypeTables, [](RE::VM* vm, RE::TDB* tdb) -> uint32_t {
            auto* previousTdb = vm->tdb;
            _MESSAGE("Runtime builder entered: vm=%p, TDB=%p, previous TDB=%p", vm, tdb, previousTdb);
            auto result = original(vm, tdb);
            _MESSAGE("Runtime builder returned: success=%u, metadata published=%d, current TDB=%p",
                     result & 0xFF, bool(g_database), vm->tdb);
            if ((result & 0xFF) && g_database && !previousTdb) {
                OnVmTypeTablesBuilt();
            }
            return result;
        });
    }

    static void InstallHooks() {
        if (!HookInitializeTypeInfo()) {
            throw std::runtime_error("TypeInfo boundary hook failed");
        }
        if (!HookRegisterTypeMethods()) {
            throw std::runtime_error("native type entry installer hook failed");
        }
        // 静态构造前通知与外层返回桥共用阶段处理的 ready 门禁。
        if (!HookRunStaticConstructors() || !HookInitializeManagedTypes()) {
            throw std::runtime_error("native entry initialization completion hook failed");
        }
        if (!HookFindRuntimeTypeNarrow()) {
            throw std::runtime_error("narrow runtime type lookup hook failed");
        }
        if (!HookFindRuntimeTypeWide()) {
            throw std::runtime_error("wide runtime type lookup hook failed");
        }
        // TDB hook 及其余 VM hook 全部成功后，最后接通 builder 中的实际注入流程。
        if (!HookBuildTypeTables()) {
            throw std::runtime_error("runtime builder hook failed");
        }
    }
} // namespace ci::hooks::VM

namespace ci::TypeInjection {
    // 集中解析并保存原生函数入口及 VM 对象指针，不安装 hook。
    static void LookupCoreFunctions() {
        using namespace hooks::VM;
        using namespace hooks::TDB;

        // TU5 与当前 EXE 全映像各唯一一次；同一调用点解析 builder、VM 和启动 TDB。
        auto buildTypeTablesPattern =
            HookUtils::ScanIDAPattern("48 8B 35 ? ? ? ? 48 8D 15 ? ? ? ? 48 89 F1 E8 ? ? ? ? 84 C0");
        if (!buildTypeTablesPattern) {
            throw std::runtime_error("runtime builder call signature not found");
        }
        const auto buildTypeTablesCall = buildTypeTablesPattern + 17;
        auto buildTypeTables =
            reinterpret_cast<FnBuildTypeTables>(HookUtils::ReadOffsetData(buildTypeTablesCall, 1, 5));
        // 复用已验证的 builder caller 签名；unwind 给出外层原生注册函数边界，无新增版本地址。
        DWORD64 imageBase{};
        auto* initializeManagedTypesUnwind = RtlLookupFunctionEntry(
            buildTypeTablesCall, &imageBase, nullptr);
        if (!initializeManagedTypesUnwind ||
            imageBase + initializeManagedTypesUnwind->EndAddress <= buildTypeTablesCall + 5) {
            throw std::runtime_error("native entry initialization caller unwind boundary is missing");
        }
        auto initializeManagedTypes = reinterpret_cast<FnInitializeManagedTypes>(
            imageBase + initializeManagedTypesUnwind->BeginAddress);
        const auto initializeManagedTypesSize =
            initializeManagedTypesUnwind->EndAddress - initializeManagedTypesUnwind->BeginAddress;
        // 从 caller 的参数准备或返回值处理处定位 E8，再解码 rel32 得到函数入口。
        auto registerTypeMethods =
            reinterpret_cast<FnRegisterTypeMethods>(HookUtils::ScanIDAPattern("E8 ? ? ? ? 4B 8B 44 27 E0", 0, 1, 5));
        auto findTypeDefinition = reinterpret_cast<FnFindTypeDefinition>(
            HookUtils::ScanIDAPattern("48 83 C4 10 48 89 F1 48 89 C2 E8", 10, 1, 5));
        auto findRuntimeTypeNarrow = reinterpret_cast<FnFindRuntimeTypeNarrow>(
            HookUtils::ScanIDAPattern("4C 01 D2 48 8B 0D ? ? ? ? E8", 10, 1, 5));
        auto findRuntimeTypeWide = reinterpret_cast<FnFindRuntimeTypeWide>(
            HookUtils::ScanIDAPattern("44 8B 43 04 48 8B 53 08 E8", 8, 1, 5));
        // 闭合泛型查找是 builder 内部调用；短签名只在这个已知 caller 的范围内唯一。
        auto findGenericTypeDefinitionCall = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(buildTypeTables),
            "E8 ? ? ? ? 49 89 C4", 0x400);
        if (!findGenericTypeDefinitionCall) {
            throw std::runtime_error("closed type lookup call signature not found");
        }
        auto findGenericTypeDefinition =
            reinterpret_cast<FnFindGenericTypeDefinition>(HookUtils::ReadOffsetData(findGenericTypeDefinitionCall, 1, 5));
        if (!registerTypeMethods || !findTypeDefinition || !findGenericTypeDefinition || !findRuntimeTypeNarrow ||
            !findRuntimeTypeWide) {
            throw std::runtime_error("required startup signature not found");
        }
        if (!HookUtils::LookupCallee(reinterpret_cast<void*>(initializeManagedTypes),
                                    reinterpret_cast<void*>(registerTypeMethods), false,
                                    initializeManagedTypesSize)) {
            throw std::runtime_error("builder caller does not contain native member entry registration");
        }
        auto relocatePattern = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(buildTypeTables),
            "48 89 F9 E8 ? ? ? ? 84 C0", 0x100);
        if (!relocatePattern) {
            throw std::runtime_error("metadata relocator call signature not found");
        }
        auto relocateCall = relocatePattern + 3;
        auto relocate =
            reinterpret_cast<FnRelocate>(HookUtils::ReadOffsetData(relocateCall, 1, 5));
        auto** vmGlobal = reinterpret_cast<RE::VM**>(HookUtils::ReadOffsetData(buildTypeTablesPattern, 3, 7));
        // 全部入口确认后再发布；后续 hook 安装和回调只读这些函数指针。
        RE_InitManagedTypes = initializeManagedTypes;
        RE_BuildTypeTables = buildTypeTables;
        RE_RegisterTypeMethods = registerTypeMethods;
        RE_FindRuntimeTypeNarrow = findRuntimeTypeNarrow;
        RE_FindRuntimeTypeWide = findRuntimeTypeWide;
        RE_Relocate = relocate;
        RE_FindTypeDefinition = findTypeDefinition;
        RE_FindGenericTypeDefinition = findGenericTypeDefinition;
        g_vmWrapper = vmGlobal;
    }

    // HID 首次 CreateWindowEx 前调用；此时 RF API 尚未初始化，只使用原生定位与 hook。
    // 安装成功不代表类型或方法已经就绪；类型 builder 必须最后接入。
    bool InstallHooks() {
        namespace VM = hooks::VM;
        namespace TDB = hooks::TDB;

        try {
            // 宿主只安装一次；固定近址数据与 CALL 跳板共享池，动态方法入口仍归数据库持有。
            if (!g_branchTrampoline.Create(900)) {
                throw std::runtime_error("type injection trampoline allocation failed");
            }
            const auto gameDirectory = FileUtils::GetExecutableDirectory();
            _MESSAGE("ClassInjector startup: pid=%lu", GetCurrentProcessId());
            // 核对一对一的 TOML / DLL 和整份函数版本；失效插件不会进入 metadata 构建。
            // 独立类型 provider 与兼任 RF 插件的 provider 一起核对，依赖可跨目录引用。
            LoadClassProviders({gameDirectory / L"plugins", gameDirectory / L"reframework/plugins"},
                               g_typeConfigs, g_providerCallbacks);
            // 只提供 ProviderContext 回调的插件同样需要经过完整的原生 TDB 初始化流程。
            if (g_typeConfigs.empty() && g_providerCallbacks.empty()) {
                _MESSAGE("Type injection skipped: no valid type declarations or CI initialization providers in plugins "
                         "or reframework/plugins");
                return false;
            }
            // 1. 集中确认原生入口和 VM 指针槽。
            LookupCoreFunctions();
            // 2. 先安装 TDB hooks，再安装 VM hooks；VM 内部最后接通 builder。
            TDB::InstallHooks();
            VM::InstallHooks();
            _MESSAGE("Startup addresses: builder=%p, relocator=%p, installer=%p, type lookup=%p, native initialization=%p",
                     VM::RE_BuildTypeTables, TDB::RE_Relocate, VM::RE_RegisterTypeMethods, TDB::RE_FindTypeDefinition,
                     VM::RE_InitManagedTypes);
            _MESSAGE("Startup hooks installed; %zu declarations from plugins and reframework/plugins", g_typeConfigs.size());
            return true;
        } catch (const std::exception& error) {
            _MESSAGE("Startup hook installation failed: %s", error.what());
            return false;
        }
    }
} // namespace ci::TypeInjection
