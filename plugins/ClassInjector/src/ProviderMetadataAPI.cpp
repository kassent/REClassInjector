#include "ProviderMetadataAPI.h"
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <cstring>

namespace ci {
    static const Type* Unwrap(TypeDefinitionHandle typeHandle) {
        return reinterpret_cast<const Type*>(typeHandle);
    }

    static const FieldDef* Unwrap(FieldHandle fieldHandle) {
        return reinterpret_cast<const FieldDef*>(fieldHandle);
    }

    static const MethodDef* Unwrap(MethodHandle methodHandle) {
        return reinterpret_cast<const MethodDef*>(methodHandle);
    }

    static TypeDefinitionHandle Wrap(const Type* value) {
        return reinterpret_cast<TypeDefinitionHandle>(value);
    }

    static const char* GetFullName(TypeDefinitionHandle type) {
        return type ? g_database->GetFullNameData(uint32_t(Unwrap(type)->index)) : nullptr;
    }

    static TypeDefinitionHandle FindType(const char* name) {
        return Wrap(g_database && name ? g_database->GetType(name) : nullptr);
    }

    static TypeDefinitionHandle GetParent(TypeDefinitionHandle typeHandle) {
        auto* type = Unwrap(typeHandle);
        return Wrap(type && type->parentTypeIndex ? &g_database->types[type->parentTypeIndex] : nullptr);
    }

    static TypeDefinitionHandle GetElement(TypeDefinitionHandle typeHandle) {
        auto* type = Unwrap(typeHandle);
        return Wrap(type && type->elementTypeIndex ? &g_database->types[type->elementTypeIndex] : nullptr);
    }

    static bool IsEnum(TypeDefinitionHandle typeHandle) {
        return g_database && g_database->IsEnum(Unwrap(typeHandle));
    }

    static bool IsValueType(TypeDefinitionHandle typeHandle) {
        auto* type = Unwrap(typeHandle);
        return type && type->objectType == std::to_underlying(via::clr::VMObjType::ValType);
    }

    static bool IsArray(TypeDefinitionHandle typeHandle) {
        auto* type = Unwrap(typeHandle);
        return type && type->objectType == std::to_underlying(via::clr::VMObjType::Array);
    }

    static uint32_t GetValueSize(TypeDefinitionHandle typeHandle) {
        return g_database ? g_database->GetValueTypeSize(Unwrap(typeHandle)) : 0;
    }

    static uint32_t GetFieldCount(TypeDefinitionHandle typeHandle) {
        auto* type = Unwrap(typeHandle);
        return type ? g_database->typeImpls[type->implIndex].numMemberFields : 0;
    }

    static uint32_t GetMethodCount(TypeDefinitionHandle typeHandle) {
        auto* type = Unwrap(typeHandle);
        return type ? g_database->typeImpls[type->implIndex].numMemberMethods : 0;
    }

    static FieldHandle FindField(TypeDefinitionHandle typeHandle, const char* name) {
        return reinterpret_cast<FieldHandle>(
            g_database && name ? g_database->FindFieldDeclaration(Unwrap(typeHandle), name) : nullptr);
    }

    static MethodHandle FindMethod(TypeDefinitionHandle typeHandle, const char* name) {
        return reinterpret_cast<MethodHandle>(g_database && name ? g_database->GetMethodDef(Unwrap(typeHandle), name)
                                                                 : nullptr);
    }

    static Result GetFields(TypeDefinitionHandle typeHandle, FieldHandle* outFields, uint32_t outSize,
                            uint32_t* outCount) {
        if (outCount)
            *outCount = 0;
        if (!typeHandle)
            return ERROR_UNKNOWN;
        const auto length = GetFieldCount(typeHandle);
        if (outSize / sizeof(FieldHandle) < length)
            return ERROR_OUT_TOO_SMALL;
        if (length && !outFields)
            return ERROR_UNKNOWN;
        const auto start = Unwrap(typeHandle)->memberFieldIndex;
        for (uint32_t i = 0; i < length; ++i)
            outFields[i] = reinterpret_cast<FieldHandle>(&g_database->fields[start + i]);
        if (outCount)
            *outCount = length;
        return ERROR_NONE;
    }

    static Result GetMethods(TypeDefinitionHandle typeHandle, MethodHandle* outMethods, uint32_t outSize,
                             uint32_t* outCount) {
        if (outCount)
            *outCount = 0;
        if (!typeHandle)
            return ERROR_UNKNOWN;
        const auto length = GetMethodCount(typeHandle);
        if (outSize / sizeof(MethodHandle) < length)
            return ERROR_OUT_TOO_SMALL;
        if (length && !outMethods)
            return ERROR_UNKNOWN;
        const auto start = Unwrap(typeHandle)->memberMethodIndex;
        for (uint32_t i = 0; i < length; ++i)
            outMethods[i] = reinterpret_cast<MethodHandle>(&g_database->methods[start + i]);
        if (outCount)
            *outCount = length;
        return ERROR_NONE;
    }

    static const char* GetFieldName(FieldHandle fieldHandle) {
        return fieldHandle ? g_database->strings.data() + g_database->fieldImpls[Unwrap(fieldHandle)->impl].name
                           : nullptr;
    }

