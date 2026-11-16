#include <ci/Provider.hpp>

namespace ClassInjectorIncompatibleProvider {
    static uint32_t registrationCalls{};
    static uint32_t notificationCalls{};
}

// 真实 DLL 分别模拟无握手、ABI1、ABI2 和未来不匹配的 SDK。
#ifdef CI_TEST_LEGACY_SDK
extern "C" __declspec(dllexport) bool ci_register_functions(const ci::RegistrationAPI* registrationAPI) {
#else
extern "C" __declspec(dllexport) uint32_t ci_get_sdk_abi() noexcept {
#ifdef CI_TEST_ABI_ONE
    return 1;
#elif defined(CI_TEST_ABI_TWO)
    return 2;
#else
    return ci::SDK_ABI_VERSION + 1;
#endif
}
extern "C" __declspec(dllexport) bool ci_register_provider(const ci::RegistrationAPI* registrationAPI) {
#endif
    ++ClassInjectorIncompatibleProvider::registrationCalls;
    return true;
}

extern "C" __declspec(dllexport) void ci_on_initialize(const ci::ProviderContext* context) noexcept {
    ++ClassInjectorIncompatibleProvider::notificationCalls;
}

extern "C" __declspec(dllexport) uint32_t ci_test_provider_calls() noexcept {
    return ClassInjectorIncompatibleProvider::registrationCalls + ClassInjectorIncompatibleProvider::notificationCalls;
}
