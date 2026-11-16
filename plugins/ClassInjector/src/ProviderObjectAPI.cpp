#include "ProviderRuntimeAPI.h"
#include "TypeDatabase.h"
#include <GameTypes.h>
#include <cstring>

namespace ci {
    using FnCreateInstance = void* (*)(void* vmContext, const Type* type, bool allocateOnHeap);
    using FnCreateArray = void* (*)(void* vmContext, const Type* elementType, uint64_t count, bool allocateOnHeap);
    using FnCreateAction = void* (*)(void* vmContext, const Type* actionType, void* target, const MethodDef* method,
                                     bool allocateOnHeap);
    using FnFastAllocateString = RE::SystemString* (*)(void* vmContext, int32_t length);
    static FnCreateInstance createInstance{};
    static FnCreateArray createArray{};
    static FnCreateAction createAction{};
    static FnFastAllocateString fastAllocateString{};
    static ManagedObject objectAPI{};

    static void* CreateInstance(void* vmContext, TypeDefinitionHandle type, bool allocateOnHeap) {
        return createInstance(vmContext, reinterpret_cast<const Type*>(type), allocateOnHeap);
    }

    static void* CreateArray(void* vmContext, TypeDefinitionHandle type, uint64_t count, bool allocateOnHeap) {
        return createArray(vmContext, reinterpret_cast<const Type*>(type), count, allocateOnHeap);
    }

    static void* CreateAction(void* vmContext, TypeDefinitionHandle type, void* target, MethodHandle method,
                              bool allocateOnHeap) {
        const auto* declaration = reinterpret_cast<const MethodDef*>(method);
        if (!vmContext || !type || !declaration || !declaration->encodedOffset)
            return nullptr;
        return createAction(vmContext, reinterpret_cast<const Type*>(type), target, declaration, allocateOnHeap);
    }

    static bool AssignReference(void* owner, void** field, void* value) noexcept {
        using Object = reframework::API::ManagedObject;
        using FnReference = void (*)(Object* object);
        return RE::set_managed_field(static_cast<Object*>(owner), reinterpret_cast<Object**>(field),
                                     static_cast<Object*>(value), reinterpret_cast<FnReference>(objectAPI.add_ref),
                                     reinterpret_cast<FnReference>(objectAPI.release));
    }

    static void* CreateString(void* vmContext, const char* text) {
        if (!vmContext || !text)
            return nullptr;
        const auto wide = RE::widen(text, MB_ERR_INVALID_CHARS);
        if (*text && wide.empty())
            return nullptr;
        const auto units = static_cast<int32_t>(wide.size());
        auto* result = fastAllocateString(vmContext, units);
        if (!result || result->size != units)
            return nullptr;
        std::memcpy(result->data, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        return result;
    }

    const ManagedObject& PrepareObjectAPI(uintptr_t createInstanceAddress, uintptr_t createArrayAddress,
                                          uintptr_t createActionAddress, uintptr_t retainAddress,
                                          uintptr_t releaseAddress, uintptr_t fastAllocateStringAddress) {
        createInstance = reinterpret_cast<FnCreateInstance>(createInstanceAddress);
        createArray = reinterpret_cast<FnCreateArray>(createArrayAddress);
        createAction = reinterpret_cast<FnCreateAction>(createActionAddress);
        fastAllocateString = reinterpret_cast<FnFastAllocateString>(fastAllocateStringAddress);
        const bool references = retainAddress && releaseAddress && retainAddress != releaseAddress;
        objectAPI = {[](const void* object) noexcept {
                         return object ? reinterpret_cast<TypeDefinitionHandle>(
                                             **static_cast<const Type* const* const*>(object))
                                       : nullptr;
                     },
                     createInstance ? &CreateInstance : nullptr,
                     createArray ? &CreateArray : nullptr,
                     references ? reinterpret_cast<void (*)(void*)>(retainAddress) : nullptr,
                     references ? reinterpret_cast<void (*)(void*)>(releaseAddress) : nullptr,
                     references ? &AssignReference : nullptr,
                     fastAllocateString ? &CreateString : nullptr,
                     createAction ? &CreateAction : nullptr};
        return objectAPI;
    }
} // namespace ci
