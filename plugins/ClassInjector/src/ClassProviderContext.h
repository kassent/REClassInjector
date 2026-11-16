#pragma once
#include <ci/Provider.h>

namespace ci {
    // 服务表地址进程期稳定；原生函数在启动窗口解析。
    const ProviderContext& GetContext();
    void PrepareContext();
} // namespace ci
