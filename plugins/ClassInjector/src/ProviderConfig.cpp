#include "ProviderConfig.h"
#include <LogUtils.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <stdexcept>

namespace ci {
    using namespace std::string_literals;
    static std::string RequireString(const toml::table& table, std::string_view key) {
        auto value = table[key].value<std::string>();
        if (!value || value->empty()) {
            throw std::runtime_error("missing string: " + std::string(key));
        }
        return *value;
    }
    static MethodKind ParseMethodKind(std::string_view value) {
        if (value == "constructor") {
            return MethodKind::Constructor;
        }
        if (value == "instance") {
            return MethodKind::Instance;
        }
        if (value == "virtual") {
            return MethodKind::Virtual;
        }
        if (value == "override") {
            return MethodKind::Override;
        }
        if (value == "abstract") {
            return MethodKind::Abstract;
        }
        throw std::runtime_error("invalid method kind: " + std::string(value));
    }
    static ParameterModifier ParseParameterModifier(std::string_view value) {
        if (value.empty()) {
            return ParameterModifier::Value;
        }
        if (value == "ref") {
            return ParameterModifier::Ref;
        }
        if (value == "out") {
            return ParameterModifier::Out;
        }
        throw std::runtime_error("parameter modifier must be ref or out: " + std::string(value));
    }
    // 每个函数显式指定契约版本；DLL 由同名 TOML 确定，业务函数不通过导出名查找。
    static Binding ParseBinding(const toml::table& method) {
        auto* table = method["binding"].as_table();
        if (!table) {
            throw std::runtime_error("method binding is required");
        }
        if (table->contains("dll") || table->contains("backend")) {
            throw std::runtime_error("binding only accepts function and version");
        }
        auto version = (*table)["version"].value_exact<int64_t>();
        if (!version || *version < 0 || *version > UINT32_MAX) {
            throw std::runtime_error("binding.version must be an unsigned 32-bit integer");
        }
        return {
            .backend = Backend::Cpp, .function = RequireString(*table, "function"), .version = uint32_t(*version)};
    }

    // 缺省才使用默认值；显式填写的错误类型不降级为缺省。
    template <class T>
    static T ReadOptionalValue(const toml::table& table, std::string_view key, T fallback) {
        const auto node = table[key];
        if (!node) return fallback;
        auto value = node.value_exact<T>();
        if (!value) throw std::runtime_error("invalid value type: " + std::string(key));
        return std::move(*value);
    }

    static const toml::array* ReadOptionalArray(const toml::table& table, std::string_view key) {
        const auto node = table[key];
        if (!node) return nullptr;
        auto* array = node.as_array();
        if (!array) throw std::runtime_error(std::string(key) + " must be an array");
        return array;
    }

