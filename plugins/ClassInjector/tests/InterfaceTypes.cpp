#include "ClassProvider.h"
#include <reframework/API.hpp>

using ManagedObject = reframework::API::ManagedObject;

// 测试 bootstrap 在 SDK 就绪后初始化此 DLL；字段访问不经过注入器。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    return true;
}
void DisposeProbe(void* vmctx, ManagedObject* self) noexcept { ++*self->get_field<int32_t>("Counter"); }
void DisposeDerivedProbe(void* vmctx, ManagedObject* self) noexcept { *self->get_field<int32_t>("Counter") += 10; }
bool MoveNextProbe(void* vmctx, ManagedObject* self) noexcept { return ++*self->get_field<int32_t>("Cursor") <= 3; }
ManagedObject* CurrentProbe(void* vmctx, ManagedObject* self) noexcept {
    return *self->get_field<ManagedObject*>("Current");
}
void ResetProbe(void* vmctx, ManagedObject* self) noexcept { *self->get_field<int32_t>("Cursor") = 0; }
ManagedObject* EnumeratorProbe(void* vmctx, ManagedObject* self) noexcept {
    ResetProbe(vmctx, self);
    return self;
}
ManagedObject* SelfProbe(void* vmctx, ManagedObject* self) noexcept { return self; }
int32_t CompareProbe(void* vmctx, ManagedObject* self, ManagedObject* other) noexcept {
    return other == *self->get_field<ManagedObject*>("Current") ? 123 : 456;
}

CI_REGISTER_PROVIDER(registration) {
    registration.RegisterFunction("DisposeProbe", &DisposeProbe, 1)
        .RegisterFunction("DisposeDerivedProbe", &DisposeDerivedProbe, 1)
        .RegisterFunction("MoveNextProbe", &MoveNextProbe, 1)
        .RegisterFunction("CurrentProbe", &CurrentProbe, 1)
        .RegisterFunction("ResetProbe", &ResetProbe, 1)
        .RegisterFunction("EnumeratorProbe", &EnumeratorProbe, 1)
        .RegisterFunction("SelfProbe", &SelfProbe, 1)
        .RegisterFunction("CompareProbe", &CompareProbe, 1);
}
