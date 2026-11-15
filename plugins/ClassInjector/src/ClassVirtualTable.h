#pragma once

namespace ci {
    struct TypeDatabase;
    struct RuntimeType;
    // Write already-created managed slots, including inherited entries and interface aliases.
    void UpdateVirtualTable(TypeDatabase& database, const RuntimeType& type);
} // namespace ci
