#include "ClassProviderContext.h"
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <HookUtils.h>
#include <LogUtils.h>
#include <cstring>

// 函数入口检查失败时返回默认空值；条件仅求值一次。
#define CI_REQUIRE(condition) \
    do {                      \
        if (!(condition))     \
            return {};        \
    } while (false)

namespace ci {
    static const Type* GetType(const char* name) { return g_database && name ? g_database->GetType(name) : nullptr; }

    static void* GetMethod(const Type* type, const char* name) {
        return g_database && type && name ? g_database->GetMethod(type, name) : nullptr;
    }

    static const FieldLayout* GetFieldLayout(const Type* type, const char* name) {
        return g_database && type && name ? g_database->GetFieldLayout(type, name) : nullptr;
    }

    // 通过宿主 TypeDatabase 提供纯元数据父链查询；尚无 TypeDatabase 时返回 false，不调用 VM。
    static bool IsDerivedFrom(const Type* type, const Type* baseType) {
        return g_database && g_database->IsDerivedFrom(type, baseType);
    }

    // 通过宿主 TypeDatabase 查询枚举分类，不依赖 RF SDK 或原生分配/反射入口。
    static bool IsEnum(const Type* type) {
        return g_database && g_database->IsEnum(type);
    }

    // 元数据查询，无 VM/SDK 依赖；JSON 支持范围仍由调用方决定。
    static uint32_t GetValueTypeSize(const Type* type) {
        return g_database ? g_database->GetValueTypeSize(type) : 0;
    }

    // This is the API table itself, not a duplicate cache of its entries. Only
    // the host's one-shot completion phase fills the object/RSZ slots; providers
    // borrow the process-lifetime const table after that phase has completed.
    static ProviderContext context{.GetType = &GetType, .GetMethod = &GetMethod, .GetFieldLayout = &GetFieldLayout,
                           .IsDerivedFrom = &IsDerivedFrom, .IsEnum = &IsEnum, .GetValueTypeSize = &GetValueTypeSize};

    const ProviderContext& GetContext() { return context; }

    using FnCreateActionEntry = void*(__cdecl*)(void*, const Type*, void*, const MethodDef*, bool);
    // 委托工厂需要 MethodDef 而非函数地址；仅此适配器跨调用保存入口。
    static FnCreateActionEntry CreateActionEntry{};
    using FnFastAllocateString = RE::SystemString*(__cdecl*)(void*, int32_t);
    static FnFastAllocateString FastAllocateString{};
    using FnCreateUserDataResource = void**(__cdecl*)(void*, void**, void*, const wchar_t*);
    static FnCreateUserDataResource CreateUserDataResource{};
    static void** resourceManagerSlot{};
    using FnCreateResource = void*(__cdecl*)(void*, void*, const wchar_t*);
    static FnCreateResource CreateResource{};
    static void** resourceTypeInfoSlot{};

    // 通过原生 USER 请求入口创建外部资源引用，路径复用 common 的严格 UTF-8 转换。
    static void* CreateUserData(const Type* type, const char* utf8Path) {
        CI_REQUIRE(*resourceManagerSlot);
        CI_REQUIRE(type);
        CI_REQUIRE(type->typeInfo);
        CI_REQUIRE(utf8Path);
        CI_REQUIRE(*utf8Path);

        const auto path = RE::widen(utf8Path, MB_ERR_INVALID_CHARS);
        if (path.empty())
            return nullptr;
        void* object{};
        CreateUserDataResource(*resourceManagerSlot, &object, type->typeInfo, path.c_str());
        return object;
    }

    static const Type* GetInstanceType(const void* object) noexcept {
        return object ? **static_cast<const Type* const* const*>(object) : nullptr;
    }

    static void* GetRawField(void* object, const FieldLayout* field) noexcept {
        CI_REQUIRE(object);
        CI_REQUIRE(field);
        // Closed generic instances can differ from the type's default ObjectInfo.
        const auto info = *static_cast<const uintptr_t*>(object);
        const auto base = *reinterpret_cast<const int32_t*>(info - sizeof(void*));
        return static_cast<uint8_t*>(object) + base + field->valueOffset;
    }

    // 分配指定 holder 并交接原生资源请求引用；路径转换不在 CI 内重复实现。
    static void* CreateResourceHolder(void* vmctx, const Type* type, const char* utf8Path) {
        const auto* holderType = context.GetType("via.ResourceHolder");
        CI_REQUIRE(g_database);
        CI_REQUIRE(vmctx);
        CI_REQUIRE(type);
        CI_REQUIRE(holderType);
        CI_REQUIRE(g_database->IsAssignable(uint32_t(type->index), uint32_t(holderType->index)));
        CI_REQUIRE(*resourceTypeInfoSlot);
        CI_REQUIRE(*resourceManagerSlot);
        CI_REQUIRE(utf8Path);
        CI_REQUIRE(*utf8Path);

        const auto path = RE::widen(utf8Path, MB_ERR_INVALID_CHARS);
        if (path.empty())
            return nullptr;
        auto* holder = context.CreateInstance(vmctx, type, false);
        if (!holder)
            return nullptr;
        // Native CLR resource-field decoder stores Resource* at holder+0x10.
        // Transfer the request's owned native reference to this fresh holder;
        // its native finalizer releases it. This is not a managed reference.
        auto** resource = reinterpret_cast<void**>(static_cast<uint8_t*>(holder) + 0x10);
        if (*resource)
            return nullptr;
        *resource = CreateResource(*resourceManagerSlot, *resourceTypeInfoSlot, path.c_str());
        return *resource ? holder : nullptr;
    }

