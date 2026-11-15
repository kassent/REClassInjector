#pragma once

namespace ci {
    struct TypeDatabase;
    // Only creates near native entries; reflection adapters are a separate build step.
    void CreateMethodTrampolines(TypeDatabase& database);
} // namespace ci
