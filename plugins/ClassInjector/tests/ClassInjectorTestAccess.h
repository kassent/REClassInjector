#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace ci {
    struct CustomTypeConfig;
    struct CustomTypeBuildSpec;
    struct ProviderConfig;
    struct ClassProviderNotification;
    struct InvokeFrame;

    // Direct access to existing implementations for offline regression tests.
    // Production translation units do not include this header.
    std::vector<CustomTypeConfig> ParseCustomTypeConfig(std::string_view configText,
                                                       const std::filesystem::path& configPath);
    std::vector<CustomTypeConfig> ResolveClassProviders(std::vector<ProviderConfig> configs,
                                                       std::vector<ClassProviderNotification>* callbacks = nullptr);
    bool LoadClassProviderBindings(ProviderConfig& config, ClassProviderNotification* notification = nullptr);

    uint32_t NameHash(std::string_view value);
    std::vector<CustomTypeBuildSpec> OrderTypeDeclarations(std::vector<CustomTypeBuildSpec> declarations);
    bool InvokeGeneratedMethod(InvokeFrame* frame, void* context);

    namespace TypeInjection {
        bool OnVmTypeTablesBuilt();
        bool OnVmMethodsRegistered();
        extern std::atomic_bool g_ready;
    } // namespace TypeInjection
} // namespace ci
