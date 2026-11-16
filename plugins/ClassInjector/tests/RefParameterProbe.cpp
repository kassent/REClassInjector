#include "NativeCallProbe.h"
#include "TypeDatabaseRecords.h"
#include <GameTypes.h>
#include <Windows.h>
#include <ci/Provider.hpp>
#include <lua.hpp>
#include <sol/sol.hpp>

// 临时真机探针只读取 provider 实收参数与引擎记录；无 hook、无对象创建/生命周期服务。
namespace {
    FnGetNativeCallProbe g_getCall{};
    FnGetEarlyQueryProbe g_getQueries{};

    void InitializeLua(lua_State* state) {
        sol::state_view lua{state};
        auto probe = lua.create_named_table("ci_ref_probe");
        probe["snapshot"] = [](sol::this_state state) {
            sol::state_view lua{state};
            auto call = *g_getCall();
            auto queries = *g_getQueries();
            auto result = lua.create_table();
            result["vmctx"] = uintptr_t(call.vmctx);
            result["expectedVmctx"] = uintptr_t(reframework::API::get()->get_vm_context());
            result["self"] = uintptr_t(call.self);
            result["position"] = call.first;
            result["work"] = call.second;
            result["opWork"] = call.third;
            result["readyNotifications"] = queries.notifications;
            result["sdkInitializedAtReady"] = queries.sdkInitializedAtReady;
            result["earlyEcho"] = uintptr_t(queries.echo);
            result["earlyOut"] = uintptr_t(queries.outId);
            return result;
        };
        probe["metadata"] = [](sol::this_state state, uintptr_t address) {
            sol::state_view lua{state};
            auto* self = reinterpret_cast<reframework::API::ManagedObject*>(address);
            auto* method = self->get_type_definition()->find_method(
                "updateTableInpl(ace.btable.BTableDef.POSITION, ace.btable.cCommandWork, ace.btable.cOperatorWork)");
            auto* tdb = reinterpret_cast<RE::TDB*>(reframework::API::get()->tdb());
            auto& definition = static_cast<ci::MethodDef*>(tdb->methods)[method->get_index()];
            auto& impl = static_cast<ci::MethodImpl*>(tdb->methodImpls)[definition.impl];
            auto* list = reinterpret_cast<const ci::ParamList*>(
                static_cast<const uint8_t*>(tdb->bytePool) + definition.GetParameterOffset());
            auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
            auto** info = *reinterpret_cast<void***>(address);
            auto** slots = static_cast<void**>(info[-2]);
            auto result = lua.create_table();
            result["index"] = method->get_index();
            result["slot"] = impl.virtualIndex;
            result["entry"] = uintptr_t(method->get_function<void*>());
            result["virtualEntry"] = uintptr_t(slots[impl.virtualIndex]);
            result["invokeId"] = list->invokeId;
            result["count"] = list->count;
            auto parameters = lua.create_table();
            // 此探针查询新增记录，Parameter ID 没有原表的高位标记。
            for (uint32_t i = 0; i < list->count; ++i) {
                auto parameter = static_cast<const ci::Parameter*>(tdb->params)[ids[i]];
                auto value = lua.create_table();
                value["modifier"] = uint32_t(parameter.modifier);
                value["flags"] = uint32_t(parameter.flags);
                value["type"] = uint32_t(parameter.type);
                parameters[i + 1] = value;
            }
            result["parameters"] = parameters;
            return result;
        };
    }
} // namespace

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    auto module = GetModuleHandleW(L"ClassInjectorTests_ci.dll");
    g_getCall = reinterpret_cast<FnGetNativeCallProbe>(GetProcAddress(module, "GetNativeCallProbe"));
    g_getQueries = reinterpret_cast<FnGetEarlyQueryProbe>(GetProcAddress(module, "GetEarlyQueryProbe"));
    if (!g_getCall || !g_getQueries) {
        return false;
    }
    parameter->functions->on_lua_state_created(InitializeLua);
    return true;
}
