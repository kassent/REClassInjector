#pragma once
#include <cstdint>

// 测试 DLL 记录真正收到的原生参数；覆盖 vmctx / this、指针宽度和隐藏返回缓冲后的栈参数。
struct NativeCallProbe {
    void* vmctx;
    void* self;
    uintptr_t first;
    uintptr_t second;
    uintptr_t third;
};
using FnGetNativeCallProbe = const NativeCallProbe* (*)();

// 早期通知只缓存元数据/入口，不在 callback 中访问 SDK 或游戏对象。
struct EarlyQueryProbe {
    uint32_t notifications;
    bool sdkInitialized;
    bool sdkInitializedAtReady;
    const void* context;
    const void* originalType;
    const void* customType;
    void* originalMethod;
    void* originalOverload;
    void* echo;
    void* outId;
    void* inheritedEcho;
    void* constructor;
    void* genericMethod;
    bool missingQueriesAreNull;
};
using FnGetEarlyQueryProbe = const EarlyQueryProbe* (*)();
