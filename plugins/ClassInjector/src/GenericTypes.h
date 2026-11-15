#pragma once
#include <vector>

namespace RE {
    struct RETypeDefinition;
    struct RETypeImpl;
}

namespace ci {
    struct TypeDatabase;
    struct CustomTypeBuildSpec;
    struct RuntimeType;
    // 展开已有闭合泛型的声明形状，并合入本 Provider 的具体方法绑定。
    void ExpandGenericTemplates(TypeDatabase& database, std::vector<CustomTypeBuildSpec>& declarations);
    // 为显式闭合声明补齐共用的开放定义身份，已有定义须满足父类/arity 契约。
    void AddGenericDefinitions(const TypeDatabase& database, std::vector<CustomTypeBuildSpec>& declarations);
    // 构建泛型名称和 descriptor；开放定义清空实例布局，不生成可执行成员。
    void BuildGenericTypeIdentity(TypeDatabase& database, const CustomTypeBuildSpec& declaration, RuntimeType& type,
                                  RE::RETypeDefinition& definition, RE::RETypeImpl& implementation);
} // namespace ci
