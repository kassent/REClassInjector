#include "ClassProvider.h"
#include "NativeCallProbe.h"
#include <GameTypes.h>
#include <reframework/API.hpp>
#include <immintrin.h>

using ManagedObject = reframework::API::ManagedObject;

namespace {
    NativeCallProbe g_call{};
    EarlyQueryProbe g_queries{};

    struct CommandResult {
        int32_t value;
        bool flag;
        uint8_t padding[3];
        int32_t type;
    };
    static_assert(sizeof(CommandResult) == 12);
} // namespace
// 此 DLL 在 CreateWindow 阶段加载；SDK 初始化另由测试 bootstrap 的标准 RF 入口触发。
// 初始化发生在 SDK 就绪后，不把日常字段赋值交给 ClassInjector。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    g_queries.sdkInitialized = true;
    reframework::API::initialize(parameter);
    return true;
}
extern "C" __declspec(dllexport) const NativeCallProbe* GetNativeCallProbe() noexcept {
    return &g_call;
}
extern "C" __declspec(dllexport) const EarlyQueryProbe* GetEarlyQueryProbe() noexcept {
    return &g_queries;
}
extern "C" __declspec(dllexport) void ci_on_tdb_ready(const ci::ProviderContext* context) noexcept {
    ++g_queries.notifications;
    g_queries.sdkInitializedAtReady = g_queries.sdkInitialized;
    g_queries.context = context;
    auto* prefab = context->GetType("via.Prefab");
    auto* arg = context->GetType("ClassInjector.Tests.Arg");
    auto* derived = context->GetType("ClassInjector.Tests.DerivedArg");
    auto* cmd = context->GetType("ClassInjector.Tests.Cmd");
    g_queries.originalType = prefab;
    g_queries.customType = arg;
    g_queries.originalMethod = context->GetMethod(prefab, "instantiate(via.vec3)");
    g_queries.originalOverload = context->GetMethod(prefab, "instantiate(via.vec3, via.Folder)");
    g_queries.echo = context->GetMethod(arg, "Echo(ClassInjector.Tests.Arg)");
    g_queries.outId = context->GetMethod(arg, "TryGetId(System.Int32, System.Int32)");
    g_queries.inheritedEcho = context->GetMethod(derived, "Echo(ClassInjector.Tests.Arg)");
    g_queries.constructor = context->GetMethod(arg, ".ctor()");
    g_queries.genericMethod = context->GetMethod(cmd, "onExecute(ace.btable.cCommandWork, ClassInjector.Tests.Arg)");
    g_queries.missingQueriesAreNull = !context->GetType("Tests.Missing") && !context->GetMethod(arg, "Missing") &&
                                      !context->GetMethod(arg, "TryGetId(System.Int32, System.Int32&)");
}
ManagedObject* WideEcho(void* vmctx, ManagedObject* self, ManagedObject* argument) noexcept {
    g_call = {vmctx, self, uintptr_t(argument), 0};
    return argument;
}
// MSVC Win64 将此 12 字节 POD 返回编译为 (out, vmctx, this, work, argument)，argument 位于栈上。
CommandResult AggregateProbe(void* vmctx, ManagedObject* self, ManagedObject* work, ManagedObject* argument) noexcept {
    g_call = {vmctx, self, uintptr_t(work), uintptr_t(argument)};
    return {42, true, {}, 7};
}
void SetPeer(void* vmctx, ManagedObject* self, ManagedObject* argument) noexcept {
    RE::set_managed_field(self, "Peer", argument);
}
void SetText(void* vmctx, ManagedObject* self, ManagedObject* text) noexcept {
    RE::set_managed_field(self, "Text", text);
}
// 扩展字段的实机探针：赋值仍使用公共引擎路径，不给生产 host 添加字段服务。
void SetExtensionPeer(void*, ManagedObject*, ManagedObject* target, ManagedObject* peer) noexcept {
    RE::set_managed_field(target, "CIPeer", peer);
}
void OnExecuteProbe(void* vmctx, ManagedObject* self, ManagedObject* work, ManagedObject* argument) noexcept {
    *self->get_field<int32_t>("Counter") += *argument->get_field<int32_t>("Value");
    RE::set_managed_field(self, "LastArg", argument);
}

bool TryGetId(void* vmctx, ManagedObject* self, int32_t fixedId, int32_t* id) noexcept {
    g_call = {vmctx, self, uintptr_t(fixedId), uintptr_t(id)};
    *id = fixedId >= 0 ? fixedId + 1000 : 0;
    return fixedId >= 0;
}
bool TryGetScale(void* vmctx, ManagedObject* self, int32_t value, float* scale) noexcept {
    g_call = {vmctx, self, uintptr_t(value), uintptr_t(scale)};
    *scale = float(value) * 0.25f;
    return true;
}
bool TryGetPeer(void* vmctx, ManagedObject* self, int32_t value, ManagedObject** peer) noexcept {
    g_call = {vmctx, self, uintptr_t(value), uintptr_t(peer)};
    *peer = value >= 0 ? self : nullptr;
    return value >= 0;
}
bool TryGetPair(void*, ManagedObject*, int32_t* first, int32_t* second) noexcept {
    *first = -17;
    *second = 42;
    return true;
}
bool TryGetNumber(void*, ManagedObject*, int32_t* number) noexcept {
    *number = 17;
    return true;
}
bool TryGetDerivedNumber(void*, ManagedObject*, int32_t* number) noexcept {
    *number = 99;
    return true;
}