    static bool AssignReference(void* owner, void** field, void* value) noexcept {
        // PrepareContext 只在两个入口均已解析时发布本函数，通知后表保持不变。
        using Object = reframework::API::ManagedObject;
        using FnObjectReference = void (*)(Object*);
        // Reuse the same negative-refcount and retain/CAS/release implementation.
        // No RF SDK lookup and no newly entered managed call scope.
        return RE::set_managed_field(static_cast<Object*>(owner), reinterpret_cast<Object**>(field),
                                     static_cast<Object*>(value), reinterpret_cast<FnObjectReference>(context.Retain),
                                     reinterpret_cast<FnObjectReference>(context.Release));
    }

    static void* CreateAction(void* vmctx, const Type* actionType, void* target, const Type* methodType,
                              const char* signature, bool forceHeapAllocation) {
        CI_REQUIRE(g_database);
        CI_REQUIRE(vmctx);
        CI_REQUIRE(actionType);
        CI_REQUIRE(methodType);
        CI_REQUIRE(signature);

        const auto* method = g_database->GetMethodDef(methodType, signature);
        if (!method || !method->encodedOffset)
            return nullptr;
        return CreateActionEntry(vmctx, actionType, target, method, forceHeapAllocation);
    }

    // 接收 NUL 结尾的 UTF-8，复用 common 转换后分配并填充托管字符串。
    static void* CreateString(void* vmctx, const char* utf8) {
        CI_REQUIRE(vmctx);
        CI_REQUIRE(utf8);

        const auto text = RE::widen(utf8, MB_ERR_INVALID_CHARS);
        // 空输入应创建空字符串；非空输入转换为空则表示失败。
        if (*utf8 && text.empty())
            return nullptr;
        const auto units = static_cast<int32_t>(text.size());
        auto* result = FastAllocateString(vmctx, units);
        if (!result || result->size != units)
            return nullptr;
        std::memcpy(result->data, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        return result;
    }

    void PrepareContext() {
        context.GetInstanceType = &GetInstanceType;
        context.GetRawField = &GetRawField;

        // 真实引用 setter：retain 是 CALL，release 是退出路径的尾 JMP。
        // TDB 已限定调用者，不再扫描整幅映像的函数头。
        const auto* countedGameObjectHolder = context.GetType("app.cCountedGameObjectHolder");
        const auto set_Body =
            countedGameObjectHolder ? context.GetMethod(countedGameObjectHolder, "set_Body(via.GameObject)") : nullptr;
        const auto set_BodyRetainCall =
            set_Body ? HookUtils::LookupFunctionPattern(set_Body, "E8 ? ? ? ? 48 8B", 0x50) : 0;
        const auto set_BodyReleaseJump =
            set_Body ? HookUtils::LookupFunctionPattern(set_Body, "E9 ? ? ? ? 48 89", 0x50) : 0;
        const auto Retain = set_BodyRetainCall ? HookUtils::ReadOffsetData(set_BodyRetainCall, 1, 5) : 0;
        const auto Release = set_BodyReleaseJump ? HookUtils::ReadOffsetData(set_BodyReleaseJump, 1, 5) : 0;
        const bool hasReferenceEntries = Retain && Release && Retain != Release;
        context.Retain = hasReferenceEntries ? reinterpret_cast<FnRetain>(Retain) : nullptr;
        context.Release = hasReferenceEntries ? reinterpret_cast<FnRelease>(Release) : nullptr;
        context.AssignReference = hasReferenceEntries ? &AssignReference : nullptr;

        const auto* stringType = context.GetType("System.String");
        const auto FastAllocateStringEntry =
            stringType ? context.GetMethod(stringType, "FastAllocateString(System.Int32)") : nullptr;
        FastAllocateString = reinterpret_cast<FnFastAllocateString>(FastAllocateStringEntry);
        context.CreateString = FastAllocateString ? &CreateString : nullptr;

        // 沿用 common/src/GameTypes.cpp 的 RE::create_managed_action
        // 调用点，准备期不依赖 RF SDK。
        const auto* partsBoxType = context.GetType("app.GUI030205PartsBox");
        const auto setupControl = partsBoxType ? context.GetMethod(partsBoxType, "setupControl()") : nullptr;
        const auto setupControlCreateActionCallSetup =
            setupControl ? HookUtils::LookupFunctionPattern(setupControl, "49 89 F8 E8", 0x80) : 0;
        CreateActionEntry = setupControlCreateActionCallSetup
                                ? reinterpret_cast<FnCreateActionEntry>(
                                      HookUtils::ReadOffsetData(setupControlCreateActionCallSetup + 3, 1, 5))
                                : nullptr;
        context.CreateAction = CreateActionEntry ? &CreateAction : nullptr;
        _MESSAGE("ProviderContext CreateAction entry=%p", CreateActionEntry);
        _MESSAGE("ProviderContext object functions: retain=%p release=%p FastAllocateString=%p", Retain, Release,
                 FastAllocateString);

        const auto rszMagicCompare = HookUtils::ScanIDAPattern("81 3B 52 53 5A 00");
        DWORD64 imageBase{};
        const auto* unwind = rszMagicCompare ? RtlLookupFunctionEntry(rszMagicCompare, &imageBase, nullptr) : nullptr;
        const auto DeserializeRsz = unwind ? imageBase + unwind->BeginAddress : 0;
        // RSZ 的实例分配点：RCX=当前 VM context，RDX=TDB Type，R8B=false。
        // TypeInfo 的 slot 1 只是 native placement 工厂；单独调用会留下 C++
        // vtable 而非托管 ObjectInfo，也绕过 CLR 的对象头/临时帧初始化。
        const auto createInstanceCallSetup =
            DeserializeRsz ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(DeserializeRsz),
                                                              "48 89 F1 45 31 C0 E8", 0x400)
                           : 0;
        const auto CreateInstance =
            createInstanceCallSetup ? HookUtils::ReadOffsetData(createInstanceCallSetup + 6, 1, 5) : 0;
        context.CreateInstance = reinterpret_cast<FnCreateInstance>(CreateInstance);
        const auto decodeFieldsCallSetup =
            DeserializeRsz
                ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(DeserializeRsz), "4D 89 F9 E8", 0x700)
                : 0;
        const auto decodeFields =
            decodeFieldsCallSetup ? HookUtils::ReadOffsetData(decodeFieldsCallSetup + 3, 1, 5) : 0;

