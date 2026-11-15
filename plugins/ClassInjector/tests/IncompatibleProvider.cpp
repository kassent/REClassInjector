#include "ClassProvider.h"

namespace ClassInjectorIncompatibleProvider {
    static uint32_t registrationCalls{};
    static uint32_t notificationCalls{};
}

// 两个真实 DLL 分别模拟握手前的旧 SDK 和显式不匹配的 SDK。
#ifdef CI_TEST_LEGACY_SDK
extern "C" __declspec(dllexport) bool ci_register_functions(const ci::ProviderRegistrationApi*) {
#else
extern "C" __declspec(dllexport) uint32_t ci_get_sdk_abi() noexcept {
    return ci::SDK_ABI_VERSION + 1;
}
extern "C" __declspec(dllexport) bool ci_register_provider(const ci::ProviderRegistrationApi*) {
#endif
    ++ClassInjectorIncompatibleProvider::registrationCalls;
    return true;
}

extern "C" __declspec(dllexport) void ci_on_tdb_ready(const ci::ProviderContext*) noexcept {
    ++ClassInjectorIncompatibleProvider::notificationCalls;
}

extern "C" __declspec(dllexport) uint32_t ci_test_provider_calls() noexcept {
    return ClassInjectorIncompatibleProvider::registrationCalls + ClassInjectorIncompatibleProvider::notificationCalls;
}
