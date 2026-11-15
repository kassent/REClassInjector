#pragma once
#include <memory>

namespace ci {
    struct TypeDatabase;
    extern std::unique_ptr<TypeDatabase> g_database;
} // namespace ci
