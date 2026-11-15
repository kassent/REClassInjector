#pragma once
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace ci {
    enum class MethodKind { Constructor, Instance, Virtual, Override, Abstract };
    enum class Backend { Builtin, Cpp };
    enum class ParameterModifier { Value, Ref, Out };

    // Cpp 地址由所在 TOML 对应的 DLL 登记表解析；Builtin 仅用于自动生成的无参构造链。
    struct Binding {
        Backend backend{Backend::Builtin};
        std::string function;
        uint32_t version{};
        void* address{};
    };
    struct FieldSpec {
        std::string name;
        std::string type{"System.Int32"};
        bool serialize{}; // 交给引擎生成 RSZ 字段描述；默认仍为仅运行时字段。
    };
    // 枚举只编辑已有类型的常量成员，不占实例字段，不生成类型或数组身份。
    struct EnumRenameSpec {
        std::variant<std::string, int32_t> source; // fromName/fromValue选源；EnumMembers 在容量计数前归一为名称。
        std::string to;
    };
    struct EnumAppendSpec {
        std::string name;
        int32_t value{};
    };
    struct EnumExtensionSpec {
        std::vector<EnumRenameSpec> renames;
        std::vector<EnumAppendSpec> appends;
    };
    struct ParameterSpec {
        std::string name;
        std::string type{"System.Int32"};
        ParameterModifier modifier{ParameterModifier::Value}; // ref/out T 接收调用方的 T*。
        ParameterSpec(std::string value) : name(std::move(value)) {}
        ParameterSpec(const char* value) : name(value) {}
        ParameterSpec(std::string value, std::string typeName,
                      ParameterModifier direction = ParameterModifier::Value)
            : name(std::move(value)), type(std::move(typeName)), modifier(direction) {}
    };
    // 同一 Provider 的声明/通知共用来源，仅在构建和通知期保留，不进入运行期元数据。
    struct ProviderSource {
        std::string configPath;
        std::string dllPath;
    };

    struct MethodSpec {
        std::string name;
        MethodKind kind{MethodKind::Instance};
        std::string returnType{"System.Void"};
        std::vector<ParameterSpec> parameters;
        std::string overrideOf;
        Binding binding;
        std::shared_ptr<const ProviderSource> providerSource; // 生成入口前的错误定位，随声明一起释放。
    };
    // 自定义类型配置也包含对已有 class/enum 的扩展，不表示目标一定是新类型。
    // 泛型只注册显式闭合实例：template 复用已有形状，definition 使用本声明的具体字段/方法。
    struct CustomTypeConfig {
        std::string name;
        std::string base{"System.Object"};
        std::vector<FieldSpec> fields;
        std::vector<MethodSpec> methods;
        bool abstract{};
        std::string genericTemplate;
        std::vector<std::string> genericArguments;
        std::vector<std::string> interfaces;
        bool extend{}; // 原版 sealed/无既有子类的纯托管 class：只追加字段，不分配新 ID 或构造入口。
        std::string genericDefinition;
        std::optional<EnumExtensionSpec> enumExtension;
        std::shared_ptr<const ProviderSource> providerSource;

        // 枚举修改独立于 class 的 extend 字段，不再额外设置一份相同状态。
        bool IsTypeExtension() const { return extend || enumExtension.has_value(); }
        // 格式化配置路径、DLL 路径与类型名，供错误和告警定位；不缓存诊断文本。
        std::string FormatDiagnosticContext() const {
            return providerSource ? providerSource->configPath + " [" + providerSource->dllPath + "]: " + name : name;
        }
    };

} // namespace ci
