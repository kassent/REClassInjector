#include "TypeDatabase.h"
#include "CustomTypeConfig.h"
#include <BranchTrampoline.h>
#include <LogUtils.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace ci {
    TypeDatabase::TypeDatabase() = default;
    TypeDatabase::~TypeDatabase() = default;

    // 枚举身份由父类型确认，底层Int32及payload约束供ABI选择与成员扩展共同使用。
    bool TypeDatabase::IsInt32Enum(uint32_t index) const {
        const auto& type = types[index];
        return IsEnum(&type) && type.objectType == std::to_underlying(via::clr::VMObjType::ValType) &&
               type.underlyingTypeIndex == std::to_underlying(via::clr::ElementType::I4) &&
               typeImpls[type.implIndex].fieldSize == sizeof(int32_t);
    }

    // TDB 名称字段存池内偏移，不存 string 指针；每条字符串以零结尾。
    uint32_t TypeDatabase::AppendString(std::string_view value) {
        uint32_t offset = uint32_t(strings.size());
        strings.insert(strings.end(), value.begin(), value.end());
        strings.push_back(0);
        return offset;
    }

    // bytePool 装参数列表、slot ID、接口列表和泛型描述，统一按 4 字节对齐并返回池内偏移。
    uint32_t TypeDatabase::AppendBytes(const void* data, size_t size) {
        while (bytes.size() % 4) {
            bytes.push_back(0);
        }
        uint32_t offset = uint32_t(bytes.size());
        auto* start = static_cast<const uint8_t*>(data);
        if (size) {
            bytes.insert(bytes.end(), start, start + size);
        }
        return offset;
    }

    // 方法的 paramsLo/Hi 合成 bytePool 偏移；ParamList 后面是参数 ID，而不是 Parameter 实体。
    const ParamList* TypeDatabase::GetMethodParameters(const MethodDef& method) const {
        return reinterpret_cast<const ParamList*>(bytes.data() + method.GetParameterOffset());
    }

    // 按引擎 objectType 分类，不根据名称猜测：class / array / string 等引用占一个指针槽。
    bool TypeDatabase::IsReferenceType(uint32_t index) const {
        auto kind = types[index].objectType;
        return kind >= std::to_underlying(via::clr::VMObjType::Object) &&
               kind <= std::to_underlying(via::clr::VMObjType::Delegate);
    }

    // 字段/返回值的数据大小，不含对象头；引用固定 8 字节，值类型使用其 fieldSize。
    uint32_t TypeDatabase::GetStorageSize(uint32_t index) const {
        if (!index || GetFullName(index) == "System.Void") {
            return 0;
        }
        if (IsReferenceType(index)) {
            return sizeof(void*);
        }
        return uint32_t(typeImpls[types[index].implIndex].fieldSize);
    }

    // 接口条目低 19 位为 interface type ID，高 13 位为该接口在类托管虚表中的窗口起点。
    // 返回 bytePool 的借用视图；调用期间不追加池数据，避免 vector 重分配使 span 失效。
    std::span<const uint32_t> TypeDatabase::GetInterfaces(const RE::RETypeDefinition& type) const {
        auto offset = type.getInterfaceListOffset();
        if (!offset) {
            return {};
        }
        auto* list = reinterpret_cast<const RE::REInterfaceList*>(bytes.data() + offset);
        return {list->encodedTypes, list->count};
    }
    // 按 declaring type、namespace、泛型实参和数组 element 还原名称，不依赖原名称搜索分区。
    std::string TypeDatabase::GetFullName(uint32_t index) const {
        if (!index) {
            return "System.Void";
        }
        auto& definition = types[index];
        if (definition.objectType == 2 && definition.elementTypeIndex) {
            return GetFullName(uint32_t(definition.elementTypeIndex)) + '[' +
                   std::string(definition.arrayRank > 1 ? definition.arrayRank - 1 : 0, ',') + ']';
        }
        auto& impl = typeImpls[definition.implIndex];
        std::string name = strings.data() + impl.nameOffset;
        if (definition.genericsOffset) {
            auto* descriptor = reinterpret_cast<const uint32_t*>(bytes.data() + definition.genericsOffset);
            if ((descriptor[0] & 0x7FFFF) != index) {
                name += '<';
                auto count = (descriptor[0] >> 19) & 0x7F;
                for (uint32_t i = 0; i < count; ++i) {
                    if (i) {
                        name += ',';
                    }
                    name += GetFullName(descriptor[i + 1]);
                }
                name += '>';
            }
        }
        if (definition.declaringTypeIndex) {
            return GetFullName(uint32_t(definition.declaringTypeIndex)) + "." + name;
        }
        auto* space = strings.data() + impl.namespaceOffset;
        return *space ? std::string(space) + "." + name : name;
    }
    // typeNames 在复制时收录原类型、预留时收录所有新增 ID，因此允许字段/签名的前向引用。
    uint32_t TypeDatabase::ResolveType(std::string_view name) const {
        auto found = typeNames.find(std::string(name));
        if (found != typeNames.end()) {
            return found->second;
        }
        throw std::runtime_error("missing managed type: " + std::string(name));
    }
    // 查找 hook 只拦截新增类型；既有名字返回空，让调用方继续执行原引擎查找。
    RuntimeType* TypeDatabase::FindType(std::string_view name) {
        auto found = typeNames.find(std::string(name));
        if (found == typeNames.end() || found->second < originalCount) {
            return nullptr;
        }
        return &customTypes[found->second - originalCount];
    }
    // 追加 method ID 与 customMethods 连续对应，原方法或越界 ID 不命中自定义入口。
    RuntimeMethod* TypeDatabase::FindMethod(uint32_t index) {
        auto offset = index - originalMethodCount;
        return offset < customMethods.size() ? &customMethods[offset] : nullptr;
    }
    // 有限类型关系：同类型、父类、已登记接口；用于接口验证，不扩展数组协变或泛型变型。
    bool TypeDatabase::IsAssignable(uint32_t source, uint32_t target) const {
        if (source == target) {
            return true;
        }
        if (types[target].typeFlags & 0x20) {
            for (auto encoded : GetInterfaces(types[source])) {
                if ((encoded & 0x7FFFF) == target) {
                    return true;
                }
            }
            return false;
        }
        return IsDerivedFrom(&types[source], &types[target]);
    }

    static const MethodDef* FindMethodDef(const TypeDatabase& database, const Type* type, std::string_view query,
                                            bool matchFullSignature) {
        const bool isConstructorQuery = query == ".ctor" || query.starts_with(".ctor(");
        auto IsGenericParameter = [&](uint32_t typeIndex) {
            auto& typeImpl = database.typeImpls[database.types[typeIndex].implIndex];
            return std::string_view(database.strings.data() + typeImpl.nameOffset).contains('!');
        };
        for (auto* declaringType = type; declaringType;
             declaringType = declaringType->parentTypeIndex ? &database.types[declaringType->parentTypeIndex] : nullptr) {
            auto& typeImpl = database.typeImpls[declaringType->implIndex];
            for (uint32_t i = 0; i < typeImpl.numMemberMethods; ++i) {
                auto methodIndex = uint32_t(declaringType->memberMethodIndex) + i;
                auto& methodDef = database.methods[methodIndex];
                auto methodName =
                    std::string_view(database.strings.data() + (database.methodImpls[methodDef.impl].name & 0x0FFFFFFF));
                if ((!matchFullSignature && query != methodName) || !methodDef.GetParameterOffset()) {
                    continue;
                }
                auto* parameterList = reinterpret_cast<const ParamList*>(database.bytes.data() + methodDef.GetParameterOffset());
                auto* parameterIds = reinterpret_cast<const uint32_t*>(parameterList + 1);
                auto parameterIdMask = methodIndex < database.originalMethodCount ? database.originalParameterMask : UINT32_MAX;
                bool hasGenericParameters = IsGenericParameter(uint32_t(database.parameters[parameterList->returnParameter].type));
                std::string methodSignature;
                if (matchFullSignature) {
                    methodSignature = methodName;
                    methodSignature += '(';
                }
                for (uint32_t j = 0; j < parameterList->count; ++j) {
                    auto parameterType = uint32_t(database.parameters[parameterIds[j] & parameterIdMask].type);
                    hasGenericParameters |= IsGenericParameter(parameterType);
                    if (matchFullSignature) {
                        if (j) {
                            methodSignature += ", ";
                        }
                        methodSignature += database.GetFullName(parameterType);
                    }
                }
                if (matchFullSignature) {
                    methodSignature += ')';
                }
                // 与 RF 一样检查类型的原始 name，而不是闭合泛型展开后的 fullName。
                if (!hasGenericParameters && (!matchFullSignature || query == methodSignature)) {
                    return &methodDef;
                }
            }
            // 构造不继承；本类缺少 .ctor() 时不能误返回父类默认入口。
            if (isConstructorQuery) break;
        }
        return nullptr;
    }

    const char* TypeDatabase::GetFullNameData(uint32_t index) const {
        {
            std::shared_lock lock(nameQueryMutex);
            if (auto found = fullNameQueries.find(index); found != fullNameQueries.end())
                return found->second.c_str();
        }
        auto name = GetFullName(index);
        std::unique_lock lock(nameQueryMutex);
        return fullNameQueries.try_emplace(index, std::move(name)).first->second.c_str();
    }

    const Type* TypeDatabase::GetType(std::string_view name) const {
        auto found = typeNames.find(std::string(name));
        // typeNames 还含供 TOML 使用的泛型别名；公共查询只接受 RF 风格的完整名称。
        return found != typeNames.end() && GetFullName(found->second) == name ? &types[found->second] : nullptr;
    }

    // 判断同类型或父类链命中；任一参数为空返回 false，不包含接口关系或协变/变型。
    bool TypeDatabase::IsDerivedFrom(const Type* type, const Type* baseType) const {
        if (!baseType)
            return false;
        for (; type; type = type->parentTypeIndex ? &types[type->parentTypeIndex] : nullptr)
            if (type == baseType)
                return true;
        return false;
    }

    // 仅以直接父类型 System.Enum 识别枚举；空类型或缺少该基础类型返回 false。
    bool TypeDatabase::IsEnum(const Type* type) const {
        if (!type)
            return false;
        const auto* enumType = GetType("System.Enum");
        return enumType && type->parentTypeIndex == enumType->index;
    }

    void* TypeDatabase::GetMethod(const Type* type, std::string_view name) const {
        const auto* method = GetMethodDef(type, name);
        return method && method->encodedOffset ? reinterpret_cast<void*>(encodingBase + method->encodedOffset) : nullptr;
    }

    // 与字段布局共用 TypeImpl.fieldSize；开放泛型不具有可供实例写入的确定布局。
    uint32_t TypeDatabase::GetValueTypeSize(const Type* type) const {
        if (!type || type->objectType != std::to_underlying(via::clr::VMObjType::ValType) || (type->typeFlags & 0x08000000))
            return 0;
        const auto size = typeImpls[type->implIndex].fieldSize;
        return size > 0 ? uint32_t(size) : 0;
    }

    const MethodDef* TypeDatabase::GetMethodDef(const Type* type, std::string_view name) const {
        if (!type) {
            return nullptr;
        }
        auto key = std::string(name);
        {
            std::shared_lock lock(methodQueryMutex);
            if (auto found = methodQueries.find(type); found != methodQueries.end()) {
                if (auto method = found->second.find(key); method != found->second.end()) {
                    return method->second;
                }
            }
        }
        auto* method = FindMethodDef(*this, type, name, false);
        if (!method) {
            method = FindMethodDef(*this, type, name, true);
        }
        // 仅首次解析失败时记录；缓存命中不刷屏，声明仍保留以读取后续安装/改写的入口。
        if (!method) {
            _MESSAGE("TDB method declaration not found: type=%s query=%s", GetFullName(uint32_t(type->index)).c_str(),
                     key.c_str());
        } else if (!method->encodedOffset) {
            _MESSAGE(
                "TDB method declaration has no entry: type=%s query=%s method=%zu",
                GetFullName(uint32_t(type->index)).c_str(), key.c_str(), size_t(method - methods.data()));
        }
        std::unique_lock lock(methodQueryMutex);
        methodQueries[type][std::move(key)] = method;
        return method;
    }

    // Only reads field tables; valid before runtime ObjectInfo exists, including inherited declarations.
    const FieldDef* TypeDatabase::GetFieldDef(const Type* type, std::string_view name) const {
        for (auto* current = type; current;
             current = current->parentTypeIndex ? &types[current->parentTypeIndex] : nullptr) {
            const auto& impl = typeImpls[current->implIndex];
            for (uint32_t i = 0; i < impl.numMemberFields; ++i) {
                const auto& field = fields[current->memberFieldIndex + i];
                const auto& layout = fieldImpls[field.impl];
                if (name == strings.data() + layout.name && !(layout.flags & 0x10))
                    return &field;
            }
        }
        return nullptr;
    }

    const FieldDef* TypeDatabase::FindFieldDeclaration(const Type* type, std::string_view name) const {
        for (auto* current = type; current;
             current = current->parentTypeIndex ? &types[current->parentTypeIndex] : nullptr) {
            const auto& impl = typeImpls[current->implIndex];
            for (uint32_t i = 0; i < impl.numMemberFields; ++i) {
                const auto& field = fields[current->memberFieldIndex + i];
                if (name == strings.data() + fieldImpls[field.impl].name)
                    return &field;
            }
        }
        return nullptr;
    }

    const void* TypeDatabase::GetFieldInitializer(const FieldDef& field) const {
        const auto& impl = fieldImpls[field.impl];
        // Raw initializer data also includes nonliteral RVA fields. Literal policy belongs to callers.
        // Reference initializers are encoded pool data, not a managed object or a pointer-sized payload.
        const auto width = IsReferenceType(uint32_t(field.type)) ? 1 : typeImpls[types[field.type].implIndex].fieldSize;
        if (width <= 0)
            return nullptr;
        const auto index = (field.initHi << 10) | (impl.initMid << 6) | impl.initLo;
        const auto* data = initializers.empty() ? static_cast<const int32_t*>(header.initData) : initializers.data();
        const auto count =
            initializers.empty() ? size_t(header.numInitData > 0 ? header.numInitData : 0) : initializers.size();
        if (!data || index >= count)
            return nullptr;
        const auto offset = int64_t(data[index]);
        const auto position = size_t(offset < 0 ? -offset : offset);
        const auto size = offset < 0 ? strings.size() : bytes.size();
        if (!offset || position > size || size - position < width)
            return nullptr;
        return offset < 0 ? static_cast<const void*>(strings.data() + position) : bytes.data() + position;
    }

    bool TypeDatabase::IsRuntimeTypeReady(const RuntimeType& type) const {
        const auto& definition = types[type.index];
        if (definition.typeFlags & 0x08000000) {
            return !definition.objectInfo;
        }
        if (!definition.typeInfo) {
            return false;
        }
        if (definition.objectType == 2) {
            // Native T[] uses System.Array's shared TypeInfo, but has its own
            // ObjectInfo and element identity. Class-only TypeInfo uniqueness
            // would stop finalization before any managed entry was installed.
            return definition.typeInfo == types[type.parent].typeInfo && definition.objectInfo &&
                   definition.objectInfo[0] == &definition && definition.elementTypeIndex &&
                   types[definition.elementTypeIndex].arrayTypeIndex == type.index;
        }
        const bool abstract = (definition.typeFlags & 0x80) != 0;
        return definition.typeInfo != types[type.parent].typeInfo &&
               (abstract || (definition.objectInfo && definition.objectInfo[0] == &definition));
    }

    // 最后的发布步骤：已有 TypeInfo 可能仍回指旧 type 记录，先把对应回指迁移到新表。
    // 再一次替换 TDB 的数量/池/表指针；引擎从此持有 TypeDatabase 地址，不支持撤销或继续扩容。
    void TypeDatabase::Publish(RE::TDB& target) {
        for (uint32_t i = 1; i < originalCount; ++i) {
            if (types[i].typeInfo) {
                auto** backPointer =
                    reinterpret_cast<RE::RETypeDefinition**>(static_cast<uint8_t*>(types[i].typeInfo) + 0x60);
                if (*backPointer == &target.types[i]) {
                    *backPointer = &types[i];
                }
            }
        }
        header.numTypes = uint32_t(types.size());
        header.numTypeImpl = uint32_t(typeImpls.size());
        header.numMethods = uint32_t(methods.size());
        header.numMethodImpl = uint32_t(methodImpls.size());
        header.numFields = uint32_t(fields.size());
        header.numFieldImpl = uint32_t(fieldImpls.size());
        header.numParams = uint32_t(parameters.size());
        header.numStringPool = uint32_t(strings.size());
        header.numBytePool = uint32_t(bytes.size());
        header.types = types.data();
        header.typeImpls = typeImpls.data();
        header.methods = methods.data();
        header.methodImpls = methodImpls.data();
        header.fields = fields.data();
        header.fieldImpls = fieldImpls.data();
        header.params = parameters.data();
        header.stringPool = strings.data();
        header.bytePool = bytes.data();
        if (!initializers.empty()) {
            header.initData = initializers.data();
            header.numInitData = int32_t(initializers.size());
        }
        target = header;
    }
} // namespace ci