        // Guid.ToByteArray's first CALL is the VM-context getter, not allocation.
        // Resolve the four-argument allocator from the actual array field decoder.
        const auto createArrayCallSetup =
            decodeFields ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(decodeFields), "45 31 C9 E8", 0x400)
                         : 0;
        const auto CreateArray = createArrayCallSetup ? HookUtils::ReadOffsetData(createArrayCallSetup + 3, 1, 5) : 0;
        context.DeserializeRsz = reinterpret_cast<FnDeserializeRsz>(DeserializeRsz);
        context.CreateArray = reinterpret_cast<FnCreateArray>(CreateArray);
        // Same external USER request as the native RSZ reader. Keep the global
        // slot's address, not its possibly uninitialized pre-cctor value.
        const auto userDataManagerLoad = DeserializeRsz
                                             ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(DeserializeRsz),
                                                                                "48 8B 2D ? ? ? ? E8", 0x400)
                                             : 0;
        const auto createUserDataCallSetup =
            DeserializeRsz
                ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(DeserializeRsz), "4D 89 E9 E8", 0x400)
                : 0;
        resourceManagerSlot = userDataManagerLoad
                                  ? reinterpret_cast<void**>(HookUtils::ReadOffsetData(userDataManagerLoad, 3, 7))
                                  : nullptr;
        CreateUserDataResource = createUserDataCallSetup
                                     ? reinterpret_cast<FnCreateUserDataResource>(
                                           HookUtils::ReadOffsetData(createUserDataCallSetup + 3, 1, 5))
                                     : nullptr;
        context.CreateUserData = resourceManagerSlot && CreateUserDataResource ? &CreateUserData : nullptr;
        const auto createResourceCallSetup =
            decodeFields ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(decodeFields), "49 89 F8 E8", 0x800)
                         : 0;
        const bool hasResourceTypeLoad =
            createResourceCallSetup &&
            memcmp(reinterpret_cast<const void*>(createResourceCallSetup - 7), "\x48\x8B\x15", 3) == 0;
        resourceTypeInfoSlot =
            hasResourceTypeLoad ? reinterpret_cast<void**>(HookUtils::ReadOffsetData(createResourceCallSetup - 7, 3, 7))
                                : nullptr;
        CreateResource =
            hasResourceTypeLoad
                ? reinterpret_cast<FnCreateResource>(HookUtils::ReadOffsetData(createResourceCallSetup + 3, 1, 5))
                : nullptr;
        context.CreateResourceHolder =
            context.CreateInstance && resourceManagerSlot && resourceTypeInfoSlot && CreateResource
                ? &CreateResourceHolder
                : nullptr;
        _MESSAGE("ProviderContext resource requests: CreateUserData=%p CreateResource=%p", CreateUserDataResource,
                 CreateResource);
        _MESSAGE("ProviderContext RSZ functions: DeserializeRsz=%p CreateInstance=%p "
                 "CreateArray=%p",
                 DeserializeRsz, CreateInstance, CreateArray);
    }

} // namespace ci

#undef CI_REQUIRE
