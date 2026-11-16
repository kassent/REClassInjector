#include "InvokeAdapters.h"
#include "TypeDatabaseRecords.h"
#include <GameTypes.h>
#include <bit>
#include <ci/API.hpp>
#include <ci/Provider.hpp>
#include <lua.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

// 独立的浮点 ABI／托管虚调用 provider；诊断不进入生产插件。
namespace ClassInjectorFloatingReturnTests {
static const ci::ProviderContext* ProviderContext{};

// self 均为 ClassInjector.Tests.FloatingReturnBase 或其派生实例；按真实 C++ 浮点 ABI 返回。
static float GetSingle(void* vmContext, void* self) noexcept {
    return *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<float>("SingleValue");
}

static double GetDouble(void* vmContext, void* self) noexcept {
    return *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<double>("DoubleValue");
}
static float GetNegativeSingle(void* vmctx, void* self) noexcept {
    return -GetSingle(vmctx, self);
}
static double GetNegativeDouble(void* vmctx, void* self) noexcept {
    return -GetDouble(vmctx, self);
}
static float CallVirtualSingle(reframework::API::ManagedObject* self) {
    return RE_VIRTUAL_CALL<float>(self, "ClassInjector.Tests.FloatingReturnBase", "get_VirtualSingle()");
}
static double CallVirtualDouble(reframework::API::ManagedObject* self) {
    return RE_VIRTUAL_CALL<double>(self, "ClassInjector.Tests.FloatingReturnBase", "get_VirtualDouble()");
}
static void Check(bool valid, const char* reason) {
    if (!valid) throw std::runtime_error(reason);
}

template <class T> static void CheckReturn(T actual, T expected) {
    // 位级对照同时覆盖有限值、负零、无穷与 NaN，不依赖 NaN 的数值相等语义。
    using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    Check(std::bit_cast<Bits>(actual) == std::bit_cast<Bits>(expected), "native floating-point return bits changed");
}

static sol::table NativeReturns(sol::this_state state, uintptr_t address) {
    sol::state_view lua{state};
    auto result = lua.create_table();
    auto* self = reinterpret_cast<reframework::API::ManagedObject*>(address);
    auto* vmctx = reframework::API::get()->get_vm_context();
    const auto count = RE::read_offset_data<uint32_t>(vmctx, 0x78);
    const auto depth = RE::read_offset_data<uint32_t>(vmctx, 0x7C);
    Check(count > 0 && depth > 0, "probe needs a native managed call scope");
    const auto* base = ProviderContext->sdk->tdb->find_type("ClassInjector.Tests.FloatingReturnBase");
    const auto single = *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<float>("SingleValue");
    const auto real = *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<double>("DoubleValue");
    using FnGetSingle = float (*)(void* vmContext, void* object);
    using FnGetDouble = double (*)(void* vmContext, void* object);
    auto getSingle = reinterpret_cast<FnGetSingle>(ProviderContext->sdk->method->get_function(
        ProviderContext->sdk->type_definition->find_method(base, "get_Single()")));
    auto getDouble = reinterpret_cast<FnGetDouble>(ProviderContext->sdk->method->get_function(
        ProviderContext->sdk->type_definition->find_method(base, "get_Double()")));
    Check(getSingle && getDouble, "floating-point entry missing");
    CheckReturn(getSingle(vmctx, self), single);
    CheckReturn(getDouble(vmctx, self), real);
    // 原生虚分派直接读 ObjectInfo 的托管虚表；不借 GetMethod 的声明入口冒充派发。
    auto** info = *reinterpret_cast<void***>(self);
    auto** slots = static_cast<void**>(info[-2]);
    for (const auto* name : {"get_VirtualSingle()", "get_VirtualDouble()"}) {
        auto* method = self->get_type_definition()->find_method(name);
        Check(method && method->get_virtual_index() >= 0, "floating-point virtual slot missing");
        auto* entry = slots[method->get_virtual_index()];
        Check(entry == method->get_function<void*>(), "derived virtual slot does not contain override entry");
        if (name == std::string_view("get_VirtualSingle()")) {
            CheckReturn(CallVirtualSingle(self), -single);
        } else {
            CheckReturn(CallVirtualDouble(self), -real);
        }
    }
    result["single"] = single;
    result["double"] = real;
    result["managedCount"] = count;
    result["managedDepth"] = depth;
    return result;
}

static sol::table VirtualSequence(sol::this_state state, uintptr_t baseAddress, uintptr_t derivedAddress) {
    auto* vmctx = reframework::API::get()->get_vm_context();
    const auto count = RE::read_offset_data<uint32_t>(vmctx, 0x78);
    const auto depth = RE::read_offset_data<uint32_t>(vmctx, 0x7C);
    Check(count > 0 && depth > 0, "sequence probe needs a native managed call scope");
    auto* base = reinterpret_cast<reframework::API::ManagedObject*>(baseAddress);
    auto* derived = reinterpret_cast<reframework::API::ManagedObject*>(derivedAddress);
    Check(base->get_type_definition()->get_full_name() == "ClassInjector.Tests.FloatingReturnBase",
          "sequence requires the actual base instance first");
    Check(derived->get_type_definition()->get_full_name() == "ClassInjector.Tests.FloatingReturnDerived",
          "sequence requires the actual derived instance second");
    for (auto* self : {base, derived, base, derived}) {
        const auto single = *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<float>("SingleValue");
        const auto real = *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<double>("DoubleValue");
        CheckReturn(CallVirtualSingle(self), self == base ? single : -single);
        CheckReturn(CallVirtualDouble(self), self == base ? real : -real);
    }
    auto result = sol::state_view{state}.create_table();
    result["calls"] = 8;
    result["managedCount"] = count;
    result["managedDepth"] = depth;
    return result;
}

static void InitializeLua(lua_State* state) {
    sol::state_view lua{state};
    auto probe = lua.create_named_table("ci_floating_return_probe");
    probe["native"] = &NativeReturns;
    probe["virtualSequence"] = &VirtualSequence;
    probe["metadata"] = [](sol::this_state state) {
        sol::state_view lua{state};
        auto result = lua.create_table();
        auto* tdb = reinterpret_cast<RE::TDB*>(reframework::API::get()->tdb());
        auto* type = reframework::API::get()->tdb()->find_type("ClassInjector.Tests.FloatingReturnBase");
        for (const auto* name : {"get_Single()", "get_Double()", "get_VirtualSingle()", "get_VirtualDouble()"}) {
            auto* method = type->find_method(name);
            auto& definition = static_cast<ci::MethodDef*>(tdb->methods)[method->get_index()];
            auto* list = reinterpret_cast<const ci::ParamList*>(static_cast<const uint8_t*>(tdb->bytePool) + definition.GetParameterOffset());
            auto item = lua.create_table();
            item["returnType"] = method->get_return_type()->get_full_name();
            item["invokeId"] = list->invokeId;
            item["slot"] = method->get_virtual_index();
            const auto* code = method->get_function<const uint8_t*>();
            item["tailJump"] = code[0] == 0xFF && code[1] == 0x25;
            result[name] = item;
        }
        return result;
    };
}
} // namespace ClassInjectorFloatingReturnTests

extern "C" __declspec(dllexport) void ci_on_initialize(const ci::ProviderContext* context) noexcept {
    ClassInjectorFloatingReturnTests::ProviderContext = context;
}
extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    *version = {.major = REFRAMEWORK_PLUGIN_VERSION_MAJOR, .minor = REFRAMEWORK_PLUGIN_VERSION_MINOR, .patch = REFRAMEWORK_PLUGIN_VERSION_PATCH};
}
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    if (!ClassInjectorFloatingReturnTests::ProviderContext) return false;
    parameter->functions->on_lua_state_created(ClassInjectorFloatingReturnTests::InitializeLua);
    return true;
}
CI_REGISTER_PROVIDER(registration) {
    using namespace ClassInjectorFloatingReturnTests;
    registration.register_func("GetSingle", &GetSingle, 1)
        .register_func("GetDouble", &GetDouble, 1)
        .register_func("GetNegativeSingle", &GetNegativeSingle, 1)
        .register_func("GetNegativeDouble", &GetNegativeDouble, 1);
}