    static TypeDefinitionHandle GetFieldType(FieldHandle fieldHandle) {
        return Wrap(fieldHandle ? &g_database->types[Unwrap(fieldHandle)->type] : nullptr);
    }

    static TypeDefinitionHandle GetDeclaringType(FieldHandle fieldHandle) {
        return Wrap(fieldHandle ? &g_database->types[Unwrap(fieldHandle)->declaringType] : nullptr);
    }

    static uint32_t GetFieldFlags(FieldHandle fieldHandle) {
        return fieldHandle ? g_database->fieldImpls[Unwrap(fieldHandle)->impl].flags : 0;
    }

    static bool IsStatic(FieldHandle fieldHandle) {
        return (GetFieldFlags(fieldHandle) & 0x10) != 0;
    }

    static bool IsLiteral(FieldHandle fieldHandle) {
        return (GetFieldFlags(fieldHandle) & 0x40) != 0;
    }

    static uint32_t GetFieldOffset(FieldHandle fieldHandle) {
        return fieldHandle ? g_database->fieldImpls[Unwrap(fieldHandle)->impl].offset : 0;
    }

    static uint32_t GetFieldBaseOffset(FieldHandle fieldHandle) {
        if (!fieldHandle)
            return 0;
        const auto* type = Unwrap(GetDeclaringType(fieldHandle));
        const auto base = type->objectInfo ? *reinterpret_cast<const int32_t*>(type->objectInfo - 1) : 0;
        return uint32_t(base + GetFieldOffset(fieldHandle));
    }

    static void* GetInitData(FieldHandle fieldHandle) {
        return fieldHandle ? const_cast<void*>(g_database->GetFieldInitializer(*Unwrap(fieldHandle))) : nullptr;
    }

    static void* GetFieldData(FieldHandle fieldHandle, void* object, bool isValueType) {
        if (!fieldHandle)
            return nullptr;
        if (IsLiteral(fieldHandle))
            return GetInitData(fieldHandle);
        if (!object || IsStatic(fieldHandle))
            return nullptr;
        const auto base =
            isValueType ? 0 : *reinterpret_cast<const int32_t*>(*static_cast<const uintptr_t*>(object) - sizeof(void*));
        return static_cast<uint8_t*>(object) + base + GetFieldOffset(fieldHandle);
    }

    static const char* GetMethodName(MethodHandle methodHandle) {
        return methodHandle ? g_database->strings.data() +
                                  (g_database->methodImpls[Unwrap(methodHandle)->impl].name & 0x0fffffff)
                            : nullptr;
    }

    static void* GetFunction(MethodHandle methodHandle) {
        const auto* method = Unwrap(methodHandle);
        return method && method->encodedOffset
                   ? reinterpret_cast<void*>(g_database->encodingBase + method->encodedOffset)
                   : nullptr;
    }

    static bool IsStaticMethod(MethodHandle methodHandle) {
        return methodHandle && (g_database->methodImpls[Unwrap(methodHandle)->impl].flags & 0x10) != 0;
    }

    static bool IsDerivedFrom(TypeDefinitionHandle type, TypeDefinitionHandle base) {
        return g_database && g_database->IsDerivedFrom(Unwrap(type), Unwrap(base));
    }

    static uint32_t GetIndex(TypeDefinitionHandle type) {
        return type ? uint32_t(Unwrap(type)->index) : 0;
    }

    static uint32_t GetRank(TypeDefinitionHandle type) {
        return type ? uint32_t(Unwrap(type)->arrayRank) : 0;
    }

    static TypeDefinitionHandle GetUnderlyingType(TypeDefinitionHandle type) {
        if (!IsEnum(type))
            return nullptr;
        const char* name{};
        switch (Unwrap(type)->underlyingTypeIndex) {
        case 4:
            name = "System.SByte";
            break;
        case 5:
            name = "System.Byte";
            break;
        case 6:
            name = "System.Int16";
            break;
        case 7:
            name = "System.UInt16";
            break;
        case 8:
            name = "System.Int32";
            break;
        case 9:
            name = "System.UInt32";
            break;
        case 10:
            name = "System.Int64";
            break;
        case 11:
            name = "System.UInt64";
            break;
        default:
            return nullptr;
        }
        return FindType(name);
    }

    const SDKAPI& GetMetadataAPI() {
        static const TDB tdb{FindType};
        static const TDBTypeDefinition type{
            GetFullName,  GetParent,     GetElement,        IsEnum,    IsValueType, IsArray,
            GetValueSize, GetFieldCount, GetMethodCount,    FindField, FindMethod,  IsDerivedFrom,
            GetIndex,     GetRank,       GetUnderlyingType, GetFields, GetMethods};
        static const TDBField field{GetFieldName, GetFieldType,       GetDeclaringType, GetFieldFlags, IsStatic,
                                    IsLiteral,    GetFieldBaseOffset, GetFieldOffset,   GetInitData,   GetFieldData};
        static const TDBMethod method{GetMethodName, GetFunction, IsStaticMethod};
        static const SDKAPI sdk{&tdb, &type, &field, &method};
        return sdk;
    }
} // namespace ci
