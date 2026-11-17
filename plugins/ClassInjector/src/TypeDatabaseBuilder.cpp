#include "TypeDatabaseBuilder.h"
#include "CustomTypeBuildSpec.h"
#include "ClassFields.h"
#include "ClassMethods.h"
#include "GenericTypes.h"
#include "EnumMembers.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

// 元数据层：在 TypeDatabase 自有存储中复制原表、追加 class 形状，再由 TypeInjection 在启动窗口发布。
// 不自行分配 TypeInfo / ObjectInfo，也不生成任意泛型 AOT 代码；这些运行时对象仍由原 builder 创建。
// 读代码主线：BuildTDB -> CopyTypeDatabase -> ExpandGenericTemplates -> OrderTypeDeclarations
//            -> ReserveTypeDatabase -> BuildClass(Fields / Methods / Interfaces) -> RebaseReferenceBitmaps -> Publish。
namespace ci {

    // 与引擎 FQN 查找一致的 MurmurHash3 x86_32（seed=FFFFFFFF）；名称哈希不等同于结构 CRC。
    uint32_t NameHash(std::string_view value) {
        uint32_t hash = 0xFFFFFFFF;
        size_t position = 0;
        auto Mix = [](uint32_t word) { return std::rotl(word * 0xCC9E2D51u, 15) * 0x1B873593u; };
        for (; position + 4 <= value.size(); position += 4) {
            uint32_t word;
            std::memcpy(&word, value.data() + position, 4);
            hash = std::rotl(hash ^ Mix(word), 13) * 5u + 0xE6546B64u;
        }
        uint32_t tail = 0;
        for (size_t i = position; i < value.size(); ++i) {
            tail |= uint32_t(uint8_t(value[i])) << ((i - position) * 8);
        }
        if (position != value.size()) {
            hash ^= Mix(tail);
        }
        hash ^= uint32_t(value.size());
        hash = (hash ^ (hash >> 16)) * 0x85EBCA6Bu;
        hash = (hash ^ (hash >> 13)) * 0xC2B2AE35u;
        return hash ^ (hash >> 16);
    }

    template <class T> static std::vector<T> CopyTable(const void* source, size_t count) {
        std::vector<T> result;
        auto* data = static_cast<const T*>(source);
        if (count) {
            result.assign(data, data + count);
        }
        return result;
    }
    // 复制已经 relocation 的记录与池，旧 ID/记录顺序保持不动；未扩展的其他 TDB 表继续沿用原指针。
    static void CopyTypeDatabase(TypeDatabase& database, const RE::TDB& original) {
        database.header = original;
        database.originalCount = original.numTypes;
        database.originalMethodCount = original.numMethods;
        database.originalParameterMask = std::bit_ceil(original.numParams) - 1;
        database.types = CopyTable<RE::RETypeDefinition>(original.types, original.numTypes);
        database.typeImpls = CopyTable<RE::RETypeImpl>(original.typeImpls, original.numTypeImpl);
        database.methods = CopyTable<MethodDef>(original.methods, original.numMethods);
        database.methodImpls = CopyTable<MethodImpl>(original.methodImpls, original.numMethodImpl);
        database.fields = CopyTable<FieldDef>(original.fields, original.numFields);
        database.fieldImpls = CopyTable<FieldImpl>(original.fieldImpls, original.numFieldImpl);
        database.parameters = CopyTable<Parameter>(original.params, original.numParams);
        database.strings = CopyTable<char>(original.stringPool, original.numStringPool);
        database.bytes = CopyTable<uint8_t>(original.bytePool, original.numBytePool);
        // 直接遍历全部类型恢复名字，包含原搜索第一分区之外的嵌套/数组/闭合泛型。
        for (uint32_t i = 1; i < original.numTypes; ++i) {
            auto name = database.GetFullName(i);
            database.typeNames.try_emplace(name, i);
            // 同时接受 dump 省略 `arity 的闭合泛型拼写与引擎完整名称。
            auto tick = name.find('`');
            if (tick != std::string::npos) {
                auto end = name.find_first_not_of("0123456789", tick + 1);
                if (end != std::string::npos && name[end] == '<') {
                    database.typeNames.try_emplace(name.substr(0, tick) + name.substr(end), i);
                }
            }
        }
    }

