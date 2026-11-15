#include "ClassProviders.h"
#include <Windows.h>
#include <LogUtils.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ci {
    // 加载TOML唯一对应的DLL，登记函数/枚举编辑，并核对该文件的全部函数契约。
    // 不调用业务函数/SDK 初始化。LoadLibrary 的模块引用保留到进程结束，避免入口指向已卸载代码。
    bool LoadClassProviderBindings(ProviderConfig& config, ClassProviderNotification* notification) {
        try {
            // 扫描阶段已由同目录、同名 TOML 标识 Provider；DLL 名称不再要求 _ci 后缀。
            auto module = LoadLibraryExW(config.dllPath.c_str(), nullptr,
                                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) {
                throw std::runtime_error("DLL load failed: " + config.dllPath.string() + " (" +
                                         std::to_string(GetLastError()) + ")");
            }
            // 不把 API 表交给未知 SDK 布局；DllMain 已由 Windows 执行，这不是模块沙箱。
            const auto getSdkAbi = reinterpret_cast<FnGetSdkAbi>(GetProcAddress(module, "ci_get_sdk_abi"));
            if (!getSdkAbi) {
                throw std::runtime_error("SDK ABI identity missing; expected=" + std::to_string(SDK_ABI_VERSION));
            }
            const auto providerAbi = getSdkAbi();
            if (providerAbi != SDK_ABI_VERSION) {
                throw std::runtime_error("SDK ABI mismatch: expected=" + std::to_string(SDK_ABI_VERSION) +
                                         " actual=" + std::to_string(providerAbi));
            }
            auto registerFunctions =
                reinterpret_cast<FnRegisterProvider>(GetProcAddress(module, "ci_register_provider"));
            if (!registerFunctions) {
                throw std::runtime_error("ci_register_provider entry is missing");
            }
            struct Function {
                void* address;
                uint32_t version;
            };
            using Functions = std::unordered_map<std::string, Function>;
            const auto providerSource = std::make_shared<const ProviderSource>(config.configPath.string(), config.dllPath.string());
            for (auto& type : config.types) type.providerSource = providerSource;
            struct RegistrationState {
                std::shared_ptr<const ProviderSource> providerSource;
                Functions functions;
                std::vector<CustomTypeConfig> enumExtensions;
            } state{.providerSource = providerSource};
            const ProviderRegistrationApi api{
                .context = &state,
                .AddFunction = [](void* context, const char* name, void* address, uint32_t version) {
                    auto& functions = static_cast<RegistrationState*>(context)->functions;
                    if (!name || !*name || !address || !functions.emplace(name, Function{address, version}).second) {
                        _MESSAGE("Invalid/duplicate function registration: %s", name ? name : "<null>");
                        return false;
                    }
                    return true;
                },
                .AddEnumExtension = [](void* context, const char* name, const EnumRename* renames, size_t renameCount, const EnumAppend* appends, size_t appendCount) {
                    if (!name || !*name || (!renameCount && !appendCount) || (renameCount && !renames) || (appendCount && !appends)) return false;
                    CustomTypeConfig type{.name = name, .enumExtension = EnumExtensionSpec{}};
                    for (size_t i = 0; i < renameCount; ++i) {
                        if (!renames[i].to || !*renames[i].to) return false;
                        EnumRenameSpec rename{.to = renames[i].to};
                        switch (renames[i].source) {
                        case EnumRenameSource::Name:
                            if (!renames[i].fromName || !*renames[i].fromName) return false;
                            rename.source = renames[i].fromName;
                            break;
                        case EnumRenameSource::Value:
                            rename.source = renames[i].fromValue;
                            break;
                        default:
                            return false;
                        }
                        type.enumExtension->renames.push_back(std::move(rename));
                    }
                    for (size_t i = 0; i < appendCount; ++i) {
                        if (!appends[i].name || !*appends[i].name) return false;
                        type.enumExtension->appends.push_back({.name = appends[i].name, .value = appends[i].value});
                    }
                    auto& state = *static_cast<RegistrationState*>(context);
                    type.providerSource = state.providerSource;
                    state.enumExtensions.push_back(std::move(type));
                    return true;
                }};
            if (!registerFunctions(&api)) {
                throw std::runtime_error("DLL registration failed");
            }
            for (auto& type : config.types) {
                for (auto& method : type.methods) {
                    auto& binding = method.binding;
                    if (binding.backend != Backend::Cpp) {
                        continue;
                    }
                    auto found = state.functions.find(binding.function);
                    if (found == state.functions.end()) {
                        throw std::runtime_error(type.name + "." + method.name + ": function missing: " + binding.function);
                    }
                    if (binding.version != found->second.version) {
                        throw std::runtime_error(type.name + "." + method.name + ": function version mismatch: " + binding.function +
                                                 " expected=" + std::to_string(binding.version) +
                                                 " actual=" + std::to_string(found->second.version));
                    }
                    binding.address = found->second.address;
                }
            }
            config.types.insert(config.types.end(), std::make_move_iterator(state.enumExtensions.begin()), std::make_move_iterator(state.enumExtensions.end()));
            if (notification) {
                *notification = {reinterpret_cast<FnTdbReady>(GetProcAddress(module, "ci_on_tdb_ready")), providerSource};
            }
            return true;
        } catch (const std::exception& error) {
            _MESSAGE("Plugin skipped: %s: %s", config.configPath.string().c_str(), error.what());
            return false;
        }
    }

    // 检查完整类型名或泛型/数组中的类型名；不将 Arg2 误判为 Arg 的依赖。
    static bool ContainsTypeName(std::string_view typeNameExpression, std::string_view dependencyTypeName) {
        constexpr std::string_view SEPARATORS = "<>,[] &*";
        for (auto offset = typeNameExpression.find(dependencyTypeName); offset != std::string_view::npos;
             offset = typeNameExpression.find(dependencyTypeName, offset + 1)) {
            auto end = offset + dependencyTypeName.size();
            if ((offset == 0 || SEPARATORS.contains(typeNameExpression[offset - 1])) &&
                (end == typeNameExpression.size() || SEPARATORS.contains(typeNameExpression[end]))) {
                return true;
            }
        }
        return false;
    }

    static bool HasTypeDependency(const CustomTypeConfig& config, std::string_view dependencyTypeName) {
        auto ContainsDependencyType = [&](std::string_view typeNameExpression) {
            return ContainsTypeName(typeNameExpression, dependencyTypeName);
        };
        if (ContainsDependencyType(config.base) || ContainsDependencyType(config.genericTemplate) ||
            ContainsDependencyType(config.genericDefinition) ||
            std::ranges::any_of(config.genericArguments, ContainsDependencyType) ||
            std::ranges::any_of(config.interfaces, ContainsDependencyType)) {
            return true;
        }
        for (auto& field : config.fields) {
            if (ContainsDependencyType(field.type)) {
                return true;
            }
        }
        for (auto& method : config.methods) {
            if (ContainsDependencyType(method.returnType) || ContainsDependencyType(method.overrideOf) ||
                std::ranges::any_of(method.parameters, [&](auto& parameter) { return ContainsDependencyType(parameter.type); })) {
                return true;
            }
        }
        return false;
    }

    // 先核对整份文件，再筛掉失效插件及其依赖；此时仍未修改原 TDB 或分配方法跳板。
    std::vector<CustomTypeConfig> ResolveClassProviders(std::vector<ProviderConfig> configs, std::vector<ClassProviderNotification>* callbacks) {
        // 配置不携带加载结果；接受状态与通知仅在这一轮解析 Provider 依赖时存活。
        struct ClassProvider {
            ProviderConfig config;
            bool enabled{true};
            ClassProviderNotification notification{};
        };
        std::vector<ClassProvider> providers;
        providers.reserve(configs.size());
        for (auto& config : std::exchange(configs, {})) providers.push_back({std::move(config)});
        std::unordered_map<std::wstring, size_t> owners;
        for (size_t i = 0; i < providers.size(); ++i) {
            auto path = providers[i].config.dllPath.wstring();
            std::ranges::transform(path, path.begin(), [](wchar_t value) { return wchar_t(towlower(value)); });
            auto [found, inserted] = owners.emplace(path, i);
            if (!inserted) {
                providers[i].enabled = providers[found->second].enabled = false;
                _MESSAGE("Plugin skipped: a DLL must have exactly one TOML: %s", providers[i].config.dllPath.string().c_str());
            }
        }
        for (auto& provider : providers) {
            if (provider.enabled) {
                provider.enabled = LoadClassProviderBindings(provider.config, &provider.notification);
            }
        }
        // 按插件整体传播失效：同文件的其余类型/扩展也跳过，不留下部分注册。
        bool changed;
        do {
            changed = false;
            for (auto& provider : providers) {
                if (!provider.enabled) {
                    continue;
                }
                for (auto& dependency : providers) {
                    if (dependency.enabled) {
                        continue;
                    }
                    for (auto& type : dependency.config.types) {
                        if (!type.IsTypeExtension() && std::ranges::any_of(provider.config.types, [&](auto& candidate) {
                                return HasTypeDependency(candidate, type.name);
                            })) {
                            provider.enabled = false;
                            changed = true;
                            _MESSAGE("Plugin skipped: %s depends on unavailable type %s",
                                     provider.config.configPath.string().c_str(), type.name.c_str());
                            break;
                        }
                    }
                    if (!provider.enabled) {
                        break;
                    }
                }
            }
        } while (changed);

        std::vector<CustomTypeConfig> result;
        for (auto& provider : providers) {
            if (provider.enabled) {
                _MESSAGE("Accepted provider: %s, %zu type declarations", provider.config.configPath.string().c_str(),
                         provider.config.types.size());
                result.insert(result.end(), std::make_move_iterator(provider.config.types.begin()),
                              std::make_move_iterator(provider.config.types.end()));
                // 在整份校验和依赖剔除完成后保留通知；加载过但被拒绝的 DLL 不收到回调。
                if (callbacks && provider.notification.callback) {
                    callbacks->push_back(std::move(provider.notification));
                }
            }
        }
        return result;
    }

    // 只收集顶层同名 TOML / DLL 对并按文件名排序；空标记文件也参与加载登记。
    std::vector<CustomTypeConfig> LoadClassProviders(const std::vector<std::filesystem::path>& directories, std::vector<ClassProviderNotification>* callbacks) {
        std::vector<std::filesystem::path> configPaths;
        for (const auto& directory : directories) {
            if (!std::filesystem::exists(directory)) {
                continue;
            }
            for (auto& entry : std::filesystem::directory_iterator(directory)) {
                if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".toml") == 0) {
                    auto dllPath = entry.path();
                    dllPath.replace_extension(L".dll");
                    if (std::filesystem::is_regular_file(dllPath)) {
                        configPaths.push_back(entry.path());
                    }
                }
            }
        }
        std::ranges::sort(configPaths);
        std::vector<ProviderConfig> configs;
        for (const auto& configPath : configPaths) {
            try {
                std::ifstream stream(configPath, std::ios::binary);
                if (!stream) throw std::runtime_error("TOML file could not be opened");
                std::string configText{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                if (stream.bad()) throw std::runtime_error("TOML file read failed");
                configs.push_back(ParseProviderConfig(configText, configPath));
            } catch (const std::exception& error) {
                _MESSAGE("Plugin TOML skipped: %s: %s", configPath.string().c_str(), error.what());
            }
        }
        return ResolveClassProviders(std::move(configs), callbacks);
    }

    // Consume the callback list before entering provider code; no second notified flag is needed.
    void NotifyClassProviders(std::vector<ClassProviderNotification>& callbacks, const ProviderContext& context) {
        auto pending = std::exchange(callbacks, {});
        for (const auto& notification : pending) {
            if (notification.providerSource) {
                _MESSAGE("Notifying provider: %s [%s]", notification.providerSource->configPath.c_str(), notification.providerSource->dllPath.c_str());
            }
            notification.callback(&context);
        }
    }
} // namespace ci
