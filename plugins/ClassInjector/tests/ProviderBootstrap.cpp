#include <Windows.h>
#include <reframework/API.h>

// 测试专用 RF 插件：给已经由描述/绑定阶段加载的示例和测试 DLL 初始化 SDK。
// 不加载第二份 provider，不涉及类型注册或对象生命周期；生产 host 无此转发逻辑。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    const wchar_t* providers[]{L"CIExampleProvider_ci.dll", L"ManagedTypes_ci.dll", L"ClassInjectorTests_ci.dll",
                               L"InterfaceTests_ci.dll"};
    for (auto name : providers) {
        auto module = GetModuleHandleW(name);
        if (!module) {
            continue;
        }
        auto initialize = reinterpret_cast<REFPluginInitializeFn>(GetProcAddress(module, "reframework_plugin_initialize"));
        if (!initialize || !initialize(parameter)) {
            return false;
        }
    }
    return true;
}
