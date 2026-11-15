#include "GenericTypes.h"
#include "TypeDatabase.h"
#include "CustomTypeBuildSpec.h"
#include "ClassMethods.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace ci {
    // 仅接受当前入口 ABI 支持的闭合实参，不放宽为任意值类型。
    static void CheckGenericArgument(const TypeDatabase& database, uint32_t index) {
        if (!database.IsReferenceType(index) && database.GetFullName(index) != "System.Int32" && !database.IsInt32Enum(index)) {
            throw std::runtime_error("generic argument must be a managed reference, Int32 or Int32 enum");
        }
        if (database.types[index].typeFlags & 0x08000000) {
            throw std::runtime_error("generic argument must be closed");
        }
    }

    // 将已有闭合泛型的“形状”展开成普通 Spec：替换父类/字段/接口/方法中的实参引用。
    // 不是定义新 open generic 或复制旧 AOT 方法体；所有具体非构造方法都必须提供自己的 binding。
    void ExpandGenericTemplates(TypeDatabase& database, std::vector<CustomTypeBuildSpec>& specs) {
        for (auto& spec : specs) {
            try {
                if (!spec.genericTemplate.empty()) {
                    auto index = database.ResolveType(spec.genericTemplate);
                    auto& definition = database.types[index];
                    if (!definition.genericsOffset) {
                        throw std::runtime_error("generic_template must be a closed class: " + spec.genericTemplate);
                    }
                    auto* generic =
                        reinterpret_cast<const uint32_t*>(database.bytes.data() + definition.genericsOffset);
                    if ((generic[0] & 0x7FFFF) == index ||
                        ((generic[0] >> 19) & 0x7F) != spec.genericArguments.size()) {
                        throw std::runtime_error("closed generic arity mismatch: " + spec.name);
                    }
                    std::vector<std::pair<std::string, std::string_view>> replacements;
                    for (size_t i = 0; i < spec.genericArguments.size(); ++i) {
                        CheckGenericArgument(database, generic[i + 1]);
                        auto from = database.GetFullName(generic[i + 1]);
                        auto found = std::ranges::find_if(replacements, [&](const auto& entry) { return entry.first == from; });
                        if (found == replacements.end()) {
                            replacements.emplace_back(std::move(from), spec.genericArguments[i]);
                        } else if (found->second != spec.genericArguments[i]) {
                            // 闭合记录已丢失重复实参的位置来源，不能猜测它来自哪个泛型参数。
                            throw std::runtime_error("ambiguous generic template argument: " + spec.name + ": " + from);
                        }
                    }
                    // 完整实参优先于其前缀，例如 Object[] 优先于 Object。
                    std::ranges::sort(replacements, std::greater{}, [](const auto& entry) { return entry.first.size(); });
                    auto Substitute = [&](std::string_view name) {
                        std::string result;
                        result.reserve(name.size());
                        for (size_t position = 0; position < name.size();) {
                            const auto start = position;
                            if (position == 0 || name[position - 1] == '<' || name[position - 1] == ',') {
                                for (const auto& [from, to] : replacements) {
                                    if (!name.substr(position).starts_with(from)) continue;
                                    const auto end = position + from.size();
                                    if (end != name.size() && name[end] != '>' && name[end] != ',' && name[end] != '[') continue;
                                    result += to;
                                    position = end;
                                    break;
                                }
                            }
                            // 只扫描原始名称，已经写入 result 的目标类型不再参与替换。
                            if (position == start) result += name[position++];
                        }
                        return result;
                    };
                    spec.base = Substitute(database.GetFullName(uint32_t(definition.parentTypeIndex)));
                    for (auto encoded : database.GetInterfaces(definition)) {
                        spec.interfaces.push_back(Substitute(database.GetFullName(encoded & 0x7FFFF)));
                    }
                    auto& impl = database.typeImpls[definition.implIndex];
                    // 从原记录还原完整声明再替换类型；原 abstract 可由用户提供 virtual binding 实现。
                    for (size_t i = 0; i < impl.numMemberFields; ++i) {
                        auto& field = database.fields[definition.memberFieldIndex + i];
                        auto& fieldImpl = database.fieldImpls[field.impl];
                        if (fieldImpl.flags & 0x10) {
                            throw std::runtime_error("closed generic static fields are not supported");
                        }
                        spec.fields.push_back({database.strings.data() + fieldImpl.name,
                                               Substitute(database.GetFullName(uint32_t(field.type)))});
                    }
                    std::vector<MethodSpec> methods;
                    for (size_t i = 0; i < impl.numMemberMethods; ++i) {
                        auto& method = database.methods[definition.memberMethodIndex + i];
                        auto& methodImpl = database.methodImpls[method.impl];
                        if (methodImpl.flags & 0x10) {
                            throw std::runtime_error("closed generic static methods are not supported");
                        }
                        auto specMethod = ReadMethodDeclaration(database, uint32_t(definition.memberMethodIndex) + uint32_t(i));
                        specMethod.returnType = Substitute(specMethod.returnType);
                        for (auto& parameter : specMethod.parameters) {
                            parameter.type = Substitute(parameter.type);
                        }
                        auto supplied = std::ranges::find_if(spec.methods, [&](auto& m) {
                            if (m.name != specMethod.name || m.parameters.size() != specMethod.parameters.size()) {
                                return false;
                            }
                            for (size_t j = 0; j < m.parameters.size(); ++j) {
                                if (m.parameters[j].type != specMethod.parameters[j].type ||
                                    m.parameters[j].modifier != specMethod.parameters[j].modifier) {
                                    return false;
                                }
                            }
                            return true;
                        });
                        if (supplied != spec.methods.end()) {
                            if (supplied->returnType != specMethod.returnType) {
                                throw std::runtime_error("generic template return type mismatch: " + supplied->name);
                            }
                            // 签名匹配后保留声明参数名，原模板的名字可能已被裁剪。
                            for (size_t j = 0; j < specMethod.parameters.size(); ++j) {
                                specMethod.parameters[j].name = supplied->parameters[j].name;
                            }
                            specMethod.binding = supplied->binding;
                            if (specMethod.kind == MethodKind::Abstract && supplied->kind != MethodKind::Abstract) {
                                specMethod.kind = MethodKind::Virtual;
                            }
                            spec.methods.erase(supplied);
                        } else if (specMethod.kind != MethodKind::Constructor &&
                                   specMethod.kind != MethodKind::Abstract) {
                            throw std::runtime_error("closed generic method requires its own C++ binding: " +
                                                     spec.name + "." + specMethod.name);
                        }
                        methods.push_back(std::move(specMethod));
                    }
                    if (!spec.methods.empty()) {
                        throw std::runtime_error("generic template method not found: " + spec.methods.front().name);
                    }
                    spec.methods = std::move(methods);
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
    }

    // 显式闭合声明共用一条 definition 身份记录，不提供开放成员或动态 MakeGenericType。
    // 原 builder 的 08000000h 路径跳过开放定义的 TypeInfo/ObjectInfo 构建，仍创建 RuntimeType。
    void AddGenericDefinitions(const TypeDatabase& database, std::vector<CustomTypeBuildSpec>& specs) {
        std::unordered_map<std::string, size_t> definitions;
        std::vector<CustomTypeBuildSpec> generated;
        for (auto& spec : specs) {
            try {
                if (spec.genericDefinition.empty()) {
                    continue;
                }
                auto arity = spec.genericArguments.size();
                if (!arity || arity > 127 || !spec.genericDefinition.ends_with('`' + std::to_string(arity))) {
                    throw std::runtime_error("generic definition name/arity mismatch: " + spec.name);
                }
                if (auto found = database.typeNames.find(spec.genericDefinition); found != database.typeNames.end()) {
                    auto& type = database.types[found->second];
                    auto* descriptor = reinterpret_cast<const uint32_t*>(database.bytes.data() + type.genericsOffset);
                    if (!type.genericsOffset || type.objectType != uint32_t(via::clr::VMObjType::Object) || !(type.typeFlags & 0x08000000) ||
                        (descriptor[0] & 0x7FFFF) != found->second || ((descriptor[0] >> 19) & 127) != arity ||
                        database.GetFullName(uint32_t(type.parentTypeIndex)) != spec.base) {
                        throw std::runtime_error("incompatible existing generic definition: " + spec.genericDefinition);
                    }
                    continue;
                }
                auto [found, inserted] = definitions.emplace(spec.genericDefinition, generated.size());
                if (inserted) {
                    CustomTypeBuildSpec definition{CustomTypeConfig{.name = spec.genericDefinition, .base = spec.base, .abstract = true,
                                                      .genericArguments = std::vector<std::string>(arity), .providerSource = spec.providerSource}};
                    definition.isGenericDefinition = true;
                    definition.requestedByType = spec.name;
                    generated.push_back(std::move(definition));
                } else if (generated[found->second].base != spec.base ||
                           generated[found->second].genericArguments.size() != arity) {
                    throw std::runtime_error("generic definitions must share base and arity; first declared by " + generated[found->second].FormatDiagnosticContext());
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        specs.insert(specs.end(), std::make_move_iterator(generated.begin()),
                     std::make_move_iterator(generated.end()));
    }

    // 写入开放/闭合泛型身份；成员布局仍走 class 构建流程，不复制 AOT 方法体。
    void BuildGenericTypeIdentity(TypeDatabase& database, const CustomTypeBuildSpec& spec, RuntimeType& type,
                                  RE::RETypeDefinition& definition, RE::RETypeImpl& impl) {
        if (spec.isGenericDefinition) {
            const auto dot = spec.name.rfind('.');
            impl.nameOffset = database.AppendString(std::string_view(spec.name).substr(dot + 1));
            impl.namespaceOffset = database.AppendString(std::string_view(spec.name).substr(0, dot));
            definition.declaringTypeIndex = 0;
            // 开放 descriptor：头部 8B，随后每参数为 constraint/attributes + nameOffset（8B）。
            // 不发布可执行成员、字段布局或 default constructor；闭合实例单独构建这些记录。
            definition.typeFlags |= 0x08000000;
            impl.fieldSize = 0;
            impl.numMemberFields = impl.numMemberMethods = 0;
            std::vector<uint32_t> descriptor(2 + spec.genericArguments.size() * 2);
            descriptor[0] = type.index | uint32_t(spec.genericArguments.size() << 19);
            for (size_t i = 0; i < spec.genericArguments.size(); ++i) {
                descriptor[3 + i * 2] = database.AppendString(spec.genericArguments.size() == 1 ? "T" : "T" + std::to_string(i));
            }
            definition.genericsOffset = database.AppendBytes(descriptor.data(), descriptor.size() * 4);
        } else {
            auto source = database.types[database.ResolveType(
                spec.genericTemplate.empty() ? spec.genericDefinition : spec.genericTemplate)];
            auto sourceImpl = database.typeImpls[source.implIndex];
            impl.nameOffset = sourceImpl.nameOffset;
            impl.namespaceOffset = sourceImpl.namespaceOffset;
            definition.declaringTypeIndex = source.declaringTypeIndex;
            auto header = *reinterpret_cast<const uint32_t*>(database.bytes.data() + source.genericsOffset);
            type.genericDefinition = header & 0x7FFFF;
            // 泛型头低 19 位关联 definition，高位记录实参数量；后续条目为实参 type ID。
            std::vector<uint32_t> descriptor{type.genericDefinition | uint32_t(spec.genericArguments.size() << 19)};
            std::string expected = database.GetFullName(type.genericDefinition) + '<';
            for (size_t i = 0; i < spec.genericArguments.size(); ++i) {
                auto arg = database.ResolveType(spec.genericArguments[i]);
                CheckGenericArgument(database, arg);
                descriptor.push_back(arg);
                if (i) {
                    expected += ',';
                }
                expected += spec.genericArguments[i];
            }
            expected += '>';
            if (expected != spec.name) {
                throw std::runtime_error("closed generic name must be " + expected);
            }
            definition.genericsOffset = database.AppendBytes(descriptor.data(), descriptor.size() * 4);
        }
    }
} // namespace ci
