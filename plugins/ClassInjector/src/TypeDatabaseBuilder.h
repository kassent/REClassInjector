#pragma once
#include "CustomTypeConfig.h"
#include "TypeDatabase.h"

namespace ci {
    // 输入为已 relocation 的 TDB；返回未发布的新表，不修改原表。
    std::unique_ptr<TypeDatabase> BuildTDB(const RE::TDB& original, std::vector<CustomTypeConfig> declarations);
} // namespace ci
