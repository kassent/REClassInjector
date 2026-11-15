#include <Windows.h>
#include <intrin.h>
#include <HookUtils.h>
#include <LogUtils.h>
#include <ClassInjector.h>
#include <atomic>
#include <filesystem>
#include <hidsdi.h>
#include <string_view>

extern "C" {
#define HID_EXPORT(NAME) FARPROC Original##NAME{};
#include "HIDExports.inc"
#undef HID_EXPORT
}

namespace HIDWrapper {
    static SafetyHookInline g_createWindowExAHook;
    static SafetyHookInline g_createWindowExWHook;
    static std::atomic_bool g_classInjectorTriggered{}; // A/W 回调共用的一次性触发，不表示类型已就绪。
    static uintptr_t g_executableStart{};
    static uintptr_t g_executableEnd{};

    // Wine 尚未实现这两项较新的按钮数组 API；明确返回不支持，不改写报告或伪造成功。
    static NTSTATUS WINAPI HidP_GetButtonArrayUnsupported(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, PHIDP_BUTTON_ARRAY_DATA, PUSHORT, PHIDP_PREPARSED_DATA, PCHAR, ULONG) {
        return HIDP_STATUS_NOT_IMPLEMENTED;
    }
    static NTSTATUS WINAPI HidP_SetButtonArrayUnsupported(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, PHIDP_BUTTON_ARRAY_DATA, USHORT, PHIDP_PREPARSED_DATA, PCHAR, ULONG) {
        return HIDP_STATUS_NOT_IMPLEMENTED;
    }
    // 已核 System32 实现为 NTSTATUS(PULONG)：写入 parser 版本后返回 HIDP_STATUS_SUCCESS。
    // 缺失时不冒充 Windows parser 版本，输出 0 并返回不支持。
    static NTSTATUS WINAPI HidP_GetVersionInternalUnsupported(PULONG version) {
        if (version)
            *version = 0;
        return HIDP_STATUS_NOT_IMPLEMENTED;
    }

    static FARPROC ResolveSystemHidExport(HMODULE systemHid, const char* name) {
        if (const auto address = GetProcAddress(systemHid, name))
            return address;
        FARPROC unsupported{};
        const std::string_view exportName{name};
        if (exportName == "HidP_GetButtonArray")
            unsupported = reinterpret_cast<FARPROC>(&HidP_GetButtonArrayUnsupported);
        else if (exportName == "HidP_SetButtonArray")
            unsupported = reinterpret_cast<FARPROC>(&HidP_SetButtonArrayUnsupported);
        else if (exportName == "HidP_GetVersionInternal")
            unsupported = reinterpret_cast<FARPROC>(&HidP_GetVersionInternalUnsupported);
        if (unsupported)
            _MESSAGE("System HID export missing: %s; calls return HIDP_STATUS_NOT_IMPLEMENTED", name);
        else
            _MESSAGE("Required system HID export missing: %s; initialization failed", name);
        return unsupported;
    }

    static void InitializeClassInjector(const void* caller) {
        auto address = uintptr_t(caller);
        if (address < g_executableStart || address >= g_executableEnd || g_classInjectorTriggered.exchange(true)) {
            return;
        }
        // 先撤触发器，provider 加载期间创建窗口也不会递归注册。
        (void)g_createWindowExAHook.disable();
        (void)g_createWindowExWHook.disable();
        _MESSAGE("CreateWindowEx from EXE: caller=%p, uptime=%llu ms", caller, GetTickCount64());
        // 只触发一次核心安装；安装失败由核心统一记录，类型发布仍归真实 VM 初始化。
        _MESSAGE("Type injection hooks ready=%d", ci::InstallTypeInjectionHooks());
    }

    static HWND WINAPI CreateWindowExAHook(DWORD exStyle, LPCSTR className, LPCSTR windowName, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
        InitializeClassInjector(_ReturnAddress());
        return g_createWindowExAHook.unsafe_call<HWND>(exStyle, className, windowName, style, x, y, width, height, parent, menu, instance, parameter);
    }
    static HWND WINAPI CreateWindowExWHook(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style, int x, int y, int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
        InitializeClassInjector(_ReturnAddress());
        return g_createWindowExWHook.unsafe_call<HWND>(exStyle, className, windowName, style, x, y, width, height, parent, menu, instance, parameter);
    }
} // namespace HIDWrapper

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    using namespace HIDWrapper;
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    DisableThreadLibraryCalls(module);
    initLogger("HIDWrapper");
    wchar_t systemPath[MAX_PATH];
    GetSystemDirectoryW(systemPath, MAX_PATH);
    auto hid = LoadLibraryW((std::filesystem::path(systemPath) / L"hid.dll").c_str());
    if (!hid) {
        return FALSE;
    }
    bool resolved = true;
#define HID_EXPORT(NAME)                                                                                               \
    Original##NAME = ResolveSystemHidExport(hid, #NAME);                                                               \
    resolved &= Original##NAME != nullptr;
#include "HIDExports.inc"
#undef HID_EXPORT
    if (!resolved) {
        return FALSE;
    }
    auto executable = GetModuleHandleW(nullptr);
    g_executableStart = uintptr_t(executable);
    g_executableEnd = g_executableStart + HookUtils::GetModuleSize(executable).value_or(0);
    auto user32 = LoadLibraryW(L"user32.dll");
    auto createWindowExA = reinterpret_cast<decltype(&CreateWindowExA)>(GetProcAddress(user32, "CreateWindowExA"));
    auto createWindowExW = reinterpret_cast<decltype(&CreateWindowExW)>(GetProcAddress(user32, "CreateWindowExW"));
    // 触发后需 disable，沿用参考 wrapper 的显式 hook 句柄，不另建 hook 框架。
    g_createWindowExAHook = CreateHookFunction(createWindowExA, &CreateWindowExAHook);
    g_createWindowExWHook = CreateHookFunction(createWindowExW, &CreateWindowExWHook);
    _MESSAGE("HID proxy loaded: pid=%lu, CreateWindowExA=%d, CreateWindowExW=%d", GetCurrentProcessId(),
             bool(g_createWindowExAHook), bool(g_createWindowExWHook));
    return TRUE;
}
