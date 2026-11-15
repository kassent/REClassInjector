#include "ClassProvider.h"
#include <GameTypes.h>
#include "TypeDatabaseRecords.h"
#include "InvokeAdapters.h"
#include <lua.hpp>
#include <sol/sol.hpp>

namespace ClassInjectorFiveArgumentTests {
static const ci::ProviderContext* ProviderContext{};

// self 为 ClassInjector.Tests.FiveArguments；first/third 声明为 System.Object。
static void Initialize(void* context, void* self, void* first, int32_t second, void* third, int32_t fourth, uint32_t fifth) noexcept {
    ProviderContext->AssignReference(self, ProviderContext->GetField<void*>(self, "First"), first);
    ProviderContext->AssignReference(self, ProviderContext->GetField<void*>(self, "Third"), third);
    *ProviderContext->GetField<int32_t>(self, "Second") = second;
    *ProviderContext->GetField<int32_t>(self, "Fourth") = fourth;
    *ProviderContext->GetField<uint32_t>(self, "Fifth") = fifth;
    ++*ProviderContext->GetField<int32_t>(self, "Calls");
    *ProviderContext->GetField<int32_t>(self, "ManagedEntry") = RE::read_offset_data<int32_t>(context, 0x78);
    *ProviderContext->GetField<int32_t>(self, "ManagedDepth") = RE::read_offset_data<int32_t>(context, 0x7C);
}

static void InitializeOriginal(void* context, void* self, void* first, int32_t second, void* third, int32_t fourth, bool fifth) noexcept {
    Initialize(context, self, first, second, third, fourth, fifth ? 1u : 0u);
}

static void InitializeLua(lua_State* state) {
    sol::state_view lua{state};
    auto probe = lua.create_named_table("ci_five_argument_probe");
    probe["metadata"] = [](sol::this_state state) {
        sol::state_view lua{state};
        auto result = lua.create_table();
        auto& api = reframework::API::get();
        auto* tdb = reinterpret_cast<RE::TDB*>(api->tdb());
        auto* type = api->tdb()->find_type("ClassInjector.Tests.FiveArguments");
        for (const auto* name : {"init", "initOriginal"}) {
            auto* method = type->find_method(name);
            auto& definition = static_cast<ci::MethodDef*>(tdb->methods)[method->get_index()];
            auto* list = reinterpret_cast<const ci::ParamList*>(static_cast<const uint8_t*>(tdb->bytePool) + definition.GetParameterOffset());
            auto item = lua.create_table();
            item["methodId"] = method->get_index();
            item["invokeId"] = list->invokeId;
            item["count"] = list->count;
            item["entry"] = uintptr_t(method->get_function<void*>());
            result[name] = item;
        }
        return result;
    };
}
} // namespace ClassInjectorFiveArgumentTests

extern "C" __declspec(dllexport) void ci_on_tdb_ready(const ci::ProviderContext* context) noexcept {
    ClassInjectorFiveArgumentTests::ProviderContext = context;
}
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    if (!ClassInjectorFiveArgumentTests::ProviderContext) return false;
    parameter->functions->on_lua_state_created(ClassInjectorFiveArgumentTests::InitializeLua);
    return true;
}
CI_REGISTER_PROVIDER(registration) {
    using namespace ClassInjectorFiveArgumentTests;
    registration.RegisterFunction("Initialize", &Initialize, 1).RegisterFunction("InitializeOriginal", &InitializeOriginal, 1);
}
