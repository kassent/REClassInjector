#include <GameTypes.h>
#include <HookUtils.h>
#include <lua.hpp>
#include <sol/sol.hpp>

// 测试原生 RSZ reader，而不是在 Lua 里重建对象或手动赋值。
// 当前 RF 的 sdk.deserialize 仍使用旧版本签名；此探针不修改 RF 或生产注入器。
namespace {
    using ManagedObject = reframework::API::ManagedObject;
    struct ObjectArray {
        ManagedObject** data{};
        uint32_t count{};
        uint32_t capacity{};
    };
    using FnDeserializeRsz = uint32_t (*)(void*, ObjectArray*, const uint8_t*, size_t);
    using FnFindTypeInfo = void* (*)(uint32_t);
    FnDeserializeRsz g_deserializeRsz{};
    FnFindTypeInfo g_findTypeInfo{};

    void InitializeLua(lua_State* state) {
        reframework::API::LuaLock lock;
        sol::state_view lua{state};
        auto api = lua.create_named_table("ci_rsz_probe");
        api["find_type"] = [](uint32_t hash) { return uintptr_t(g_findTypeInfo(hash)); };
        api["reader"] = [] { return uintptr_t(g_deserializeRsz); };
        api["deserialize"] = [](sol::this_state state, const sol::table& input) {
            // 显式读取 Lua table；sol2 的 vector 参数默认期望容器 userdata。
            std::vector<uint8_t> bytes(input.size());
            for (size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = input.get<uint8_t>(i + 1);
            }
            ObjectArray objects;
            auto success = g_deserializeRsz(nullptr, &objects, bytes.data(), bytes.size()) & 0xFF;
            sol::state_view lua{state};
            auto result = lua.create_table();
            result["success"] = success != 0;
            auto roots = lua.create_table();
            for (uint32_t i = 0; i < objects.count; ++i) {
                roots[i + 1] = uintptr_t(objects.data[i]);
            }
            result["roots"] = roots;
            // reader 给每个输出根持有一次引用，Lua 探针负责 release；这里只释放数组容器。
            reframework::API::get()->sdk()->functions->deallocate(objects.data);
            return result;
        };
        api["release"] = [](uintptr_t address) { reinterpret_cast<ManagedObject*>(address)->release(); };
    }
} // namespace

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    // 当前 EXE 全映像唯一的 RSZ magic 判断；通过 unwind 获取所属整函数入口。
    auto check = HookUtils::ScanIDAPattern("81 3B 52 53 5A 00");
    DWORD64 base{};
    auto* entry = check ? RtlLookupFunctionEntry(check, &base, nullptr) : nullptr;
    if (!entry) {
        return false;
    }
    g_deserializeRsz = reinterpret_cast<FnDeserializeRsz>(base + entry->BeginAddress);
    auto call = HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(g_deserializeRsz),
                                                 "E8 ? ? ? ? 66 83 78 0C 00", entry->EndAddress - entry->BeginAddress);
    if (!call) {
        return false;
    }
    g_findTypeInfo = reinterpret_cast<FnFindTypeInfo>(HookUtils::ReadOffsetData(call, 1, 5));
    parameter->functions->on_lua_state_created(InitializeLua);
    return true;
}
