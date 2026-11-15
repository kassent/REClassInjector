#pragma once
#include <cstddef>
#include <vector>

namespace ci {
    struct TypeDatabase;
    struct CustomTypeConfig;
    struct CustomTypeBuildSpec;
    struct EnumExtensionSpec;
    // 按声明顺序解析 canonical 值源，归一为名称后再预留容量和应用操作。
    void ResolveEnumRenameSources(const TypeDatabase& database, std::vector<CustomTypeBuildSpec>& declarations);
    // 统计归一后的真实改名次数；自改名只校验，不计入新记录容量。
    size_t CountEnumMemberRenames(const EnumExtensionSpec& changes);
    // 保持已有类型/数组身份和常量值，按需复制成员块后改名或追加。
    void ApplyEnumMemberChanges(TypeDatabase& database, const CustomTypeConfig& declaration);
} // namespace ci
