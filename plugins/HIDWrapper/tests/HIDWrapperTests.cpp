#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <hidsdi.h>

namespace HIDWrapperTests {
    static void Check(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }
}

// 调用者将此 EXE 放进没有 provider 的临时目录；只验 HID 转发和一次性启动触发。
// 此回归没有真实 TDB，不声称验证游戏类型发布、Context 服务或战斗逻辑。
int wmain(int argc, wchar_t** argv) {
    using namespace HIDWrapperTests;
    try {
        Check(argc == 2, "usage: HIDWrapperTests.exe <integrated hid.dll>");
        const auto wrapper = LoadLibraryW(argv[1]);
        Check(wrapper != nullptr, "integrated hid.dll loads without a standalone host");
        Check(GetModuleHandleW(L"ClassInjector.dll") == nullptr, "standalone host must not be loaded");
        Check(GetProcAddress(wrapper, "ci_bootstrap") == nullptr, "retired cross-DLL bootstrap export");
        Check(GetProcAddress(wrapper, "ci_is_ready") == nullptr, "retired cross-DLL ready export");
        Check(GetProcAddress(wrapper, "reframework_plugin_initialize") == nullptr, "production host has no RF initialization entry");

        unsigned ordinal = 1;
#define HID_EXPORT(NAME)                                                                                       \
        Check(GetProcAddress(wrapper, #NAME) != nullptr, "HID name export: " #NAME);                            \
        Check(GetProcAddress(wrapper, #NAME) == GetProcAddress(wrapper, MAKEINTRESOURCEA(ordinal++)),           \
              "HID name/ordinal identity: " #NAME);
#include "../src/HIDExports.inc"
#undef HID_EXPORT
        Check(ordinal == 48, "all 47 HID name/ordinal exports are preserved");
        wchar_t systemDirectory[MAX_PATH];
        Check(GetSystemDirectoryW(systemDirectory, MAX_PATH) != 0, "system directory");
        const auto systemHid = LoadLibraryW((std::filesystem::path(systemDirectory) / L"hid.dll").c_str());
        Check(systemHid && systemHid != wrapper, "system HID remains a separate module");
        using FnGetHidGuid = void(WINAPI*)(GUID*);
        GUID expected{}, actual{};
        reinterpret_cast<FnGetHidGuid>(GetProcAddress(systemHid, "HidD_GetHidGuid"))(&expected);
        reinterpret_cast<FnGetHidGuid>(GetProcAddress(wrapper, "HidD_GetHidGuid"))(&actual);
        Check(IsEqualGUID(expected, actual), "forwarded HID GUID matches System32");

        // 在 Wine 上实际走缺失导出分支；Windows 保留真实系统实现，不替换或模拟它。
        unsigned unavailableExports = 0;
        HIDP_BUTTON_ARRAY_DATA button{};
        USHORT buttonLength = 1;
        char report = 0x5a;
        if (!GetProcAddress(systemHid, "HidP_GetButtonArray")) {
            const auto getButtonArray = reinterpret_cast<decltype(&HidP_GetButtonArray)>(GetProcAddress(wrapper, "HidP_GetButtonArray"));
            Check(getButtonArray(HidP_Input, 0, 0, 0, &button, &buttonLength, nullptr, &report, 1) == HIDP_STATUS_NOT_IMPLEMENTED, "missing GetButtonArray reports unsupported");
            Check(buttonLength == 1 && button.ArrayIndex == 0 && !button.On && report == 0x5a, "unsupported get preserves caller buffers");
            ++unavailableExports;
        }
        if (!GetProcAddress(systemHid, "HidP_SetButtonArray")) {
            const auto setButtonArray = reinterpret_cast<decltype(&HidP_SetButtonArray)>(GetProcAddress(wrapper, "HidP_SetButtonArray"));
            Check(setButtonArray(HidP_Output, 0, 0, 0, &button, 1, nullptr, &report, 1) == HIDP_STATUS_NOT_IMPLEMENTED, "missing SetButtonArray reports unsupported");
            Check(report == 0x5a, "unsupported set preserves caller report");
            ++unavailableExports;
        }
        if (!GetProcAddress(systemHid, "HidP_GetVersionInternal")) {
            using FnHidP_GetVersionInternal = NTSTATUS(WINAPI*)(PULONG);
            const auto getVersionInternal = reinterpret_cast<FnHidP_GetVersionInternal>(GetProcAddress(wrapper, "HidP_GetVersionInternal"));
            ULONG version = 0xffffffff;
            Check(getVersionInternal(&version) == HIDP_STATUS_NOT_IMPLEMENTED && version == 0, "missing parser version is not advertised as implemented");
            ++unavailableExports;
        }

        // HWND_MESSAGE 不显示窗口、不抢焦点；A/W 连续调用应只安装一次 CI 启动入口。
        const auto first = CreateWindowExW(0, L"STATIC", L"HIDWrapperTests", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        const auto second = CreateWindowExA(0, "STATIC", "HIDWrapperTests", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        Check(first && second, "message-only window creation forwards to user32");
        DestroyWindow(first);
        DestroyWindow(second);
        wchar_t executable[MAX_PATH];
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::ifstream stream(std::filesystem::path(executable).parent_path() / L"logs/HIDWrapper.log");
        Check(bool(stream), "single integrated host log");
        const std::string log{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        const auto trigger = log.find("CreateWindowEx from EXE:");
        Check(trigger != std::string::npos && log.find("CreateWindowEx from EXE:", trigger + 1) == std::string::npos, "A/W share one startup trigger");
        Check(log.find("ClassInjector startup:") != std::string::npos, "integrated CI core was called directly");
        Check(log.find("Type injection skipped: no valid type declarations or "
                       "CI initialization providers") != std::string::npos,
              "empty provider directories do not patch an unrelated EXE");
        Check(GetModuleHandleW(L"ClassInjector.dll") == nullptr, "initialization did not load the retired host");
        std::cout << "PASS: 47 HID names/ordinals, System32 forwarding, " << unavailableExports << " unsupported exports checked, direct CI initialization once across W/A, no standalone host or production RF exports\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
