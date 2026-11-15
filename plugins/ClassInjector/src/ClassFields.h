#pragma once

namespace RE {
    struct RETypeDefinition;
    struct RETypeImpl;
}

namespace ci {
    struct TypeDatabase;
    struct CustomTypeConfig;
    // 已验证声明 + 完整源布局 → 追加字段记录及引用/循环位图；不创建运行时对象。
    void BuildClassFields(TypeDatabase& database, const CustomTypeConfig& config,
                          const RE::RETypeDefinition& typeDef, RE::RETypeImpl& typeImpl,
                          const RE::RETypeDefinition& sourceTypeDef);
    // 在原 class 尾部扩展字段，保留既有偏移与方法；共享 TypeImpl 使用局部副本。
    void ExtendClassFields(TypeDatabase& database, const CustomTypeConfig& config);
    // 所有池追加完成后，重定位原 TypeImpl 中指向旧 byte pool 的外置位图。
    void RebaseReferenceBitmaps(TypeDatabase& database);
} // namespace ci
