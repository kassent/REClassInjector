#pragma once
#include "CustomTypeConfig.h"

namespace ci {
    // 自定义类型的构建记录：复用配置成员，补充构建时生成的类型信息。
    // 配置移入后不另存字段/方法列表；这些记录在数据库构建结束后释放。
    struct CustomTypeBuildSpec : CustomTypeConfig {
        bool isGenericDefinition{};   // 开放泛型定义，不生成实例成员和构造入口。
        std::string arrayElementType; // 自动生成数组的元素类型名；非数组为空。
        std::string requestedByType;  // 引入该自动生成类型的作者类型名，仅用于报错。

        std::string FormatDiagnosticContext() const {
            auto context = CustomTypeConfig::FormatDiagnosticContext();
            if (!requestedByType.empty()) context += " (requested by " + requestedByType + ")";
            return context;
        }
    };
} // namespace ci
