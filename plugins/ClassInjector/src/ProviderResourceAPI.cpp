#include "ProviderRuntimeAPI.h"
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <GameTypes.h>
#define CI_REQUIRE(condition)                                                                                          \
    do {                                                                                                               \
        if (!(condition))                                                                                              \
            return {};                                                                                                 \
    } while (false)

namespace ci {
    using FnUserData = void** (*)(void* manager, void** object, void* typeInfo, const wchar_t* path);
    using FnResource = void* (*)(void* manager, void* typeInfo, const wchar_t* path);
    static void** resourceManagerSlot{};
    static void** resourceTypeInfoSlot{};
    static FnUserData CreateUserDataResource{};
    static FnResource CreateResource{};
    static const ManagedObject* objects{};

    static void* CreateUserData(TypeDefinitionHandle handle, const char* utf8Path) {
        const auto* type = reinterpret_cast<const Type*>(handle);
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

    static void* CreateResourceHolder(void* vmContext, TypeDefinitionHandle handle, const char* utf8Path) {
        const auto* type = reinterpret_cast<const Type*>(handle);
        const auto* holderType = g_database ? g_database->GetType("via.ResourceHolder") : nullptr;
        CI_REQUIRE(g_database);
        CI_REQUIRE(vmContext);
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
        auto* holder = objects->create_instance(vmContext, handle, false);
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

    const ResourceManager& PrepareResourceAPI(uintptr_t deserializeAddress, void** resourceManagerAddress,
                                              uintptr_t createUserDataAddress, void** resourceTypeInfoAddress,
                                              uintptr_t createResourceAddress, const ManagedObject& objectAPI) {
        resourceManagerSlot = resourceManagerAddress;
        resourceTypeInfoSlot = resourceTypeInfoAddress;
        CreateUserDataResource = reinterpret_cast<FnUserData>(createUserDataAddress);
        CreateResource = reinterpret_cast<FnResource>(createResourceAddress);
        objects = &objectAPI;
        static ResourceManager api{};
        api = {resourceManagerAddress && createUserDataAddress ? &CreateUserData : nullptr,
               objectAPI.create_instance && resourceManagerAddress && resourceTypeInfoAddress && createResourceAddress
                   ? &CreateResourceHolder
                   : nullptr,
               reinterpret_cast<decltype(api.deserialize_rsz)>(deserializeAddress)};
        return api;
    }
} // namespace ci

#undef CI_REQUIRE
