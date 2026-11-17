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
    // 覆盖两份输出，返回本次接受的类型配置和待通知项；已加载模块仍按原策略留驻。
    void LoadClassProviders(const std::vector<std::filesystem::path>& directories,
                            std::vector<CustomTypeConfig>& typeConfigs,
                            std::vector<ClassProviderNotification>& callbacks);
    void NotifyClassProviders(std::vector<ClassProviderNotification>& callbacks, const ProviderContext& context);
} // namespace ci