    // 从已解析的 TOML 读取自定义类型配置；这里只校验语法和构造签名，类型/ABI 兼容性随后检查。
    static std::vector<CustomTypeConfig> ReadCustomTypeConfig(const toml::table& document, const std::filesystem::path& configPath) {
        std::vector<CustomTypeConfig> result;
        auto dllPath = std::filesystem::absolute(configPath).lexically_normal();
        dllPath.replace_extension(L".dll");
        const auto providerSource = std::make_shared<const ProviderSource>(configPath.string(), dllPath.string());
        if (document["schema_version"].value_exact<int64_t>() != 1) {
            throw std::runtime_error(configPath.string() + ": schema_version must be 1");
        }
        auto* types = document["types"].as_array();
        if (document.contains("types") && !types) {
            throw std::runtime_error(configPath.string() + ": types must be an array");
        }
        if (!types && !document.contains("enums")) {
            throw std::runtime_error(configPath.string() + ": types array is required");
        }
        if (types) {
            for (auto& node : *types) {
                auto* table = node.as_table();
                if (!table) {
                    throw std::runtime_error("types entry must be a table");
                }
                CustomTypeConfig type{.name = RequireString(*table, "name")};
                type.providerSource = providerSource;
                try {
                    type.extend = ReadOptionalValue(*table, "extend", false);
                    if (type.extend) {
                        // 原类型扩展只使用字段；多余的 class 属性告警后忽略，不丢弃整份声明。
                        for (auto key : {"base", "abstract", "generic_template", "generic_definition", "generic_arguments",
                                         "interfaces", "methods"}) {
                            if (table->contains(key)) {
                                _MESSAGE("Warning: %s: ignoring '%s' for field extension", type.FormatDiagnosticContext().c_str(), key);
                            }
                        }
                    }
                    if (auto* fields = ReadOptionalArray(*table, "fields")) {
                        for (auto& fieldNode : *fields) {
                            auto* field = fieldNode.as_table();
                            if (!field) {
                                throw std::runtime_error("field entry must be a table");
                            }
                            type.fields.push_back(
                                {.name = RequireString(*field, "name"),
                                 .type = RequireString(*field, "type"),
                                 .serialize = ReadOptionalValue(*field, "serialize", false)});
                        }
                    }
                    if (type.extend) {
                        if (type.fields.empty()) throw std::runtime_error(type.name + ": extend requires fields");
                        result.push_back(std::move(type));
                        continue;
                    }
                    // 以下仅处理新增 class；字段扩展不经过泛型、接口、方法或默认构造逻辑。
                    type.base = ReadOptionalValue(*table, "base", "System.Object"s);
                    type.abstract = ReadOptionalValue(*table, "abstract", false);
                    type.genericTemplate = ReadOptionalValue(*table, "generic_template", ""s);
                    type.genericDefinition = ReadOptionalValue(*table, "generic_definition", ""s);
                    if (!type.genericTemplate.empty() && !type.genericDefinition.empty()) {
                        throw std::runtime_error("choose generic_template or generic_definition: " + type.name);
                    }
                    if (auto* interfaces = ReadOptionalArray(*table, "interfaces")) {
                        for (auto& interfaceNode : *interfaces) {
                            auto value = interfaceNode.value<std::string>();
                            if (!value || value->empty()) {
                                throw std::runtime_error("interface must be a type name");
                            }
                            type.interfaces.push_back(*value);
                        }
                    }
                    if (auto* arguments = ReadOptionalArray(*table, "generic_arguments")) {
                        for (auto& argument : *arguments) {
                            auto value = argument.value<std::string>();
                            if (!value) {
                                throw std::runtime_error("generic argument must be a type name");
                            }
                            type.genericArguments.push_back(*value);
                        }
                    }
                    if ((!type.genericTemplate.empty() || !type.genericDefinition.empty()) != !type.genericArguments.empty()) {
                        throw std::runtime_error("generic declarations require definition/template and arguments: " + type.name);
                    }
                    if (auto* methods = ReadOptionalArray(*table, "methods")) {
                        for (auto& methodNode : *methods) {
                            auto* table = methodNode.as_table();
                            if (!table) {
                                throw std::runtime_error("method entry must be a table");
                            }
                            MethodSpec method{.name = RequireString(*table, "name"),
                                              .kind = ParseMethodKind(ReadOptionalValue(*table, "kind", "instance"s))};
                            auto returnType = ReadOptionalValue(*table, "return_type", "System.Void"s);
                            method.returnType = returnType;
                            if (auto* parameters = ReadOptionalArray(*table, "parameters")) {
                                for (auto& parameter : *parameters) {
                                    auto* parameterTable = parameter.as_table();
                                    if (!parameterTable) {
                                        throw std::runtime_error("parameter entry must be a table");
                                    }
                                    auto modifier = parameterTable->contains("modifier")
                                                        ? RequireString(*parameterTable, "modifier")
                                                        : ""s;
                                    method.parameters.emplace_back(RequireString(*parameterTable, "name"),
                                                                   RequireString(*parameterTable, "type"),
                                                                   ParseParameterModifier(modifier));
                                }
                            }
                            if ((method.name == ".ctor") != (method.kind == MethodKind::Constructor) ||
                                (method.kind == MethodKind::Constructor && returnType != "System.Void")) {
                                throw std::runtime_error("constructor signature must be Void .ctor(parameters)");
                            }
                            method.overrideOf = ReadOptionalValue(*table, "override_of", ""s);
                            if (method.kind != MethodKind::Abstract) {
                                method.binding = ParseBinding(*table);
                            }
                            type.methods.push_back(std::move(method));
                        }
                    }
                    // 只有完全省略构造才生成 Builtin .ctor()；显式带参构造不暗中补默认构造。
                    if (std::ranges::none_of(type.methods, [](auto& method) {
                            return method.kind == MethodKind::Constructor;
                        })) {
                        type.methods.insert(type.methods.begin(), MethodSpec{.name = ".ctor", .kind = MethodKind::Constructor});
                    }
                    result.push_back(std::move(type));
                } catch (const std::runtime_error& error) {
                    throw std::runtime_error(type.FormatDiagnosticContext() + ": " + error.what());
                }
            }
        }
        if (document.contains("enums")) {
            auto* extensions = document["enums"].as_array();
            if (!extensions) throw std::runtime_error("enums must be an array");
            for (auto& node : *extensions) {
                auto* table = node.as_table();
                if (!table) throw std::runtime_error("enum extension must be a table");
                CustomTypeConfig type{.name = RequireString(*table, "name"), .enumExtension = EnumExtensionSpec{}};
                type.providerSource = providerSource;
                try {
                    auto ReadInt32 = [](const toml::table& item, std::string_view key) {
                        if (!item[key].is_integer()) throw std::runtime_error("enum value must be Int32");
                        auto value = item[key].value<int64_t>();
                        if (!value || *value < INT32_MIN || *value > INT32_MAX) throw std::runtime_error("enum value must be Int32");
                        return int32_t(*value);
                    };
                    if (table->contains("renames")) {
                        auto* renames = (*table)["renames"].as_array();
                        if (!renames) throw std::runtime_error("enum renames must be an array");
                        for (auto& item : *renames) {
                            auto* rename = item.as_table();
                            if (!rename) throw std::runtime_error("enum rename must be a table");
                            if (rename->contains("from")) throw std::runtime_error("enum rename uses fromName or fromValue, not from");
                            if (rename->contains("fromName") == rename->contains("fromValue")) {
                                throw std::runtime_error("enum rename requires exactly one of fromName/fromValue");
                            }
                            EnumRenameSpec value{.to = RequireString(*rename, "to")};
                            if (rename->contains("fromName")) value.source = RequireString(*rename, "fromName");
                            else value.source = ReadInt32(*rename, "fromValue");
                            type.enumExtension->renames.push_back(std::move(value));
                        }
                    }
                    if (table->contains("appends")) {
                        auto* appends = (*table)["appends"].as_array();
                        if (!appends) throw std::runtime_error("enum appends must be an array");
                        for (auto& item : *appends) {
                            auto* append = item.as_table();
                            if (!append) throw std::runtime_error("enum append must be a table");
                            type.enumExtension->appends.push_back({.name = RequireString(*append, "name"), .value = ReadInt32(*append, "value")});
                        }
                    }
                    if (type.enumExtension->renames.empty() && type.enumExtension->appends.empty()) throw std::runtime_error("empty enum extension");
                    result.push_back(std::move(type));
                } catch (const std::runtime_error& error) {
                    throw std::runtime_error(type.FormatDiagnosticContext() + ": " + error.what());
                }
            }
        }
        return result;
    }

    // 文本入口供离线类型配置测试复用；实际加载必须经过 ParseProviderConfig / ResolveClassProviders。
    std::vector<CustomTypeConfig> ParseCustomTypeConfig(std::string_view configText, const std::filesystem::path& configPath) {
        return ReadCustomTypeConfig(toml::parse(configText, configPath.string()), configPath);
    }

    ProviderConfig ParseProviderConfig(std::string_view configText, const std::filesystem::path& configPath) {
        auto dllPath = std::filesystem::absolute(configPath).lexically_normal();
        dllPath.replace_extension(L".dll");
        ProviderConfig config{.configPath = configPath, .dllPath = std::move(dllPath)};
        // TOML 的存在即表示参与 CI 加载；内容只用于可选类型声明，不再决定 DLL 路径。
        try {
            auto document = toml::parse(configText, configPath.string());
            if (document.contains("types") || document.contains("enums")) {
                config.types = ReadCustomTypeConfig(document, configPath);
            }
        } catch (const std::exception& error) {
            config.types.clear();
            _MESSAGE("Plugin declarations skipped: %s: %s", configPath.string().c_str(), error.what());
        }
        return config;
    }

} // namespace ci
