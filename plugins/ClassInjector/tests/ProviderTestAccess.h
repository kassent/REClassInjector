#pragma once
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <ci/API.hpp>
#include <ci/Provider.h>

namespace CIProviderTestAccess {
    inline const ci::Type* QueryType(const ci::TypeDatabase& database, std::string_view name) {
        return database.GetType(name);
    }

    inline const ci::Type* QueryType(const std::unique_ptr<ci::TypeDatabase>& database, std::string_view name) {
        return database->GetType(name);
    }

    inline void* QueryMethod(const ci::TypeDatabase& database, const ci::Type* type, std::string_view name) {
        return database.GetMethod(type, name);
    }

    inline void* QueryMethod(const std::unique_ptr<ci::TypeDatabase>& database, const ci::Type* type,
                             std::string_view name) {
        return database->GetMethod(type, name);
    }

    inline ci::TypeDefinitionHandle Handle(const ci::Type* type) {
        return reinterpret_cast<ci::TypeDefinitionHandle>(type);
    }

    inline const ci::Type* QueryType(const ci::ProviderContext& context, const char* name) {
        return reinterpret_cast<const ci::Type*>(context.sdk->tdb->find_type(name));
    }

    inline const ci::Type* QueryType(const ci::ProviderContext* context, const char* name) {
        return QueryType(*context, name);
    }

    inline void* QueryMethod(const ci::ProviderContext& context, const ci::Type* type, const char* name) {
        if (!type || !name)
            return nullptr;
        auto method = context.sdk->type_definition->find_method(Handle(type), name);
        return method ? context.sdk->method->get_function(method) : nullptr;
    }

    inline void* QueryMethod(const ci::ProviderContext* context, const ci::Type* type, const char* name) {
        return QueryMethod(*context, type, name);
    }

    inline bool IsEnum(const ci::ProviderContext& context, const ci::Type* type) {
        return context.sdk->type_definition->is_enum(Handle(type));
    }

    inline bool IsDerived(const ci::ProviderContext& context, const ci::Type* type, const ci::Type* baseType) {
        return context.sdk->type_definition->is_derived_from(Handle(type), Handle(baseType));
    }

    inline uint32_t ValueSize(const ci::ProviderContext& context, const ci::Type* type) {
        return context.sdk->type_definition->get_valuetype_size(Handle(type));
    }

    inline const ci::Type* InstanceType(const ci::ProviderContext& context, const void* object) {
        return reinterpret_cast<const ci::Type*>(context.sdk->managed_object->get_type_definition(object));
    }

    inline void* CreateAction(const ci::ProviderContext& context, void* vmContext, const ci::Type* action, void* target,
                              const ci::Type* owner, const char* signature, bool allocateOnHeap) {
        auto method =
            owner && signature ? context.sdk->type_definition->find_method(Handle(owner), signature) : nullptr;
        return context.sdk->managed_object->create_action(vmContext, Handle(action), target, method, allocateOnHeap);
    }
} // namespace CIProviderTestAccess
