#include <GameTypes.h>
#include <LogUtils.h>
#include <ci/API.hpp>
#include <ci/Provider.hpp>
#include <lua.hpp>
#include <reframework/API.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

namespace ClassInjectorDiagnostics {
    using API = reframework::API;
    // 只借用生产核心发布的服务；不依赖内部 TypeDatabase，也不链接第二份核心。
    static const ci::ProviderContext* context{};

    static API::Method* FindMethod(const std::string& typeName, const std::string& methodName) {
        auto* type = API::get()->tdb()->find_type(typeName);
        return type ? type->find_method(methodName) : nullptr;
    }

    // 仅执行现有整数/引用参数探针；保留真实虚表分派，不以反射调用代替接口窗口验证。
    static uint64_t InvokeScalarMethod(API::Method* method, void* entry, uintptr_t self, uintptr_t first, uintptr_t second) {
        const auto* resultType = method->get_return_type();
        const auto resultName = resultType->get_full_name();
        const auto size =
            context->sdk->type_definition->get_valuetype_size(reinterpret_cast<ci::TypeDefinitionHandle>(resultType));
        if (method->get_num_params() > 2 || resultName == "System.Single" || resultName == "System.Double" ||
            (size && size != 1 && size != 2 && size != 4 && size != 8)) {
            throw std::runtime_error("use SDK invoke for floating-point/aggregate signatures or more than two arguments");
        }
        using FnInvokeScalarMethod =
            uint64_t (*)(void* vmContext, void* object, uintptr_t firstValue, uintptr_t secondValue);
        const auto value = reinterpret_cast<FnInvokeScalarMethod>(entry)(API::get()->get_vm_context(), reinterpret_cast<void*>(self), first, second);
        if (resultName == "System.Void") {
            return 0;
        }
        if (resultName == "System.Int32") {
            return uint64_t(int64_t(int32_t(value)));
        }
        // 原生窄返回值只有低位有效；引用/指针的 size=0，保留完整地址。
        return size && size < 8 ? value & ((uint64_t(1) << (size * 8)) - 1) : value;
    }

    static void InitializeDiagnostics(lua_State* state) {
        API::LuaLock lock;
        if (!context) {
            _MESSAGE("Diagnostics unavailable: CI initialization ProviderContext was not received");
            return;
        }
        sol::state_view lua{state};
        auto diagnostics = lua.create_named_table("class_injector");
        diagnostics["ready"] = [] { return context != nullptr; };
        diagnostics["set_reference"] = [](uintptr_t self, const std::string& name, uintptr_t value) {
            auto* owner = reinterpret_cast<void*>(self);
            auto** field = ci::API::ManagedObject{context->sdk, owner}.get_field<void*>(name.c_str());
            return field && context->sdk->managed_object->assign_reference &&
                   context->sdk->managed_object->assign_reference(owner, field, reinterpret_cast<void*>(value));
        };
        diagnostics["virtual_index"] = [](const std::string& declaringType, const std::string& name) {
            const auto* method = FindMethod(declaringType, name);
            return method ? method->get_virtual_index() : -1;
        };
        diagnostics["call_virtual"] = [](uintptr_t address, const std::string& declaringType, const std::string& name, sol::optional<uintptr_t> first, sol::optional<uintptr_t> second) {
            auto* method = FindMethod(declaringType, name);
            const auto slot = method ? method->get_virtual_index() : -1;
            if (slot < 0) {
                throw std::runtime_error("class virtual method not found");
            }
            auto** info = *reinterpret_cast<void***>(address);
            auto** slots = static_cast<void**>(info[-2]);
            return InvokeScalarMethod(method, slots[slot], address, first.value_or(0), second.value_or(0));
        };
        diagnostics["call_interface"] = [](uintptr_t address, const std::string& interfaceName, const std::string& name,
                                           sol::optional<uintptr_t> first, sol::optional<uintptr_t> second) {
            const auto* type =
                reinterpret_cast<const RE::RETypeDefinition*>(context->sdk->tdb->find_type(interfaceName.c_str()));
            if (!type || !(type->typeFlags & 0x20)) {
                throw std::runtime_error("interface type not found: " + interfaceName);
            }
            const auto* source = reinterpret_cast<const RE::RETypeDefinition*>(
                context->sdk->managed_object->get_type_definition(reinterpret_cast<void*>(address)));
            if (!source || !source->getInterfaceListOffset()) {
                throw std::runtime_error("object does not implement " + interfaceName);
            }
            // 只直接读取现有 common/GameTypes.h 的接口布局；方法记录、名称和签名交给 SDK。
            const auto& tdb = *reinterpret_cast<const RE::TDB*>(API::get()->tdb());
            const auto* bytes = static_cast<const uint8_t*>(tdb.bytePool);
            const auto* interfaces = reinterpret_cast<const RE::REInterfaceList*>(bytes + source->getInterfaceListOffset());
            bool implements = false;
            for (uint32_t index = 0; index < interfaces->count; ++index) {
                implements |= (interfaces->encodedTypes[index] & 0x7FFFF) == type->index;
            }
            if (!implements) {
                throw std::runtime_error("object does not implement " + interfaceName);
            }
            const auto* ids = reinterpret_cast<const uint32_t*>(bytes + type->unknown2C);
            for (size_t slot = 0; slot < tdb.typeImpls[type->implIndex].numManagedVtable; ++slot) {
                auto* method = API::get()->tdb()->get_method(ids[slot]);
                if (name != method->get_name()) {
                    continue;
                }
                auto** info = *reinterpret_cast<void***>(address);
                auto** slots = static_cast<void**>(info[-3 - type->getInterfaceIndex()]);
                return InvokeScalarMethod(method, slots[slot], address, first.value_or(0), second.value_or(0));
            }
            throw std::runtime_error("interface method not found");
        };
        _MESSAGE("C++ type diagnostics ready");
    }
} // namespace ClassInjectorDiagnostics

// 空声明清单只用于取得生产 ProviderContext；本模块不新增类型、不安装启动 hook。
CI_REGISTER_PROVIDER(registration) {}

extern "C" __declspec(dllexport) void ci_on_initialize(const ci::ProviderContext* context) noexcept {
    ClassInjectorDiagnostics::context = context;
}

extern "C" __declspec(dllexport) void reframework_plugin_required_version(REFrameworkPluginVersion* version) {
    *version = {.major = REFRAMEWORK_PLUGIN_VERSION_MAJOR,
                .minor = REFRAMEWORK_PLUGIN_VERSION_MINOR,
                .patch = REFRAMEWORK_PLUGIN_VERSION_PATCH};
}

extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    initLogger("ClassInjectorDiagnostics");
    parameter->functions->on_lua_state_created(ClassInjectorDiagnostics::InitializeDiagnostics);
    _MESSAGE("Diagnostics initialized; injection ready=%d", ClassInjectorDiagnostics::context != nullptr);
    return true;
}
