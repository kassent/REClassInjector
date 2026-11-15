#include <Windows.h>
#include <reframework/API.h>

// 临时 RF 测试入口只初始化已被 CI 加载的闭合类型 provider；不参与注册。
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    auto module = GetModuleHandleW(L"ClassInjectorClosedTypes_ci.dll");
    auto initialize = module ? reinterpret_cast<REFPluginInitializeFn>(
                                   GetProcAddress(module, "reframework_plugin_initialize"))
                             : nullptr;
    return initialize && initialize(parameter);
}
