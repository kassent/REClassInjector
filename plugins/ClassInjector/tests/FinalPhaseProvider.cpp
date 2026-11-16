#include <ci/Provider.hpp>

namespace ClassInjectorFinalPhaseProvider {
    static uint32_t calls{};
}

CI_REGISTER_PROVIDER(registration) {}

extern "C" __declspec(dllexport) void ci_on_post_initialize(const ci::ProviderContext* context) noexcept {
    ++ClassInjectorFinalPhaseProvider::calls;
}

extern "C" __declspec(dllexport) uint32_t ci_test_final_phase_calls() noexcept {
    return ClassInjectorFinalPhaseProvider::calls;
}
