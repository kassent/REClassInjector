#pragma once
#include "CustomTypeConfig.h"
#include <filesystem>
#include <string_view>

namespace ci {
    // 一份 TOML 对应一个实现 DLL；全部绑定通过后才让该文件的类型进入 metadata 构建。
    struct ProviderConfig {
        std::filesystem::path configPath;
        std::filesystem::path dllPath;
        std::vector<CustomTypeConfig> types;
    };

    ProviderConfig ParseProviderConfig(std::string_view configText, const std::filesystem::path& configPath);
} // namespace ci
