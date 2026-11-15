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
    // TypeDatabase / runtimeTypesReady / g_ready 分别表示 metadata、形状、原生入口注册完成。
    std::unique_ptr<TypeDatabase> g_database;
    std::atomic_bool g_ready{};

    // 仅首次 builder 的原函数执行期间有效，供其内部触发的 relocator hook 识别所属 VM。
    static RE::VM* g_initializingVm{};
    // 原名称搜索仍使用扩表前的数量；近址存储使原 RIP-relative 指令可直接引用它。
    static uint32_t* g_originalTypeCountStorage{};
    static std::vector<CustomTypeConfig> g_declarations;
    static std::vector<ClassProviderNotification> g_tdbReadyCallbacks;

    using FnBuildRuntimeTables = uint32_t (*)(RE::VM*, RE::TDB*);
    using FnInitializeRuntimeEntries = void (*)();
    using FnRelocateMetadata = uint32_t (*)(RE::TDB*);
    using FnInstallTypeEntries = void (*)(RE::VM*, RE::RETypeDefinition**, uint32_t, void*, void**, uint32_t, void**, uint32_t, void*);
    using FnFindTypeByName = RE::RETypeDefinition* (*)(void*, const char*);
    using FnFindClosedType = RE::RETypeDefinition* (*)(void*, uint32_t, uint32_t);
    using FnFindRuntimeTypeAscii = void* (*)(void*, const char*, uint32_t);
    using FnFindRuntimeTypeWide = void* (*)(void*, const wchar_t*, uint32_t);

    static FnInitializeRuntimeEntries g_initializeRuntimeEntriesOriginal{};
    // 新记录按依赖顺序追加，不满足原名称搜索的排序/分区规则。
    // 将原函数的三个数量引用指向旧上界；新类型名字由下方查找 hook 单独处理。
    static void LimitOriginalNameLookupRange(uintptr_t findTypeByName, uintptr_t countAddress) {
        DWORD64 base;
        auto* unwind = RtlLookupFunctionEntry(findTypeByName, &base, nullptr);
        if (!unwind) {
            throw std::runtime_error("name lookup unwind entry is missing");
        }
        auto scanSize = uint32_t(base + unwind->EndAddress - findTypeByName);
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
        // 三条指令都是 6 字节，disp32 位于 +2；全部确认后再写入，避免只改了一部分。
        for (size_t i = 0; i < signatures.size(); ++i) {
            auto instruction =
                HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(findTypeByName), signatures[i], scanSize);
            if (!instruction || HookUtils::ReadOffsetData(instruction, 2, 6) != countAddress) {
                throw std::runtime_error("name lookup count reference signature mismatch");
            }
            auto difference = int64_t(uintptr_t(g_originalTypeCountStorage)) - int64_t(instruction + 6);
            if (difference < INT32_MIN || difference > INT32_MAX) {
                throw std::runtime_error("name bound storage is outside RIP-relative range");
            }
            references[i] = {.displacement = instruction + 2, .value = int32_t(difference)};
        }
        for (auto& reference : references) {
            HookUtils::SafeWrite32(reference.displacement, uint32_t(reference.value));
        }
    }

    // 返回 builder 创建的 System.RuntimeType 对象，而非 TDB 的 RETypeDefinition 记录。
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


    // 输入是已完成 relocation 的原 TDB。先离线构建全部表和 binding，再修改引擎表指针。
    // Publish 后原 builder 会继续消费新表，因此 TypeDatabase 的存储保留到进程结束。
    static void BuildAndPublishTypeDatabase(RE::TDB& tdb, RE::VM& vm, uintptr_t methodAddressBase,
                                            uintptr_t findTypeByName, uintptr_t typeCountAddress) {
        try {
            if (tdb.version != 81) {
                throw std::runtime_error("expected TDB81");
            }
            auto database = BuildTypeDatabase(tdb, std::move(g_declarations));
            database->encodingBase = methodAddressBase;
            database->vm = &vm;
            CreateMethodTrampolines(*database);
            PrepareInvokeAdapters(*database);
            if (database->invokeDispatchId && !InstallInvokeDispatch(*database, reinterpret_cast<void*>(g_initializeRuntimeEntriesOriginal))) {
                throw std::runtime_error("invoke dispatch installation failed");
            }
            if (!database->customTypes.empty()) {
                LimitOriginalNameLookupRange(findTypeByName, typeCountAddress);
            }
            g_database = std::move(database);
            g_database->Publish(tdb);
            _MESSAGE("Published metadata: %u -> %u types, %zu custom methods", g_database->originalCount,
                     tdb.numTypes, g_database->customMethods.size());
        } catch (const std::exception& error) {
            _MESSAGE("Metadata injection failed before publish: %s", error.what());
        }
    }



    // builder 返回仅保证 TypeInfo / ObjectInfo。原生方法由外层随后注册，不能在这里通知。
    bool FinalizeRuntimeTypes() {
        if (!g_database) {
            _MESSAGE("Runtime shapes FAILED: metadata was not published");
            return false;
        }
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

    bool CompleteRuntimeInitialization(std::vector<ClassProviderNotification>& callbacks) {
        if (g_ready.load(std::memory_order_acquire)) {
            return true;
        }
        if (!g_database || !g_database->runtimeTypesReady) {
            _MESSAGE("Runtime entries FAILED: metadata=%d shapes ready=%d; ready remains false; providers not notified",
                     bool(g_database), g_database && g_database->runtimeTypesReady);
            return false;
        }
        // 此时原生entry注册循环均已返回，但全局cctor尚未开始；同步继承入口后通知代码适配。
        for (const auto& type : g_database->customTypes) UpdateVirtualTable(*g_database, type);
        PrepareContext();
        _MESSAGE("Runtime entries ready; notifying providers=%zu", callbacks.size());
        g_ready.store(true, std::memory_order_release);
        NotifyClassProviders(callbacks, GetContext());
        _MESSAGE("TDB-ready notifications complete; injection ready=1");
        return true;
    }

    static bool InstallPreStaticConstructorNotification(void (*initializeRuntimeEntries)()) {
        if (!initializeRuntimeEntries) return false;
        // 原函数尾部按表执行全局cctor，EnemyUtil等静态数组在此被构造。
        // 在外层CALL返回后通知已太晚：即使改了cctor机器码，现有Type[]仍只有150。
        // 只在已核实的cctor循环之前通知，不覆盖RF扫描的入口/原生entry注册循环。
        DWORD64 initializationBase{};
        auto* initializationUnwind = RtlLookupFunctionEntry(reinterpret_cast<uintptr_t>(initializeRuntimeEntries), &initializationBase, nullptr);
        const auto initializationSize = initializationUnwind
            ? uint32_t(initializationBase + initializationUnwind->EndAddress - reinterpret_cast<uintptr_t>(initializeRuntimeEntries)) : 0;
        const auto staticConstructorLoop = initializationSize ? HookUtils::LookupFunctionPattern(
            reinterpret_cast<void*>(initializeRuntimeEntries),
            "C6 05 ? ? ? ? 00 31 DB 4C 8D 35 ? ? ? ? 48 89 F9 42 FF 14 33", initializationSize) : 0;
        if (!staticConstructorLoop) {
            _MESSAGE("Native initialization hook FAILED: pre-cctor entry-ready boundary missing");
            return false;
        }
        static auto beforeStaticConstructorsHook = safetyhook::create_mid(
            reinterpret_cast<void*>(staticConstructorLoop + 7), [](SafetyHookContext&) {
                _MESSAGE("Native entries registered; notifying providers before native static constructors");
                CompleteRuntimeInitialization(g_tdbReadyCallbacks);
            });
        if (!beforeStaticConstructorsHook) {
            _MESSAGE("Native initialization hook FAILED: pre-cctor entry-ready midhook");
            return false;
        }
        return true;
    }

    // Consume the startup entry's shared trampoline pool without recreating its storage.
    bool InstallRuntimeInitializationCall(uintptr_t initializeRuntimeEntriesCall, void (*initializeRuntimeEntries)()) {
        if (!initializeRuntimeEntriesCall || !initializeRuntimeEntries || *reinterpret_cast<const uint8_t*>(initializeRuntimeEntriesCall) != 0xE8 ||
            HookUtils::ReadOffsetData(initializeRuntimeEntriesCall, 1, 5) != uintptr_t(initializeRuntimeEntries) || g_initializeRuntimeEntriesOriginal) {
            _MESSAGE("Native initialization CALL hook FAILED: call=%p expected=%p original=%p",
                     reinterpret_cast<void*>(initializeRuntimeEntriesCall), initializeRuntimeEntries, g_initializeRuntimeEntriesOriginal);
            return false;
        }
        // RF 从原函数入口反汇编寻找 invoke table。只改上一级 CALL，绝不覆盖该入口/函数体。
        g_initializeRuntimeEntriesOriginal = g_branchTrampoline.Write5Call(initializeRuntimeEntriesCall, +[]() {
            _MESSAGE("Native entry initialization entered (caller CALL bridge; native body unchanged)");
            g_initializeRuntimeEntriesOriginal();
            _MESSAGE("Native entry initialization returned");
            CompleteRuntimeInitialization(g_tdbReadyCallbacks);
        });
        return g_initializeRuntimeEntriesOriginal == initializeRuntimeEntries;
    }

    // 由 HIDWrapper 在首次 CreateWindowEx 前调用，此时 REFramework API 尚未初始化。
    // 只用原生扫描/hook 接入首次 VM 构建，不在已运行的 VM 上重放初始化。
    // 成功返回仅表示 hook 已就绪；ready 要等 builder/relocator 与外层原生 entry 注册均完成。
    bool InstallTypeInjectionHooks() {
        try {
            // 宿主只安装一次；固定近址数据与 CALL 跳板共享池，动态方法入口仍归数据库持有。
            if (!g_branchTrampoline.Create(900)) {
                throw std::runtime_error("type injection trampoline allocation failed");
            }
            const auto gameDirectory = FileUtils::GetExecutableDirectory();
            _MESSAGE("ClassInjector startup: pid=%lu", GetCurrentProcessId());
            // 核对一对一的 TOML / DLL 和整份函数版本；失效插件不会进入 metadata 构建。
            // 独立类型 provider 与兼任 RF 插件的 provider 一起核对，依赖可跨目录引用。
            g_declarations = LoadClassProviders({gameDirectory / L"plugins", gameDirectory / L"reframework/plugins"}, &g_tdbReadyCallbacks);
            // 只提供 ProviderContext 回调的插件同样需要经过完整的原生 TDB 初始化流程。
            if (g_declarations.empty() && g_tdbReadyCallbacks.empty()) {
                _MESSAGE("Type injection skipped: no valid type declarations or TDB-ready providers in plugins or reframework/plugins");
                return false;
            }
            // 1. 找到首次 builder 调用和 VM 全局槽位，并确认尚未错过 VM 初始化窗口。
            // TU5 与当前 EXE 全映像各唯一一次；同一调用点解析 builder、VM 和启动 TDB。
            auto buildRuntimeTablesCall =
                HookUtils::ScanIDAPattern("48 8B 35 ? ? ? ? 48 8D 15 ? ? ? ? 48 89 F1 E8 ? ? ? ? 84 C0");
            if (!buildRuntimeTablesCall) {
                throw std::runtime_error("runtime builder call signature not found");
            }
            auto buildRuntimeTables =
                reinterpret_cast<FnBuildRuntimeTables>(HookUtils::ReadOffsetData(buildRuntimeTablesCall + 17, 1, 5));
            // 复用已验证的 builder caller 签名；unwind 给出外层原生注册函数边界，无新增版本地址。
            DWORD64 initializeRuntimeEntriesBase{};
            auto* initializeRuntimeEntriesUnwind = RtlLookupFunctionEntry(
                buildRuntimeTablesCall + 17, &initializeRuntimeEntriesBase, nullptr);
            if (!initializeRuntimeEntriesUnwind ||
                initializeRuntimeEntriesBase + initializeRuntimeEntriesUnwind->EndAddress <= buildRuntimeTablesCall + 22) {
                throw std::runtime_error("native entry initialization caller unwind boundary is missing");
            }
            auto initializeRuntimeEntries = reinterpret_cast<FnInitializeRuntimeEntries>(
                initializeRuntimeEntriesBase + initializeRuntimeEntriesUnwind->BeginAddress);
            // 两个初始化调用后保存返回值并平衡栈；TU5/当前 EXE 全 executable segments 各唯一。
            // 20B 前缀当前有3处，含栈平衡的21B唯一；再校验第一个 CALL 必须指向上述 unwind 函数。
            auto initializeRuntimeEntriesCall = HookUtils::ScanIDAPattern(
                "E8 ? ? ? ? E8 ? ? ? ? 48 89 05 ? ? ? ? 48 83 C4 28");
            if (!initializeRuntimeEntriesCall ||
                HookUtils::ReadOffsetData(initializeRuntimeEntriesCall, 1, 5) != uintptr_t(initializeRuntimeEntries)) {
                throw std::runtime_error("native entry initialization caller CALL signature/target mismatch");
            }
            auto** vmAddress = reinterpret_cast<RE::VM**>(HookUtils::ReadOffsetData(buildRuntimeTablesCall, 3, 7));
            auto* vm = *vmAddress;
            auto* currentTdb = vm ? vm->tdb : nullptr;
            auto runtimeTypeCount = vm ? vm->runtimeTypeCount : 0;
            _MESSAGE("Bootstrap state: builder=%p, vm=%p, current TDB=%p, RuntimeType count=%llu", buildRuntimeTables,
                     vm, currentTdb, runtimeTypeCount);
            if (currentTdb) {
                _MESSAGE("Startup window missed: VM already has a TDB before bootstrap hooks; load this DLL before the "
                         "first runtime-table builder entry");
                return false; // 不在已运行的 VM 上重跑整套 builder。
            }
            // 2. 定位运行时表安装、名称/泛型/RuntimeType 查找及 TypeInfo 初始化分区的入口。
            // 从 caller 的参数准备或返回值处理处定位 E8，再解码 rel32 得到函数入口。
            auto installTypeEntries =
                reinterpret_cast<FnInstallTypeEntries>(HookUtils::ScanIDAPattern("E8 ? ? ? ? 4B 8B 44 27 E0", 0, 1, 5));
            auto findTypeByName = reinterpret_cast<FnFindTypeByName>(
                HookUtils::ScanIDAPattern("48 83 C4 10 48 89 F1 48 89 C2 E8", 10, 1, 5));
            auto findRuntimeTypeAscii = reinterpret_cast<FnFindRuntimeTypeAscii>(
                HookUtils::ScanIDAPattern("4C 01 D2 48 8B 0D ? ? ? ? E8", 10, 1, 5));
            auto findRuntimeTypeWide = reinterpret_cast<FnFindRuntimeTypeWide>(
                HookUtils::ScanIDAPattern("44 8B 43 04 48 8B 53 08 E8", 8, 1, 5));
            // 闭合泛型查找是 builder 内部调用；短签名只在这个已知 caller 的范围内唯一。
            auto findClosedTypeCall = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(buildRuntimeTables),
                                                                       "E8 ? ? ? ? 49 89 C4", 0x400);
            if (!findClosedTypeCall) {
                throw std::runtime_error("closed type lookup call signature not found");
            }
            auto findClosedType =
                reinterpret_cast<FnFindClosedType>(HookUtils::ReadOffsetData(findClosedTypeCall, 1, 5));
            // 保留 CMP 起点，后续全局量解码 (+6) 和 mid-hook (+12) 的位置不变。
            auto typeInfoBoundary = HookUtils::ScanIDAPattern("69 C9 CD CC CC CC 39 0D ? ? ? ?");
            if (!installTypeEntries || !findTypeByName || !findClosedType || !findRuntimeTypeAscii ||
                !findRuntimeTypeWide || !typeInfoBoundary) {
                throw std::runtime_error("required startup signature not found");
            }
            if (!HookUtils::LookupCallee(reinterpret_cast<void*>(initializeRuntimeEntries),
                                        reinterpret_cast<void*>(installTypeEntries), false,
                                        initializeRuntimeEntriesUnwind->EndAddress - initializeRuntimeEntriesUnwind->BeginAddress)) {
                throw std::runtime_error("builder caller does not contain native member entry registration");
            }
            // 分区边界与总数量是相邻但不同的全局值。旧分区规则保留，总数量由新 TDB 扩展。
            auto boundaryAddress = HookUtils::ReadOffsetData(typeInfoBoundary + 6, 2, 6);
            auto relocateMetadataPattern = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(buildRuntimeTables),
                                                                            "48 89 F9 E8 ? ? ? ? 84 C0", 0x100);
            if (!relocateMetadataPattern) {
                throw std::runtime_error("metadata relocator call signature not found");
            }
            auto relocateMetadataCall = relocateMetadataPattern + 3;
            auto relocateMetadata =
                reinterpret_cast<FnRelocateMetadata>(HookUtils::ReadOffsetData(relocateMetadataCall, 1, 5));
            auto encodingBaseLoad = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(installTypeEntries),
                                                                     "4C 8D 35 ? ? ? ? 45 31 FF", 0x140);
            if (!encodingBaseLoad) {
                throw std::runtime_error("method encoding base load is missing");
            }
            // 宿主只安装一次；这些定位事实仅供 relocator 回调消费，校验后只读。
            // MethodDef::encodedOffset 以 methodAddressBase 为基准，不是裸函数指针。
            static const auto methodAddressBase = HookUtils::ReadOffsetData(encodingBaseLoad, 3, 7);
            static const auto typeNameLookupAddress = uintptr_t(findTypeByName);
            static const auto typeCountAddress = boundaryAddress - 4;

            g_originalTypeCountStorage = static_cast<uint32_t*>(g_branchTrampoline.Allocate(sizeof(uint32_t)));
            *g_originalTypeCountStorage = *reinterpret_cast<uint32_t*>(boundaryAddress - 4);

            // 3. 仅放行追加的新 ID，不把原先被分区判断排除的既有类型一起放行。
            static auto typeInfoBoundaryHook =
                safetyhook::create_mid(reinterpret_cast<void*>(typeInfoBoundary + 12), [](SafetyHookContext& context) {
                    if (g_database && uint32_t(context.rcx) >= g_database->originalCount &&
                        uint32_t(context.rcx) < g_database->types.size()) {
                        // 清除 CF / ZF，让后面的 JBE 不跳转，继续初始化这个新增类型。
                        context.rflags &= ~uintptr_t(0x41);
                    }
                });
            if (!typeInfoBoundaryHook) {
                throw std::runtime_error("TypeInfo boundary hook failed");
            }

            // 4. 先让原 relocator 把相对表地址转换完，再发布扩展表；此时 builder 尚未建运行时类型。
            if (!HookLambda(relocateMetadata, [](RE::TDB* tdb) -> uint32_t {
                auto result = original(tdb);
                _MESSAGE("Metadata relocated: success=%u, TDB=%p, version=%u, types=%u, initializing VM=%p",
                         result & 0xFF, tdb, tdb->version, tdb->numTypes, g_initializingVm);
                if (!(result & 0xFF) || !g_initializingVm || g_database) {
                    return result;
                }
                *g_originalTypeCountStorage = tdb->numTypes;
                if (g_declarations.empty() && g_tdbReadyCallbacks.empty()) {
                    _MESSAGE("Metadata injection skipped: no type declarations or TDB-ready providers loaded");
                    return result;
                }
                BuildAndPublishTypeDatabase(*tdb, *g_initializingVm, methodAddressBase,
                                            typeNameLookupAddress, typeCountAddress);
                return result;
            })) {
                throw std::runtime_error("metadata relocator hook failed");
            }
            // 5. 接入原生入口安装与完成通知；父类入口安装后重新同步自定义子类。
            if (!HookLambda(installTypeEntries, [](RE::VM* vm, RE::RETypeDefinition** out, uint32_t index, void* descriptor,
                                              void** virtuals, uint32_t virtualCount, void** members,
                                              uint32_t memberCount, void* auxiliary) {
                original(vm, out, index, descriptor, virtuals, virtualCount, members, memberCount, auxiliary);
                if (g_database && g_database->runtimeTypesReady) {
                    for (auto& type : g_database->customTypes) {
                        if (type.parent == index) {
                            for (const auto& child : g_database->customTypes) UpdateVirtualTable(*g_database, child);
                            break;
                        }
                    }
                }
            })) {
                throw std::runtime_error("native type entry installer hook failed");
            }
            // 保持原生入口和注册循环完整；唯一函数内通知点在末尾cctor循环之前。
            if (!InstallPreStaticConstructorNotification(initializeRuntimeEntries) ||
                !InstallRuntimeInitializationCall(initializeRuntimeEntriesCall, initializeRuntimeEntries)) {
                throw std::runtime_error("native entry initialization completion hook failed");
            }
            // 6. 追加类型不进入原有排序搜索：新名字查 TypeDatabase，其余调用仍交给原函数。
            if (!HookLambda(findTypeByName, [](void* context, const char* name) -> RE::RETypeDefinition* {
                if (g_database) {
                    if (auto* type = g_database->FindType(name)) {
                        return &g_database->types[type->index];
                    }
                }
                return original(context, name);
            })) {
                throw std::runtime_error("type name lookup hook failed");
            }
            // 闭合泛型身份同时包含原 generic definition hash 和当前实例名称 hash。
            if (!HookLambda(findClosedType,
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
                       })) {
                throw std::runtime_error("closed type lookup hook failed");
            }
            // 反射名称查询要返回 VM 的 RuntimeType 对象；ASCII / UTF-16 两条入口使用同一查找逻辑。
            if (!HookLambda(findRuntimeTypeAscii, [](void* context, const char* name, uint32_t length) -> void* {
                if (auto* type = FindCustomRuntimeType(std::string_view(name, length))) {
                    return type;
                }
                return original(context, name, length);
            })) {
                throw std::runtime_error("ASCII runtime type lookup hook failed");
            }
            if (!HookLambda(findRuntimeTypeWide, [](void* context, const wchar_t* name, uint32_t length) -> void* {
                if (g_database) {
                    if (auto* type = FindCustomRuntimeType(RE::narrow(std::wstring_view(name, length)))) {
                        return type;
                    }
                }
                return original(context, name, length);
            })) {
                throw std::runtime_error("wide runtime type lookup hook failed");
            }
            // 引擎会长期保存 hook、metadata 和入口地址，host DLL 与这些地址一起保留到进程结束。
            HMODULE pinned;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&InstallTypeInjectionHooks), &pinned);
            // 7. 最后安装 builder hook，才允许标记当前 VM 并由内部 relocator 发布新数据库。
            // 前面的安装失败时不会进入这一步；无需另存“所有 hook 已就绪”的状态。
            // 原函数返回后才有 TypeInfo / ObjectInfo，可以安装托管方法入口。
            if (!HookLambda(buildRuntimeTables, [](RE::VM* vm, RE::TDB* tdb) -> uint32_t {
                auto* previous = vm->tdb;
                _MESSAGE("Runtime builder entered: vm=%p, TDB=%p, previous TDB=%p", vm, tdb, previous);
                if (!previous && !g_database) {
                    g_initializingVm = vm;
                } else if (!g_database) {
                    _MESSAGE("Metadata injection skipped: existing TDB (reload is not supported)");
                }
                auto result = original(vm, tdb);
                g_initializingVm = nullptr;
                _MESSAGE("Runtime builder returned: success=%u, metadata published=%d", result & 0xFF,
                         bool(g_database));
                if ((result & 0xFF) && g_database && !previous) {
                    FinalizeRuntimeTypes();
                }
                return result;
            })) {
                throw std::runtime_error("runtime builder hook failed");
            }
            _MESSAGE("Startup addresses: builder=%p, relocator=%p, installer=%p, type lookup=%p, method base=%p, native initialization=%p, caller CALL=%p",
                     buildRuntimeTables, relocateMetadata, installTypeEntries, findTypeByName,
                     reinterpret_cast<void*>(methodAddressBase), initializeRuntimeEntries, reinterpret_cast<void*>(initializeRuntimeEntriesCall));
            _MESSAGE("Startup hooks installed; %zu declarations from plugins and reframework/plugins", g_declarations.size());
            return true;
        } catch (const std::exception& error) {
            _MESSAGE("Startup hook installation failed: %s", error.what());
            return false;
        }
    }

} // namespace ci