    // 原 TDB 没有新增 class 的数组身份。只补齐明确使用的 T[]；不存在的值类型/多维数组仍报错。
    // 元素 class 必须先建完，避免原 builder 读取未完成的元素布局；自引用字段只需预留 ID。
    static void AddArrayTypes(const TypeDatabase& database, std::vector<CustomTypeBuildSpec>& specs) {
        std::unordered_set<std::string> names;
        for (const auto& spec : specs) names.insert(spec.name);
        std::vector<CustomTypeBuildSpec> arrays;
        auto Add = [&](const std::string& name, const CustomTypeConfig& owner) {
            if (!name.ends_with("[]") || database.typeNames.contains(name) || names.contains(name)) return;
            auto element = name.substr(0, name.size() - 2);
            if (element.ends_with(']')) throw std::runtime_error("new jagged arrays are not supported: " + name);
            names.insert(name);
            CustomTypeBuildSpec array{CustomTypeConfig{.name = name, .base = "System.Array", .providerSource = owner.providerSource}};
            array.arrayElementType = std::move(element);
            array.requestedByType = owner.name;
            arrays.push_back(std::move(array));
        };
        for (const auto& spec : specs) {
            try {
                for (const auto& argument : spec.genericArguments) Add(argument, spec);
                for (const auto& field : spec.fields) Add(field.type, spec);
                for (const auto& method : spec.methods) {
                    Add(method.returnType, spec);
                    for (const auto& parameter : method.parameters) Add(parameter.type, spec);
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        specs.insert(specs.end(), std::make_move_iterator(arrays.begin()), std::make_move_iterator(arrays.end()));
    }

    // 检查 packed ID 位宽，预留方法等表，并给所有新增类型/impl 分配连续 ID。
    // RuntimeMethod 地址会嵌入跳板，必须在生成入口前保证 customMethods 不再因追加而搬迁。
    static void ReserveTypeDatabase(TypeDatabase& database, const std::vector<CustomTypeBuildSpec>& specs) {
        const auto& original = database.header;
        size_t methodCount = 0, fieldDefCount = 0, fieldImplCount = 0, parameterCount = 0, typeCount = 0, extensionCount = 0;
        std::unordered_map<uint32_t, size_t> extendedMembers;
        for (auto& type : specs) {
            try {
                const auto added = type.enumExtension ? type.enumExtension->appends.size() : type.fields.size();
                fieldDefCount += added;
                fieldImplCount += added;
                if (type.IsTypeExtension()) {
                    if (type.enumExtension) {
                        const auto renamed = CountEnumMemberRenames(*type.enumExtension);
                        fieldImplCount += renamed;
                        if (!added && !renamed) continue;
                    }
                    auto index = database.ResolveType(type.name);
                    auto member = extendedMembers.try_emplace(
                        index, size_t(database.typeImpls[database.types[index].implIndex].numMemberFields)).first;
                    // 真实改动才复制连续 FieldDef 块；未改名的成员沿用共享 FieldImpl。
                    fieldDefCount += member->second;
                    member->second += added;
                    ++extensionCount;
                    continue;
                }
                ++typeCount;
                methodCount += type.methods.size();
                for (auto& method : type.methods) {
                    parameterCount += method.parameters.size() + 1;
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(type.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        if (original.numTypes + typeCount >= (1u << 19) ||
            original.numTypeImpl + typeCount + extensionCount >= (1u << 18) ||
            original.numMethods + methodCount >= (1u << 22) || original.numMethodImpl + methodCount >= (1u << 19) ||
            original.numFields + fieldDefCount >= (1u << 20) || original.numFieldImpl + fieldImplCount >= (1u << 19)) {
            throw std::runtime_error("metadata packed index limit exceeded");
        }
        database.methods.reserve(original.numMethods + methodCount);
        database.methodImpls.reserve(original.numMethodImpl + methodCount);
        database.fields.reserve(original.numFields + fieldDefCount);
        database.fieldImpls.reserve(original.numFieldImpl + fieldImplCount);
        database.parameters.reserve(original.numParams + parameterCount);
        database.customTypes.reserve(typeCount);
        database.extensions.reserve(extensionCount);
        database.customMethods.reserve(methodCount);
        database.methodDeclarations.reserve(methodCount);
        database.invokeSignatures.reserve(methodCount);
        // 先登记全部名字和 ID，让字段/签名可前向引用；父类布局仍按 OrderTypeDeclarations 顺序构建。
        database.types.resize(original.numTypes + typeCount);
        database.typeImpls.resize(original.numTypeImpl + typeCount);
        database.typeImpls.reserve(original.numTypeImpl + typeCount + extensionCount);
        uint32_t index = original.numTypes;
        for (auto& spec : specs) {
            try {
                if (spec.IsTypeExtension()) {
                    continue;
                }
                if (!database.typeNames.emplace(spec.name, index).second) {
                    throw std::runtime_error("duplicate type: " + spec.name);
                }
                database.types[index].index = index;
                database.types[index].implIndex = original.numTypeImpl + index - original.numTypes;
                database.types[index].objectType = uint32_t(spec.arrayElementType.empty() ? via::clr::VMObjType::Object : via::clr::VMObjType::Array);
                ++index;
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
    }

    // 单维引用数组直接沿用引擎 System.Object[] 的存储/GC形状和原生共享 slot。
    // 不复制 Object 的闭合泛型接口（IList<Object>等），只保留 System.Array 非泛型接口。
    // 数组由原分配器按 elementTypeIndex 分配；没有普通 class ctor，也不注册业务跳板。
    static void BuildArrayType(TypeDatabase& database, const CustomTypeBuildSpec& spec) {
        auto index = database.ResolveType(spec.name);
        auto element = database.ResolveType(spec.arrayElementType);
        if (!database.IsReferenceType(element) || database.types[element].objectType != uint32_t(via::clr::VMObjType::Object) ||
            (database.types[element].typeFlags & 0x08000000)) {
            throw std::runtime_error("new arrays require a closed managed class element: " + spec.name);
        }
        auto prototype = database.types[database.ResolveType("System.Object[]")];
        auto impl = database.typeImpls[prototype.implIndex];
        auto parent = database.ResolveType("System.Array");
        if (prototype.objectType != uint32_t(via::clr::VMObjType::Array) || prototype.arrayRank != 1 || prototype.parentTypeIndex != parent ||
            impl.fieldSize || impl.numMemberFields) {
            throw std::runtime_error("unsupported native reference array prototype");
        }
        RuntimeType type{.name = spec.name, .index = index, .parent = parent};
        const auto* slots = reinterpret_cast<const uint32_t*>(database.bytes.data() + prototype.unknown2C);
        type.virtualMethods.assign(slots, slots + impl.numManagedVtable);
        prototype.index = index;
        prototype.elementTypeIndex = element;
        prototype.arrayTypeIndex = 0;
        prototype.implIndex = database.header.numTypeImpl + index - database.originalCount;
        prototype.fqnHash = NameHash(spec.name);
        prototype.typeCrc = prototype.fqnHash;
        prototype.defaultCtorIndex = 0;
        prototype.memberMethodIndex = database.methods.size();
        prototype.memberFieldIndex = database.fields.size();
        prototype.numMemberProperties = prototype.memberPropertyIndex = 0;
        prototype.interfaceListOffset = database.types[parent].interfaceListOffset;
        prototype.typeInfo = nullptr;
        prototype.objectInfo = nullptr;
        prototype.unknown48 = UINT64_MAX;
        impl.numMemberMethods = 0;
        database.typeImpls[prototype.implIndex] = impl;
        database.types[index] = prototype;
        database.types[element].arrayTypeIndex = index;
        database.customTypes.push_back(std::move(type));
    }

    // 构建一个已分配 ID 的 class：继承字段/slot -> 初始化定义 -> 字段 -> 方法 -> 接口 -> 写回表。
    // 要求父布局已完成；typeInfo/objectInfo 置空，留给原 builder 创建当前类型的独立运行时对象。
    static void BuildClass(TypeDatabase& database, CustomTypeBuildSpec& spec) {
        uint32_t index = database.ResolveType(spec.name);
        uint32_t parentIndex = database.ResolveType(spec.base);
        auto& parentDefinition = database.types[parentIndex];
        auto& parentImpl = database.typeImpls[parentDefinition.implIndex];
        if (parentDefinition.objectType != uint32_t(via::clr::VMObjType::Object) || (parentDefinition.typeFlags & 0x08000120)) {
            throw std::runtime_error("base must be a non-sealed managed class: " + spec.base);
        }
        RuntimeType type{.name = spec.name, .index = index, .parent = parentIndex};
        auto* parentMethods = reinterpret_cast<const uint32_t*>(database.bytes.data() + parentDefinition.unknown2C);
        type.virtualMethods.assign(parentMethods, parentMethods + parentImpl.numManagedVtable);
        // Stopwatch 只是已验证的普通托管记录原型，不是新类的父类；真实父链来自 spec.base。
        auto definition = database.types[database.ResolveType("System.Diagnostics.Stopwatch")];
        auto templateType = definition;
        auto impl = database.typeImpls[definition.implIndex];
        impl.staticFieldSize = 0;
        impl.numMemberFields = spec.fields.size();
        impl.numMemberMethods = uint16_t(spec.methods.size());
        impl.numNativeEntries = parentImpl.numNativeEntries;
        impl.interfaceId = parentImpl.interfaceId;
        std::memset(impl.unknown1E, 0, sizeof(impl.unknown1E));
        definition.index = index;
        definition.parentTypeIndex = parentIndex;
        definition.underlyingTypeIndex = 0;
        definition.arrayTypeIndex = 0;
        definition.elementTypeIndex = 0;
        definition.implIndex = database.header.numTypeImpl + index - database.originalCount;
        definition.systemTypeIndex = 0;
        definition.unknown0F = 0;
        // 清除原型的 FrameGC arena 分配位 01000000h，新增形状使用普通堆路径。
        // 沿用 arena 位会在反复构造/析构引用字段时影响分配游标；其余不适用的形状标志一并清除。
        definition.typeFlags = (templateType.typeFlags & ~0xD90001A0u) | (spec.abstract ? 0x80 : 0);
        definition.instanceSize = 0;
        definition.fqnHash = NameHash(spec.name);
        definition.typeCrc = definition.fqnHash; // 当前仅使用名称哈希，不模拟编译器的结构 CRC。
        definition.defaultCtorIndex = 0;
        definition.memberMethodIndex = database.methods.size();
        definition.memberFieldIndex = database.fields.size();
        definition.numMemberProperties = 0;
        definition.memberPropertyIndex = 0;
        definition.unknown2B = 0;
        definition.interfaceListOffset = parentDefinition.interfaceListOffset;
        definition.genericsOffset = 0;
        definition.interfaceIndex = parentDefinition.interfaceIndex;
        definition.typeInfo = nullptr;
        definition.objectInfo = nullptr;
        definition.unknown48 = UINT64_MAX;
        if (spec.isGenericDefinition || !spec.genericTemplate.empty() || !spec.genericDefinition.empty()) {
            BuildGenericTypeIdentity(database, spec, type, definition, impl);
        } else {
            const auto dot = spec.name.rfind('.');
            impl.nameOffset = database.AppendString(std::string_view(spec.name).substr(dot + 1));
            impl.namespaceOffset = database.AppendString(std::string_view(spec.name).substr(0, dot));
            definition.declaringTypeIndex = 0;
        }
        if (!spec.isGenericDefinition) {
            BuildClassFields(database, spec, definition, impl, parentDefinition);
            BuildClassMethods(database, spec, type, definition);
            BuildInterfaceSlots(database, spec, type, definition, impl);
            if (spec.methods.size() > UINT16_MAX || type.virtualMethods.size() > size_t(INT16_MAX)) {
                throw std::runtime_error("method/slot count exceeds TDB81 encoding");
            }
            if (!spec.abstract) {
                for (auto method : type.virtualMethods) {
                    if (method && (database.methodImpls[database.methods[method].impl].flags & 0x400)) {
                        throw std::runtime_error("concrete type has an unimplemented abstract method: " + spec.name);
                    }
                }
            }
        }
        impl.numManagedVtable = type.virtualMethods.size();
        // unknown2C 在当前 TDB81 中保存“托管 slot -> method ID”列表的 bytePool 偏移。
        definition.unknown2C = database.AppendBytes(type.virtualMethods.data(), type.virtualMethods.size() * 4);
        database.typeImpls[definition.implIndex] = impl;
        database.types[index] = definition;
        database.customTypes.push_back(std::move(type));
    }

    // 展开模板后的声明按父类/泛型实参拓扑排序，确保构建时依赖布局已经就绪。
    // 字段/方法引用不参与排序：ID 会统一预分配，所以自引用或相互引用不构成继承环。
    std::vector<CustomTypeBuildSpec> OrderTypeDeclarations(std::vector<CustomTypeBuildSpec> specs) {
        std::unordered_map<std::string, size_t> indices;
        for (size_t i = 0; i < specs.size(); ++i) {
            auto& type = specs[i];
            try {
                if (!type.IsTypeExtension()) {
                    const auto [existing, inserted] = indices.emplace(type.name, i);
                    if (!inserted) throw std::runtime_error("duplicate type; first declared by " + specs[existing->second].FormatDiagnosticContext());
                }
                if (!type.IsTypeExtension() && type.arrayElementType.empty() && type.name.ends_with(']')) {
                    throw std::runtime_error(
                        "array types are synthesized from references, not class declarations: " + type.name);
                }
                if (!type.IsTypeExtension() &&
                    (type.name.find('.') == std::string::npos ||
                     type.name.starts_with('.') || type.name.ends_with('.') ||
                     type.name.find_first_not_of(
                         "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._`<>,[] ") != std::string::npos)) {
                    throw std::runtime_error("type name must be an ASCII qualified name: " + type.name);
                }
                std::unordered_set<std::string> fields;
                for (auto& field : type.fields) {
                    if (field.name.find('\0') != std::string::npos) {
                        throw std::runtime_error("field name contains NUL");
                    }
                    if (field.name.empty() || !fields.insert(field.name).second) {
                        throw std::runtime_error(type.name + ": duplicate/empty field");
                    }
                }
                std::unordered_set<std::string> methods;
                std::unordered_set<std::string> constructors;
                for (auto& method : type.methods) {
                    if (method.name.find('\0') != std::string::npos) {
                        throw std::runtime_error("method name contains NUL");
                    }
                    auto signature = method.name;
                    auto constructorSignature = method.name;
                    for (auto& parameter : method.parameters) {
                        if (parameter.name.find('\0') != std::string::npos) {
                            throw std::runtime_error("parameter name contains NUL");
                        }
                        signature += ":" + parameter.type;
                        constructorSignature += ":" + parameter.type;
                        // 按值和 ByRef 是不同签名；ref/out 仅方向不同，不允许靠方向位重载。
                        if (parameter.modifier != ParameterModifier::Value) {
                            signature += '&';
                        }
                    }
                    if (!methods.insert(signature).second) {
                        throw std::runtime_error(type.name + ": duplicate method signature");
                    }
                    // GetMethod 与 RF 一样不在查询文本编码 ref/out；构造不能产生无法消歧的公开签名。
                    if (method.kind == MethodKind::Constructor && !constructors.insert(constructorSignature).second) {
                        throw std::runtime_error(type.name + ": ambiguous constructor signature");
                    }
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(type.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        // 三色 DFS：0 未访问，1 位于当前依赖链，2 已完成；遇到 1 表示真正的构建依赖环。
        std::vector<uint8_t> state(specs.size());
        std::vector<size_t> order;
        std::function<void(size_t)> Visit = [&](size_t index) {
            if (state[index] == 2) {
                return;
            }
            if (state[index] == 1) {
                throw std::runtime_error("inheritance cycle: " + specs[index].FormatDiagnosticContext());
            }
            state[index] = 1;
            if (specs[index].base != "System.Object") {
                auto found = indices.find(specs[index].base);
                if (found != indices.end()) {
                    Visit(found->second);
                }
            }
            for (auto& argument : specs[index].genericArguments) {
                if (auto found = indices.find(argument); found != indices.end()) {
                    Visit(found->second);
                }
            }
            if (!specs[index].arrayElementType.empty()) {
                if (auto found = indices.find(specs[index].arrayElementType); found != indices.end()) Visit(found->second);
            }
            if (!specs[index].genericDefinition.empty()) {
                if (auto found = indices.find(specs[index].genericDefinition); found != indices.end()) {
                    Visit(found->second);
                }
            }
            state[index] = 2;
            order.push_back(index);
        };
        for (size_t i = 0; i < specs.size(); ++i) {
            Visit(i);
        }
        std::vector<CustomTypeBuildSpec> result;
        for (auto index : order) {
            result.push_back(std::move(specs[index]));
        }
        return result;
    }

    // 构建未发布的完整 TypeDatabase；失败时原 TDB 不变，TypeInjection 只在 binding 也准备完毕后调用 Publish。
    std::unique_ptr<TypeDatabase> BuildTDB(const RE::TDB& original, std::vector<CustomTypeConfig> declarations) {
        std::vector<CustomTypeBuildSpec> specs;
        specs.reserve(declarations.size());
        for (auto& declaration : std::exchange(declarations, {})) specs.push_back({std::move(declaration)});
        auto database = std::make_unique<TypeDatabase>();
        CopyTypeDatabase(*database, original);
        const auto enumAppend = std::ranges::find_if(specs, [](const auto& spec) { return spec.enumExtension && !spec.enumExtension->appends.empty(); });
        if (enumAppend != specs.end()) {
            if (original.numInitData <= 0 || !original.initData) throw std::runtime_error(enumAppend->FormatDiagnosticContext() + ": enum extensions require captured initializer data");
            database->initializers = CopyTable<int32_t>(original.initData, original.numInitData);
        }
        ExpandGenericTemplates(*database, specs);
        AddGenericDefinitions(*database, specs);
        AddArrayTypes(*database, specs);
        specs = OrderTypeDeclarations(std::move(specs));
        ResolveEnumRenameSources(*database, specs);
        ReserveTypeDatabase(*database, specs);
        // 先完成原类型的布局，再构建新增 class；字段可引用已经预分配 ID 的新 class。
        for (auto& spec : specs) {
            try {
                if (spec.IsTypeExtension()) {
                    if (spec.enumExtension) ApplyEnumMemberChanges(*database, spec);
                    else ExtendClassFields(*database, spec);
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        for (auto& spec : specs) {
            try {
                if (!spec.IsTypeExtension()) {
                    if (spec.arrayElementType.empty()) BuildClass(*database, spec);
                    else BuildArrayType(*database, spec);
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        if (database->bytes.size() >= (1u << 26) || database->strings.size() >= (1u << 28)) {
            throw std::runtime_error("metadata pool encoding limit exceeded");
        }
        RebaseReferenceBitmaps(*database);
        return database;
    }

} // namespace ci
