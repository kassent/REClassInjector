#include <GameTypes.h>
#include <ci/Provider.hpp>

using ManagedObject = reframework::API::ManagedObject;

// 闭合枚举类型使用实际 Int32 ABI，字段元数据仍保留 System.DayOfWeek 身份。
int32_t GetTimerType(void*, ManagedObject* self) noexcept {
    return *self->get_field<int32_t>("_TimerType");
}
void SetTimerType(void*, ManagedObject* self, int32_t value) noexcept {
    *self->get_field<int32_t>("_TimerType") = value;
}
void TimerStart(void*, ManagedObject* self, ManagedObject*) noexcept {
    *self->get_field<int32_t>("_TimerUniqueId") += *self->get_field<int32_t>("_TimerType");
}
void DerivedTimerStart(void*, ManagedObject* self, ManagedObject*) noexcept {
    *self->get_field<int32_t>("_TimerUniqueId") += 2 * *self->get_field<int32_t>("_TimerType");
}
void ReferenceTimerStart(void*, ManagedObject* self, ManagedObject*) noexcept {
    ++*self->get_field<int32_t>("_TimerUniqueId");
}

CI_REGISTER_PROVIDER(registration) {
    registration.register_func("GetTimerType", &GetTimerType, 1)
        .register_func("SetTimerType", &SetTimerType, 1)
        .register_func("TimerStart", &TimerStart, 1)
        .register_func("DerivedTimerStart", &DerivedTimerStart, 1)
        .register_func("ReferenceTimerStart", &ReferenceTimerStart, 1);
}

// 显式测试 provider 自行完成 SDK 初始化，不让生产 host 代管 SDK 或字段访问。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    return true;
}