// 与 ace.btable.BTableDef.POSITION 的内嵌布局一致；ref 传入现有数据而不是重新初始化它。
struct BTablePosition {
    int32_t runtime;
    int32_t table;
    int32_t row;
};
static_assert(sizeof(BTablePosition) == 12);
void UpdateTableInplProbe(void* vmctx, ManagedObject* self, BTablePosition* position,
                         ManagedObject* work, ManagedObject* opWork) noexcept {
    g_call = {vmctx, self, uintptr_t(position), uintptr_t(work), uintptr_t(opWork)};
    position->runtime += 10;
    position->table += 20;
    position->row += 30;
}
void DerivedUpdateTableInplProbe(void* vmctx, ManagedObject* self, BTablePosition* position,
                                ManagedObject* work, ManagedObject* opWork) noexcept {
    g_call = {vmctx, self, uintptr_t(position), uintptr_t(work), uintptr_t(opWork)};
    position->runtime += 100;
    position->table += 200;
    position->row += 300;
}

// 自定义组件探针只记次数，不主动调用生命周期、不改 Behavior callback mask。
// createComponent 的原生注册路径应依据 TOML 的 override 元数据自动完成调度。
void BehaviorAwake(void*, ManagedObject* self) noexcept {
    ++*self->get_field<int32_t>("AwakeCount");
    *self->get_field<int32_t>("AwakeValue") = *self->get_field<int32_t>("ResourceValue");
}
void BehaviorStart(void*, ManagedObject* self) noexcept {
    ++*self->get_field<int32_t>("StartCount");
}
void BehaviorUpdate(void*, ManagedObject* self) noexcept {
    ++*self->get_field<int32_t>("UpdateCount");
}
void BehaviorLateUpdate(void*, ManagedObject* self) noexcept {
    ++*self->get_field<int32_t>("LateUpdateCount");
}
void BehaviorOnDestroy(void*, ManagedObject* self) noexcept {
    ++*self->get_field<int32_t>("DestroyCount");
}

// 值类型字段探针：直接访问内嵌数据，并实际执行要求 16B 对齐的 SIMD 读写。
// 返回 -1 用于报告布局错误，不在旧的未对齐布局上触发访问异常。
int32_t FillValueFields(void*, ManagedObject*, ManagedObject* target, int32_t seed) noexcept {
    auto* position = target->get_field<float>("CIPosition");
    auto* offset = target->get_field<float>("CIOffset");
    if ((uintptr_t(position) | uintptr_t(offset)) & 15) {
        return -1;
    }
    *target->get_field<int32_t>("CICounter") = seed + 100;
    *target->get_field<int32_t>("CIMiddle") = seed + 200;
    *target->get_field<int32_t>("CITail") = seed + 300;
    _mm_store_ps(position, _mm_set_ps(0, float(seed) * 2 + 0.75f, -float(seed) - 0.5f, float(seed) + 0.25f));
    auto value = _mm_load_ps(position);
    _mm_store_ps(offset, _mm_add_ps(value, value));
    struct Guid {
        uint32_t data1;
        uint16_t data2, data3;
        uint64_t data4;
    };
    static_assert(sizeof(Guid) == 16);
    *target->get_field<Guid>("CIGuid") = {uint32_t(seed), 0x4455, 0x6677, 0xFFEEDDCCBBAA9988};
    return 1;
}

CI_REGISTER_PROVIDER(registration) {
    registration.RegisterFunction("UpdateTableInplProbe", &UpdateTableInplProbe, 1);
    registration.RegisterFunction("DerivedUpdateTableInplProbe", &DerivedUpdateTableInplProbe, 1);
    registration.RegisterFunction("WideEcho", &WideEcho, 1)
        .RegisterFunction("AggregateProbe", &AggregateProbe, 1)
        .RegisterFunction("SetPeer", &SetPeer, 1)
        .RegisterFunction("SetText", &SetText, 1)
        .RegisterFunction("SetExtensionPeer", &SetExtensionPeer, 1)
        .RegisterFunction("OnExecuteProbe", &OnExecuteProbe, 1)
        .RegisterFunction("TryGetId", &TryGetId, 1)
        .RegisterFunction("TryGetScale", &TryGetScale, 1)
        .RegisterFunction("TryGetPeer", &TryGetPeer, 1)
        .RegisterFunction("TryGetPair", &TryGetPair, 1)
        .RegisterFunction("TryGetNumber", &TryGetNumber, 1)
        .RegisterFunction("TryGetDerivedNumber", &TryGetDerivedNumber, 1)
        .RegisterFunction("BehaviorAwake", &BehaviorAwake, 1)
        .RegisterFunction("BehaviorStart", &BehaviorStart, 1)
        .RegisterFunction("BehaviorUpdate", &BehaviorUpdate, 1)
        .RegisterFunction("BehaviorLateUpdate", &BehaviorLateUpdate, 1)
        .RegisterFunction("BehaviorOnDestroy", &BehaviorOnDestroy, 1)
        .RegisterFunction("FillValueFields", &FillValueFields, 1);
}
