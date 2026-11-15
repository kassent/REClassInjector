#include "ClassProvider.h"
#include <reframework/API.hpp>

using ManagedObject = reframework::API::ManagedObject;
using FnObjectCtor = void (*)(void*, ManagedObject*);
static FnObjectCtor ObjectCtor{};

// 父构造由插件选择；CI 只保存完整构造函数的原生入口。
extern "C" __declspec(dllexport) void ci_on_tdb_ready(const ci::ProviderContext* context) noexcept {
    ObjectCtor = reinterpret_cast<FnObjectCtor>(context->GetMethod(context->GetType("System.Object"), ".ctor()"));
}

// 由调用方在 SDK 就绪后调用标准 RF 入口；早期类型描述不访问 SDK。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    return true;
}
void BaseCtor(void* vmctx, ManagedObject* self) noexcept {
    ObjectCtor(vmctx, self);
    *self->get_field<int32_t>("Counter") = 10;
}
void DerivedCtor(void* vmctx, ManagedObject* self) noexcept {
    BaseCtor(vmctx, self);
    *self->get_field<int32_t>("Bonus") = 100;
}
int32_t Add(void* vmctx, ManagedObject* self, int32_t value) noexcept {
    auto* counter = self->get_field<int32_t>("Counter");
    *counter += value;
    return *counter;
}
int32_t BaseEvaluate(void* vmctx, ManagedObject* self) noexcept { return *self->get_field<int32_t>("Counter"); }
int32_t DerivedEvaluate(void* vmctx, ManagedObject* self) noexcept {
    return *self->get_field<int32_t>("Counter") + *self->get_field<int32_t>("Bonus");
}
int32_t Sum(void* vmctx, ManagedObject* self, int32_t a, int32_t b) noexcept { return a + b; }
void SetCounter(void* vmctx, ManagedObject* self, int32_t value) noexcept {
    *self->get_field<int32_t>("Counter") = value;
}

CI_REGISTER_PROVIDER(registration) {
    registration.RegisterFunction("BaseCtor", &BaseCtor, 1)
        .RegisterFunction("DerivedCtor", &DerivedCtor, 1)
        .RegisterFunction("Add", &Add, 1)
        .RegisterFunction("BaseEvaluate", &BaseEvaluate, 1)
        .RegisterFunction("DerivedEvaluate", &DerivedEvaluate, 1)
        .RegisterFunction("Sum", &Sum, 2)
        .RegisterFunction("SetCounter", &SetCounter, 1);
}
