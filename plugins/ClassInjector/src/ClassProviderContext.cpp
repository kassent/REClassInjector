#include "ClassProviderContext.h"
#include "ProviderMetadataAPI.h"
#include "ProviderRuntimeAPI.h"
#include "TypeDatabase.h"
#include <HookUtils.h>
#include <LogUtils.h>
#include <cstring>

namespace ci {
    static SDKAPI sdk = GetMetadataAPI();
    static ProviderContext context{&sdk};

    const ProviderContext& GetContext() {
        return context;
    }

    static const Type* GetType(const char* name) {
        return reinterpret_cast<const Type*>(sdk.tdb->find_type(name));
    }

    static void* GetMethod(const Type* type, const char* name) {
        auto method = type && name
                          ? sdk.type_definition->find_method(reinterpret_cast<TypeDefinitionHandle>(type), name)
                          : nullptr;
        return method ? sdk.method->get_function(method) : nullptr;
    }

    void PrepareContext() {

        // 真实引用 setter：retain 是 CALL，release 是退出路径的尾 JMP。
        // TDB 已限定调用者，不再扫描整幅映像的函数头。
        const auto* countedGameObjectHolder = GetType("app.cCountedGameObjectHolder");
        const auto set_Body =
            countedGameObjectHolder ? GetMethod(countedGameObjectHolder, "set_Body(via.GameObject)") : nullptr;
        const auto set_BodyRetainCall =
            set_Body ? HookUtils::LookupFunctionPattern(set_Body, "E8 ? ? ? ? 48 8B", 0x50) : 0;
        const auto set_BodyReleaseJump =
            set_Body ? HookUtils::LookupFunctionPattern(set_Body, "E9 ? ? ? ? 48 89", 0x50) : 0;
        const auto Retain = set_BodyRetainCall ? HookUtils::ReadOffsetData(set_BodyRetainCall, 1, 5) : 0;
        const auto Release = set_BodyReleaseJump ? HookUtils::ReadOffsetData(set_BodyReleaseJump, 1, 5) : 0;

        const auto* stringType = GetType("System.String");
        const auto FastAllocateStringEntry =
            stringType ? GetMethod(stringType, "FastAllocateString(System.Int32)") : nullptr;

        // 沿用 common/src/GameTypes.cpp 的 RE::create_managed_action
        // 调用点，准备期不依赖 RF SDK。
        const auto* partsBoxType = GetType("app.GUI030205PartsBox");
        const auto setupControl = partsBoxType ? GetMethod(partsBoxType, "setupControl()") : nullptr;
        const auto setupControlCreateActionCallSetup =
            setupControl ? HookUtils::LookupFunctionPattern(setupControl, "49 89 F8 E8", 0x80) : 0;
        const auto CreateActionEntry = setupControlCreateActionCallSetup
                                           ? HookUtils::ReadOffsetData(setupControlCreateActionCallSetup + 3, 1, 5)
                                           : 0;

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
        const auto resourceManagerSlot =
            userDataManagerLoad ? reinterpret_cast<void**>(HookUtils::ReadOffsetData(userDataManagerLoad, 3, 7))
                                : nullptr;
        const auto CreateUserDataResource =
            createUserDataCallSetup ? HookUtils::ReadOffsetData(createUserDataCallSetup + 3, 1, 5) : 0;
        const auto createResourceCallSetup =
            decodeFields ? HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(decodeFields), "49 89 F8 E8", 0x800)
                         : 0;
        const bool hasResourceTypeLoad =
            createResourceCallSetup &&
            memcmp(reinterpret_cast<const void*>(createResourceCallSetup - 7), "\x48\x8B\x15", 3) == 0;
        const auto resourceTypeInfoSlot =
            hasResourceTypeLoad ? reinterpret_cast<void**>(HookUtils::ReadOffsetData(createResourceCallSetup - 7, 3, 7))
                                : nullptr;
        const auto CreateResource =
            hasResourceTypeLoad ? HookUtils::ReadOffsetData(createResourceCallSetup + 3, 1, 5) : 0;
        sdk.managed_object = &PrepareObjectAPI(CreateInstance, CreateArray, CreateActionEntry, Retain, Release,
                                               reinterpret_cast<uintptr_t>(FastAllocateStringEntry));
        sdk.resource_manager = &PrepareResourceAPI(DeserializeRsz, resourceManagerSlot, CreateUserDataResource,
                                                   resourceTypeInfoSlot, CreateResource, *sdk.managed_object);
    }

} // namespace ci
