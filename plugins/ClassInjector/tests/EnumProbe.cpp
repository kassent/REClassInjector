#include <GameTypes.h>
#include <HookUtils.h>
#include <LogUtils.h>
#include <array>
#include <atomic>
#include <ci/Provider.hpp>
#include <lua.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

// 独立能力探针：只修改System.DayOfWeek；原生PlayerManager回调中单次执行反射，业务敌人不参与。
namespace ClassInjectorEnumProbe {
using API = reframework::API;
using Object = API::ManagedObject;
using Json = nlohmann::json;
using FnGetValues = RE::SystemArray<int32_t>* (*)(void*, Object*);
using FnGetNames = RE::SystemArray<RE::SystemString*>* (*)(void*, Object*);
using FnGetName = RE::SystemString* (*)(void*, Object*, Object*);
using FnToObject = Object* (*)(void*, Object*, int32_t);
static FnGetValues GetValues{};
static FnGetNames GetNames{};
static FnGetName GetName{};
static FnToObject ToObject{};
static std::atomic_bool pending{};
static std::mutex resultMutex;
static Json result{{"state", "idle"}};

static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

static Json Run(void* vmctx, int32_t entries, int32_t depth) {
    auto* definition = API::get()->tdb()->find_type("System.DayOfWeek");
    Check(definition != nullptr, "DayOfWeek missing");
    auto* runtimeType = definition->get_runtime_type();
    auto* values = GetValues(vmctx, runtimeType);
    auto* names = GetNames(vmctx, runtimeType);
    Check(values && names && values->count == 12 && names->count == values->count, "native enum array count");
    const std::array<int32_t, 12> expected{0, 1, 2, 3, 4, 5, 6, 0, -1234567, INT32_MIN, INT32_MAX, 987654321};
    const std::array<const char*, 12> labels{"CI_Sunday", "Monday", "CI_TuesdayByValue", "Wednesday", "CI_ThursdayByName", "Friday", "Saturday", "CI_ZeroAlias", "CI_NegativeByName", "CI_MinByValue", "CI_MaxByValue", "CI_FromProviderByValue"};
    Json rows = Json::array();
    for (size_t i = 0; i < expected.size(); ++i) {
        Check(values->elements[i] == expected[i] && names->elements[i] && names->elements[i]->to_str() == labels[i], "GetValues/GetNames disagreement");
        auto* boxed = ToObject(vmctx, runtimeType, expected[i]);
        auto* name = boxed ? GetName(vmctx, runtimeType, boxed) : nullptr;
        const auto canonical = i == 7 ? std::string("CI_Sunday") : std::string(labels[i]);
        Check(name && name->to_str() == canonical, "GetName canonical result");
        rows.push_back({{"value", expected[i]}, {"name", labels[i]}, {"canonical", name->to_str()}});
    }
    auto* tdb = reinterpret_cast<RE::TDB*>(API::get()->tdb());
    auto& type = tdb->types[definition->get_index()];
    Check(type.objectType == 5 && type.underlyingTypeIndex == 8 && tdb->typeImpls[type.implIndex].fieldSize == 4 && type.arrayTypeIndex != 0, "original enum layout");
    auto* valuesObject = reinterpret_cast<Object*>(values);
    Check(valuesObject->get_type_definition()->get_index() == type.arrayTypeIndex, "original enum array identity");
    return {
        {"state", "passed"},
        {"scopeEntries", entries},
        {"scopeDepth", depth},
        {"thread", GetCurrentThreadId()},
        {"typeId", definition->get_index()},
        {"arrayTypeId", uint32_t(type.arrayTypeIndex)},
        {"payloadSize", 4},
        {"rows", rows}};
}

static void InitializeLua(lua_State* state) {
    sol::state_view lua{state};
    auto probe = lua.create_named_table("ci_enum_probe");
    probe["arm"] = [] {
        std::scoped_lock lock(resultMutex);
        result = {{"state", "pending"}};
        pending.store(true, std::memory_order_release);
    };
    probe["result"] = [] {
        std::scoped_lock lock(resultMutex);
        return result.dump();
    };
}

static void ExecuteProbe(void* vmctx, int32_t entries, int32_t depth) {
    Json current;
    try { current = Run(vmctx, entries, depth); }
    catch (const std::exception& error) { current = {{"state", "failed"}, {"error", error.what()}}; }
    _MESSAGE("Enum probe: %s", current.dump().c_str());
    std::scoped_lock lock(resultMutex);
    result = std::move(current);
}

static bool InstallProbe() {
    GetValues = RE::get_method<FnGetValues>("System.Enum", "GetValues(System.Type)");
    GetNames = RE::get_method<FnGetNames>("System.Enum", "GetNames(System.Type)");
    GetName = RE::get_method<FnGetName>("System.Enum", "GetName(System.Type, System.Object)");
    ToObject = RE::get_method<FnToObject>("System.Enum", "ToObject(System.Type, System.Int32)");
    using FnUpdate = void (*)(void*, Object*);
    auto update = RE::get_method<FnUpdate>("app.PlayerManager", "update()");
    if (!GetValues || !GetNames || !GetName || !ToObject || !update) { _MESSAGE("Enum probe required method missing"); return false; }
    return HookLambda(update, [](void* vmctx, Object* self) {
        if (pending.load(std::memory_order_acquire) && vmctx == API::get()->get_vm_context()) {
            const auto entries = RE::read_offset_data<int32_t>(vmctx, 0x78);
            const auto depth = RE::read_offset_data<int32_t>(vmctx, 0x7C);
            if (entries > 0 && depth > 0 && pending.exchange(false, std::memory_order_acq_rel)) {
                ExecuteProbe(vmctx, entries, depth);
            }
        }
        original(vmctx, self);
    });
}
}

// 动态声明与TOML声明走同一枚举构建路径，顺便验证provider登记阶段。
CI_REGISTER_PROVIDER(registration) {
    std::string sunday = "CI_SundayByCpp";
    registration.register_enum(
        "System.DayOfWeek",
        {.renames = {ci::EnumRename::by_name("CI_Thursday", "CI_ThursdayByName"),
                     ci::EnumRename::by_value(2, "CI_TuesdayByValue"), ci::EnumRename::by_value(0, sunday.c_str()),
                     ci::EnumRename::by_name("CI_NegativeByValue", "CI_NegativeByName")},
         .additions = {{.name = "CI_FromProvider", .value = 987654321}}});
    sunday.assign("provider-local string changed after registration"); // 核宿主在登记调用内复制字符串。
    registration.register_enum("System.DayOfWeek",
                               {.renames = {ci::EnumRename::by_value(987654321, "CI_FromProviderByValue"),
                                            ci::EnumRename::by_name("CI_SundayByCpp", "CI_Sunday"),
                                            ci::EnumRename::by_value(INT32_MIN, "CI_MinByValue"),
                                            ci::EnumRename::by_value(INT32_MAX, "CI_MaxByValue")}});
}
extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    *version = {.major = REFRAMEWORK_PLUGIN_VERSION_MAJOR, .minor = REFRAMEWORK_PLUGIN_VERSION_MINOR, .patch = REFRAMEWORK_PLUGIN_VERSION_PATCH};
}
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    initLogger("ClassInjectorEnumProbe");
    if (!ClassInjectorEnumProbe::InstallProbe()) return false;
    parameter->functions->on_lua_state_created(ClassInjectorEnumProbe::InitializeLua);
    return true;
}
