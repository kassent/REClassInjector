#pragma once
#include "ProviderConfig.h"
#include <ci/Provider.h>

namespace ci {
    // 待通知记录持有 Provider 身份；通知时消费，不再另存匿名函数指针列表。
    struct ClassProviderNotification {
        FnInitializationCallback initialize;
        std::shared_ptr<const ProviderSource> providerSource;
        FnInitializationCallback postInitialize{};
    };
    // Return accepted declarations; modules remain loaded while their native entries can be called.
    std::vector<CustomTypeConfig> LoadClassProviders(const std::vector<std::filesystem::path>& directories,
                                                     std::vector<ClassProviderNotification>* callbacks = nullptr);
    void NotifyClassProviders(std::vector<ClassProviderNotification>& callbacks, const ProviderContext& context);
} // namespace ci
