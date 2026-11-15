#include "ClassProviders.h"
#include "ClassInjectorTestAccess.h"
#include <Windows.h>
#include <iostream>
#include <stdexcept>

namespace ClassInjectorProviderAbiTests {
    static void Check(bool passed, const char* message) {
        if (!passed) throw std::runtime_error(message);
    }

    // 不匹配 DLL 可由 Windows 加载，但不得得到 ProviderRegistrationApi / ProviderContext。
    static void TestRejectedProvider(const std::filesystem::path& file, const std::filesystem::path& compatible) {
        ci::ProviderConfig rejected{.configPath = "incompatible.toml", .dllPath = file,
                                          .types = {{.name = "Tests.Unavailable"}}};
        ci::ProviderConfig dependent{.configPath = "dependent.toml", .dllPath = compatible,
                                           .types = {{.name = "Tests.Dependent", .base = "Tests.Unavailable"}}};
        std::vector<ci::ClassProviderNotification> callbacks;
        const auto types = ci::ResolveClassProviders({rejected, dependent}, &callbacks);
        Check(types.empty() && callbacks.empty(), "SDK rejection must discard the whole provider and its dependents");
        const auto module = GetModuleHandleW(file.c_str());
        Check(module != nullptr, "rejected loaded modules retain the existing process-lifetime policy");
        const auto calls = reinterpret_cast<uint32_t (*)()>(GetProcAddress(module, "ci_test_provider_calls"));
        Check(calls && calls() == 0, "incompatible provider received registration or ready callback");
    }
}

int main(int argc, char** argv) {
    using namespace ClassInjectorProviderAbiTests;
    try {
        Check(argc == 4, "expected legacy, mismatched and compatible provider DLL paths");
        const auto compatible = std::filesystem::absolute(argv[3]);
        TestRejectedProvider(std::filesystem::absolute(argv[1]), compatible);
        TestRejectedProvider(std::filesystem::absolute(argv[2]), compatible);
        const auto module = GetModuleHandleW(compatible.c_str());
        const auto getAbi = reinterpret_cast<ci::FnGetSdkAbi>(GetProcAddress(module, "ci_get_sdk_abi"));
        Check(getAbi && getAbi() == ci::SDK_ABI_VERSION, "matching SDK identity export");
        Check(GetProcAddress(module, "ci_register_provider") && !GetProcAddress(module, "ci_register_functions"),
              "new provider must not expose the unchecked legacy registration entry");
        ci::ProviderConfig accepted{.configPath = "compatible.toml", .dllPath = compatible,
                                          .types = {{.name = "Tests.Accepted"}}};
        const auto types = ci::ResolveClassProviders({accepted});
        Check(types.size() == 1 && types[0].name == "Tests.Accepted", "matching SDK registration must succeed");
        std::cout << "PASS: matching SDK, missing/mismatched identity, rejection before API access, dependency discard and legacy-host boundary\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
