#include "ClassMethods.h"
#include "TypeDatabase.h"
#include "InvokeAdapters.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ci {
    // ByRef modifier=2；out 的方向位是 2，ref 可无方向位或带 In=1（原 updateTableInpl）。
    // In 标记本身不等于 C# readonly in；这里只还原 ref/out，不推断只读约束。
    static ParameterModifier ReadParameterModifier(const Parameter& parameter) {
        if (!parameter.modifier) {
            return ParameterModifier::Value;
        }
        if (parameter.modifier == 2 && (parameter.flags & 3) == 2) {
            return ParameterModifier::Out;
        }
        if (parameter.modifier == 2 && (parameter.flags & 3) <= 1) {
            return ParameterModifier::Ref;
        }
        throw std::runtime_error("unsupported parameter modifier/direction");
    }

    // 按值、ref、out 分别匹配原参数的 ByRef 与方向位，不将 In 标记误当 readonly in。
    static bool HasMatchingParameterModifier(const Parameter& parameter, const ParameterSpec& spec) {
        switch (spec.modifier) {
        case ParameterModifier::Value:
            return parameter.modifier == 0;
        case ParameterModifier::Ref:
            return parameter.modifier == 2 && (parameter.flags & 3) <= 1;
        case ParameterModifier::Out:
            return parameter.modifier == 2 && (parameter.flags & 3) == 2;
        }
        return false;
    }

    // 从原模板方法的 flags 还原声明种类：Virtual=40h、NewSlot=100h、Abstract=400h。
    static MethodKind GetMethodKind(std::string_view name, uint16_t flags) {
        if (name == ".ctor") {
            return MethodKind::Constructor;
        }
        if (flags & 0x400) {
            return MethodKind::Abstract;
        }
        if (!(flags & 0x40)) {
            return MethodKind::Instance;
        }
        return flags & 0x100 ? MethodKind::Virtual : MethodKind::Override;
    }

    // 从原方法记录还原声明，供接口匹配与泛型模板展开共用；保留 ref/out、参数名和方法种类。
    MethodSpec ReadMethodDeclaration(const TypeDatabase& database, uint32_t index) {
        auto& method = database.methods[index];
        MethodSpec result{.name = database.strings.data() + (database.methodImpls[method.impl].name & 0x0FFFFFFF)};
        result.kind = GetMethodKind(result.name, database.methodImpls[method.impl].flags);
        auto* list = database.GetMethodParameters(method);
        if (database.parameters[list->returnParameter].modifier) {
            throw std::runtime_error("by-reference returns are not supported");
        }
        result.returnType = database.GetFullName(uint32_t(database.parameters[list->returnParameter].type));
        auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
        auto mask = index < database.originalMethodCount ? database.originalParameterMask : UINT32_MAX;
        for (size_t i = 0; i < list->count; ++i) {
            auto& argument = database.parameters[ids[i] & mask];
            result.parameters.emplace_back(database.strings.data() + argument.name,
                                           database.GetFullName(uint32_t(argument.type)),
                                           ReadParameterModifier(argument));
        }
        return result;
    }

    // Int32 底层枚举保持自己的 type ID；只在选择调用 recipe 时等同 Int32 ABI。
    static bool IsInt32Abi(const TypeDatabase& database, uint32_t index) {
        if (database.GetFullName(index) == "System.Int32") {
            return true;
        }
        return database.IsInt32Enum(index);
    }

    // override / 接口实现必须同时匹配名字、参数和返回类型，不只比较方法名。
    // 原 Parameter ID 先按原 numParams 掩码解码；新增方法列表直接存完整 Parameter ID。
    static bool HasMatchingSignature(const TypeDatabase& database, uint32_t index, const MethodSpec& spec) {
        if (!index) {
            return false;
        }
        auto& method = database.methods[index];
        if (std::string_view(database.strings.data() + (database.methodImpls[method.impl].name & 0x0FFFFFFF)) !=
            spec.name) {
            return false;
        }
        auto* list = database.GetMethodParameters(method);
        if (list->count != spec.parameters.size()) {
            return false;
        }
        auto expectedReturn = database.ResolveType(spec.returnType);
        auto actualReturn = uint32_t(database.parameters[list->returnParameter].type);
        if (database.parameters[list->returnParameter].modifier ||
            (actualReturn != expectedReturn && !(actualReturn == 0 && spec.returnType == "System.Void"))) {
            return false;
        }
        auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
        auto mask = std::bit_ceil(database.header.numParams) - 1;
        for (size_t i = 0; i < list->count; ++i) {
            auto& parameter =
                database.parameters[method.declaringType < database.originalCount ? ids[i] & mask : ids[i]];
            if (parameter.type != database.ResolveType(spec.parameters[i].type) ||
                !HasMatchingParameterModifier(parameter, spec.parameters[i])) {
                return false;
            }
        }
        return true;
    }

    // 反射调用还需要引擎已有 invokeId：寻找参数数量、返回 ABI、virtual 属性兼容的方法作为 recipe。
    // 引用类型之间只复用相同指针 ABI 的调用形状，随后会重写实际类型 ID；不复用旧方法体。
    static const MethodDef* FindInvokeRecipe(const TypeDatabase& database, const MethodSpec& spec) {
        auto voidType = database.ResolveType("System.Void");
        auto returnType = database.ResolveType(spec.returnType);
        std::vector<uint32_t> argumentTypes;
        argumentTypes.reserve(spec.parameters.size());
        for (auto& argument : spec.parameters) {
            argumentTypes.push_back(database.ResolveType(argument.type));
        }
        auto Compatible = [&](uint32_t oldType, uint32_t newType) {
            if (!oldType || oldType == voidType) {
                return newType == voidType;
            }
            return oldType == newType || (database.IsReferenceType(oldType) && database.IsReferenceType(newType)) ||
                   (IsInt32Abi(database, oldType) && IsInt32Abi(database, newType));
        };
        auto mask = std::bit_ceil(database.header.numParams) - 1;
        bool wantsVirtual = spec.kind == MethodKind::Virtual || spec.kind == MethodKind::Override ||
                            spec.kind == MethodKind::Abstract;
        for (uint32_t i = 1; i < database.header.numMethods; ++i) {
            auto& method = database.methods[i];
            auto& impl = database.methodImpls[method.impl];
            if (!method.GetParameterOffset() || (impl.flags & 0x10) ||
                database.types[method.declaringType].objectType != std::to_underlying(via::clr::VMObjType::Object)) {
                continue;
            }
            if (bool(impl.flags & 0x40) != wantsVirtual) {
                continue;
            }
            auto* list = database.GetMethodParameters(method);
            // 开放泛型等记录的 invokeId=0 表示没有反射调用入口，不能作为可执行 recipe。
            if (!list->invokeId || list->count != spec.parameters.size() ||
                database.parameters[list->returnParameter].modifier ||
                !Compatible(uint32_t(database.parameters[list->returnParameter].type), returnType)) {
                continue;
            }
            auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
            bool match = true;
            for (size_t j = 0; j < list->count; ++j) {
                auto& parameter = database.parameters[ids[j] & mask];
                if (!HasMatchingParameterModifier(parameter, spec.parameters[j]) ||
                    !Compatible(uint32_t(parameter.type), argumentTypes[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return &method;
            }
        }
        return nullptr;
    }

    // 新 virtual / abstract 分配 NewSlot；override 去掉 NewSlot，继承原 virtual index。
    static uint16_t GetMethodFlags(MethodKind kind) {
        switch (kind) {
        case MethodKind::Constructor:
            return 0x1886;
        case MethodKind::Abstract:
            return 0x5C6;
        case MethodKind::Virtual:
            return 0x1C6;
        case MethodKind::Override:
            return 0xC6;
        default:
            return 0x86;
        }
    }

    // 为声明追加 MethodDef / MethodImpl / Parameter，并复用已有 invoke recipe。
    // 新 virtual 追加 slot；override 替换继承 slot 和它的接口别名；普通实例方法不占虚表。
    // 参数保留引擎已有 ABI；值类型必须有确定存储，反射 recipe 仍要求同一实际类型。
    // 参数数量由 TDB count 位宽和已有 recipe 决定；原生跳板原样保留栈参数。
    void BuildClassMethods(TypeDatabase& database, CustomTypeConfig& spec, RuntimeType& type, RE::RETypeDefinition& definition) {
        auto index = type.index;
        auto ctorRecipe = database.methods[database.types[database.ResolveType("System.Object")].defaultCtorIndex];
        auto methodAttributes = database.methodImpls[ctorRecipe.impl].attributes;
        for (auto& method : spec.methods) {
            try {
                if ((method.name == ".ctor") != (method.kind == MethodKind::Constructor) ||
                    (method.kind == MethodKind::Constructor && (method.returnType != "System.Void" || !method.overrideOf.empty()))) {
                    throw std::runtime_error("invalid constructor declaration: " + spec.name + "." + method.name);
                }
                // 模板展开和直接 CustomTypeConfig 输入同样遵守绑定合同，不能静默丢弃构造参数。
                if (method.kind == MethodKind::Constructor && method.binding.backend == Backend::Builtin && !method.parameters.empty()) {
                    throw std::runtime_error("parameterized constructor requires a C++ binding: " + spec.name);
                }
                if (method.kind == MethodKind::Constructor && method.binding.backend == Backend::Builtin &&
                    type.parent >= database.originalCount && !database.types[type.parent].defaultCtorIndex) {
                    throw std::runtime_error("default constructor requires a parent .ctor(): " + spec.name);
                }
                auto returnType = database.ResolveType(method.returnType);
                if (method.parameters.size() > UINT16_MAX) {
                    throw std::runtime_error("parameter count exceeds TDB limit: " + method.name);
                }
                for (auto& argument : method.parameters) {
                    auto argumentType = database.ResolveType(argument.type);
                    if (argument.modifier != ParameterModifier::Value) {
                        if (!database.GetStorageSize(argumentType)) {
                            throw std::runtime_error("ref/out parameter requires a stored value: " + argument.type);
                        }
                    } else if (!database.GetStorageSize(argumentType) ||
                               (database.types[argumentType].typeFlags & 0x08000000)) {
                        throw std::runtime_error("method argument requires a closed stored type: " + argument.type);
                    }
                }
                // 普通入口只尾跳，不把 ACTION_ID/vec3 等按值参数重新打包成 C++ ABI。
                // FindInvokeRecipe 对值类型只接受 exact type，不能借一个引用或同尺寸 struct 冒充。
                auto* recipe = method.kind == MethodKind::Constructor && method.parameters.empty() ? &ctorRecipe : FindInvokeRecipe(database, method);
                InvokeSignature invokeSignature;
                if (!recipe) {
                    invokeSignature = ClassifyInvokeSignature(database, method);
                    if (invokeSignature.result == InvokeValueKind::Unsupported) throw std::runtime_error("missing engine invoke recipe; generated invoker requires a scalar or managed-reference return: " + method.name);
                    // 分流入口与新方法签名无关；固定查找已核实的原生 wrapper，不把它的传参规则借给新方法。
                    MethodSpec anchor{.name = "invoke-dispatch", .parameters = {{"a", "System.Object"}, {"b", "System.Int32"}, {"c", "System.Object"}, {"d", "System.Int32"}, {"e", "System.Boolean"}}};
                    recipe = FindInvokeRecipe(database, anchor);
                    if (!recipe) throw std::runtime_error("missing engine invoke dispatch anchor: " + method.name);
                    database.invokeDispatchId = database.GetMethodParameters(*recipe)->invokeId;
                }
                auto list = *database.GetMethodParameters(*recipe);
                // 引用参数沿用 recipe 的方向位，尤其保留原引擎 ref 的 In=1，而不是伪造 Out=2。
                auto* recipeIds = reinterpret_cast<const uint32_t*>(database.GetMethodParameters(*recipe) + 1);
                auto parameterMask = database.originalParameterMask;
                list.returnParameter = uint32_t(database.parameters.size());
                database.parameters.push_back({.type = returnType});
                std::vector<uint32_t> parameterIds;
                for (size_t i = 0; i < method.parameters.size(); ++i) {
                    auto& argument = method.parameters[i];
                    // 新 invoker 的方向由声明决定；原 recipe 继续保留其真实 In/Out 位。
                    auto direction = invokeSignature.result == InvokeValueKind::Unsupported ? database.parameters[recipeIds[i] & parameterMask].flags & 3 : (argument.modifier == ParameterModifier::Out ? 2u : 0u);
                    parameterIds.push_back(uint32_t(database.parameters.size()));
                    database.parameters.push_back(
                        {.name = database.AppendString(argument.name),
                         .modifier = argument.modifier != ParameterModifier::Value ? 2u : 0u,
                         .type = database.ResolveType(argument.type),
                         .flags = argument.modifier != ParameterModifier::Value ? direction : 0u});
                }
                list.count = uint16_t(parameterIds.size());
                auto parameterOffset = database.AppendBytes(&list, sizeof(list));
                database.AppendBytes(parameterIds.data(), parameterIds.size() * 4);
                RuntimeMethod runtime{.index = uint32_t(database.methods.size()),
                                      .declaringType = index};
                auto kind = method.kind;
                auto flags = GetMethodFlags(kind);
                int32_t virtualSlot = -1;
                if (kind == MethodKind::Virtual || kind == MethodKind::Abstract) {
                    virtualSlot = int32_t(type.virtualMethods.size());
                    type.virtualMethods.push_back(runtime.index);
                } else if (kind == MethodKind::Override) {
                    for (size_t slot = 0; slot < type.virtualMethods.size(); ++slot) {
                        auto old = type.virtualMethods[slot];
                        if (!HasMatchingSignature(database, old, method)) {
                            continue;
                        }
                        if (!method.overrideOf.empty()) {
                            auto declaring = database.ResolveType(method.overrideOf);
                            auto& target = database.types[declaring];
                            auto& targetImpl = database.typeImpls[target.implIndex];
                            if (slot >= targetImpl.numManagedVtable) {
                                continue;
                            }
                            auto* ids = reinterpret_cast<const uint32_t*>(database.bytes.data() + target.unknown2C);
                            if (!HasMatchingSignature(database, ids[slot], method)) {
                                continue;
                            }
                        }
                        if (database.methodImpls[database.methods[old].impl].flags & 0x20) {
                            throw std::runtime_error("override target is final: " + method.name);
                        }
                        virtualSlot = int32_t(slot);
                        break;
                    }
                    if (virtualSlot < 0) {
                        throw std::runtime_error(type.name + ": override target not found: " + method.name);
                    }
                    // 同一虚方法可能在接口窗口中出现多次；override 必须同时替换所有继承别名。
                    auto old = type.virtualMethods[virtualSlot];
                    // 保留 accessor 语义，不继承 Abstract / NewSlot / Final 等实现位。
                    flags |= database.methodImpls[database.methods[old].impl].flags & 0x800;
                    for (auto& id : type.virtualMethods) {
                        if (id == old) {
                            id = runtime.index;
                        }
                    }
                }
                if (kind == MethodKind::Constructor && method.parameters.empty()) {
                    definition.defaultCtorIndex = runtime.index;
                }
                database.methods.push_back({.declaringType = index,
                                            .paramsLo = parameterOffset & 0x1FFF,
                                            .impl = uint32_t(database.methodImpls.size()),
                                            .paramsHi = parameterOffset >> 13});
                database.methodImpls.push_back({.attributes = methodAttributes,
                                                .virtualIndex = int16_t(virtualSlot),
                                                .flags = flags,
                                                .implFlags = 0,
                                                // CLR Invoke 独立检查这个位；仅写返回 Parameter 不会启用结果读回。
                                                .name = database.AppendString(method.name) | (method.returnType == "System.Void" ? 0u : 0x10000000u)});
                database.customMethods.push_back(std::move(runtime));
                method.providerSource = spec.providerSource;
                database.methodDeclarations.push_back(std::move(method));
                database.invokeSignatures.push_back(std::move(invokeSignature));
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(std::string("method ") + method.name + ": " + error.what());
            }
        }
    }

    // 实现已有接口：收集接口继承链、匹配类方法，并在 managed vtable 中建立连续分派窗口。
    // 这里只生成 metadata；ObjectInfo 的接口 dispatch prefix 交由原 builder 创建，不另加接口调用 hook。
    void BuildInterfaceSlots(TypeDatabase& database, const CustomTypeConfig& spec, RuntimeType& type,
                         RE::RETypeDefinition& definition, const RE::RETypeImpl& impl) {
        auto index = type.index;
        auto parentIndex = type.parent;
        auto& parentDefinition = database.types[parentIndex];
        auto inherited = database.GetInterfaces(parentDefinition);
        std::vector<uint32_t> interfaceEntries(inherited.begin(), inherited.end());
        std::vector<uint32_t> requested;
        auto CollectInterface = [&](auto&& collect, uint32_t id) -> void {
            if (std::ranges::find(requested, id) != requested.end()) {
                return;
            }
            auto& target = database.types[id];
            if (id >= database.originalCount || !(target.typeFlags & 0x20) || target.getInterfaceIndex() < 0) {
                throw std::runtime_error("interfaces must name existing closed interfaces: " +
                                         database.GetFullName(id));
            }
            if (target.genericsOffset &&
                (*reinterpret_cast<const uint32_t*>(database.bytes.data() + target.genericsOffset) & 0x7FFFF) ==
                    id) {
                throw std::runtime_error("open generic interfaces are not supported");
            }
            requested.push_back(id);
            for (auto encoded : database.GetInterfaces(target)) {
                collect(collect, encoded & 0x7FFFF);
            }
        };
        for (auto& name : spec.interfaces) {
            CollectInterface(CollectInterface, database.ResolveType(name));
        }
        for (auto id : requested) {
            auto& target = database.types[id];
            auto count = size_t(database.typeImpls[target.implIndex].numManagedVtable);
            auto* ids = reinterpret_cast<const uint32_t*>(database.bytes.data() + target.unknown2C);
            auto entry =
                std::ranges::find_if(interfaceEntries, [&](auto encoded) { return (encoded & 0x7FFFF) == id; });
            std::vector<uint32_t> implementations;
            for (size_t slot = 0; slot < count; ++slot) {
                auto signature = ReadMethodDeclaration(database, ids[slot]);
                auto explicitSignature = signature;
                explicitSignature.name = database.GetFullName(id) + "." + signature.name;
                uint32_t implementation = 0;
                for (auto declaring = index; declaring && !implementation;
                     declaring = declaring == index ? parentIndex
                                                    : uint32_t(database.types[declaring].parentTypeIndex)) {
                    // 当前类尚未写回 database.types，先用局部 definition/impl 查本类，再沿父链查找。
                    // 每层先匹配“完整接口名.方法名”的显式实现，再匹配普通方法名的隐式实现。
                    auto& owner = declaring == index ? definition : database.types[declaring];
                    auto& ownerImpl = declaring == index ? impl : database.typeImpls[owner.implIndex];
                    for (size_t j = 0; j < ownerImpl.numMemberMethods; ++j) {
                        auto method = uint32_t(owner.memberMethodIndex) + uint32_t(j);
                        auto flags = database.methodImpls[database.methods[method].impl].flags;
                        if (!(flags & 0x10) && HasMatchingSignature(database, method, explicitSignature)) {
                            implementation = method;
                            break;
                        }
                    }
                    if (!implementation) {
                        for (size_t j = 0; j < ownerImpl.numMemberMethods; ++j) {
                            auto method = uint32_t(owner.memberMethodIndex) + uint32_t(j);
                            if (!(database.methodImpls[database.methods[method].impl].flags & 0x10) &&
                                HasMatchingSignature(database, method, signature)) {
                                implementation = method;
                                break;
                            }
                        }
                    }
                }
                // 原 AOT 显式泛型名字可能保留旧实参拼写；未匹配到新成员时沿用已存在的父映射。
                if (!implementation && entry != interfaceEntries.end()) {
                    implementation = type.virtualMethods[(*entry >> 19) + slot];
                }
                if (!implementation) {
                    throw std::runtime_error(spec.name + ": missing interface method " + explicitSignature.name);
                }
                implementations.push_back(implementation);
            }
            // 首次实现追加窗口；再次声明已继承的接口则重写原窗口，形成接口重新实现。
            if (entry == interfaceEntries.end()) {
                auto start = uint32_t(type.virtualMethods.size());
                if (start >= (1u << 13)) {
                    throw std::runtime_error("interface method start exceeds TDB81 encoding");
                }
                interfaceEntries.push_back(id | (start << 19));
                type.virtualMethods.insert(type.virtualMethods.end(), implementations.begin(),
                                           implementations.end());
            } else {
                std::copy(implementations.begin(), implementations.end(),
                          type.virtualMethods.begin() + (*entry >> 19));
            }
        }
        if (!interfaceEntries.empty()) {
            // interfaceIndex 不是全局唯一 ID：无关接口也可能复用同一 dispatch prefix 槽。
            // 一个类同时实现它们时，该槽必须指向兼容窗口；不兼容则发布前报错，不改原 AOT 调用点。
            std::unordered_map<int32_t, uint32_t> windows;
            for (auto encoded : interfaceEntries) {
                auto id = encoded & 0x7FFFF;
                auto dispatch = database.types[id].getInterfaceIndex();
                auto [found, inserted] = windows.try_emplace(dispatch, encoded);
                if (inserted) {
                    continue;
                }
                auto previous = found->second & 0x7FFFF;
                auto count = size_t(database.typeImpls[database.types[id].implIndex].numManagedVtable);
                auto previousCount =
                    size_t(database.typeImpls[database.types[previous].implIndex].numManagedVtable);
                for (size_t slot = 0; slot < std::min(count, previousCount); ++slot) {
                    if (type.virtualMethods[(encoded >> 19) + slot] !=
                        type.virtualMethods[(found->second >> 19) + slot]) {
                        throw std::runtime_error(spec.name + ": conflicting interface dispatch index " +
                                                 std::to_string(dispatch) + ": " + database.GetFullName(previous) +
                                                 " / " + database.GetFullName(id));
                    }
                }
                if (count > previousCount) {
                    found->second = encoded;
                }
            }
            for (auto& encoded : interfaceEntries) {
                encoded = (encoded & 0x7FFFF) |
                          (windows.at(database.types[encoded & 0x7FFFF].getInterfaceIndex()) & ~0x7FFFFu);
            }
            std::vector<uint32_t> list{uint32_t(interfaceEntries.size())};
            list.insert(list.end(), interfaceEntries.begin(), interfaceEntries.end());
            definition.interfaceListOffset = database.AppendBytes(list.data(), list.size() * 4);
        }
    }
} // namespace ci
