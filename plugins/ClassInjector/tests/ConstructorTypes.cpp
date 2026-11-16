#include <GameTypes.h>
#include <HookUtils.h>
#include <array>
#include <ci/API.hpp>
#include <ci/Provider.hpp>
#include <lua.hpp>
#include <sol/sol.hpp>
#include <stdexcept>

// 显式测试 provider；仅通过测试目标部署，不进入生产 host。
namespace ClassInjectorConstructorTests {
static const ci::ProviderContext* ProviderContext{};
using FnObjectCtor = void (*)(void* vmContext, void* object);
static FnObjectCtor ObjectCtor{};

template <class T> static T& Field(void* self, const char* name) {
    return *ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<T>(name);
}

// self 为 ClassInjector.Tests.CtorBase 或派生类。父调用和字段初始化完全归 provider。
static void BaseValueCtor(void* vmctx, void* self, int32_t value) noexcept {
    ObjectCtor(vmctx, self);
    ++Field<int32_t>(self, "BaseCount");
    Field<int32_t>(self, "BaseValue") = value;
    Field<uint64_t>(self, "Context") = uintptr_t(vmctx);
}
static void BaseDefaultCtor(void* vmctx, void* self) noexcept {
    BaseValueCtor(vmctx, self, -1);
}
// self 为 ClassInjector.Tests.CtorDerived。scale 在 XMM2，payload 为第五个原生参数，位于栈上。
static void MixedCtor(void* vmctx, void* self, float scale, void* peer, void* payload) noexcept {
    BaseValueCtor(vmctx, self, int32_t(scale * 100.0f));
    ++Field<int32_t>(self, "DerivedCount");
    Field<float>(self, "Scale") = scale;
    ProviderContext->sdk->managed_object->assign_reference(self, &Field<void*>(self, "Peer"), peer);
    ProviderContext->sdk->managed_object->assign_reference(self, &Field<void*>(self, "Payload"), payload);
}
static void DerivedDefaultCtor(void* vmctx, void* self) noexcept {
    MixedCtor(vmctx, self, 0.75f, nullptr, nullptr);
}
static void SixCtor(void* vmctx, void* self, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) noexcept {
    BaseValueCtor(vmctx, self, a);
    ++Field<int32_t>(self, "DerivedCount");
    Field<int32_t>(self, "A") = a;
    Field<int32_t>(self, "B") = b;
    Field<int32_t>(self, "C") = c;
    Field<int32_t>(self, "D") = d;
    Field<int32_t>(self, "E") = e;
    Field<int32_t>(self, "F") = f;
}
static void RefCtor(void* vmctx, void* self, int32_t* value) noexcept {
    BaseValueCtor(vmctx, self, *value);
    ++Field<int32_t>(self, "DerivedCount");
    *value += 11;
}

static void Check(bool valid, const char* reason) {
    if (!valid) throw std::runtime_error(reason);
}

static void* Allocate(void* vmctx, ci::TypeDefinitionHandle type) {
    auto* self = ProviderContext->sdk->managed_object->create_instance(vmctx, type, true);
    Check(self != nullptr, "CLR allocation failed");
    ProviderContext->sdk->managed_object->add_ref(self); // 本测试调用持有一份，完成后显式释放；不改变分配/构造协议。
    return self;
}
static void CheckInitialZero(void* self) {
    const auto* type =
        reinterpret_cast<const RE::RETypeDefinition*>(ProviderContext->sdk->managed_object->get_type_definition(self));
    auto** info = *static_cast<void***>(self);
    const auto offset = int32_t(intptr_t(info[-1]));
    const auto* bytes = static_cast<const uint8_t*>(self);
    for (uint32_t i = uint32_t(offset); i < type->instanceSize; ++i)
        Check(bytes[i] == 0, "CLR allocator returned a nonzero managed field before ctor");
}

static sol::table Snapshot(sol::this_state state, uintptr_t address) {
    sol::state_view lua{state};
    auto result = lua.create_table();
    auto* self = reinterpret_cast<void*>(address);
    const auto* type =
        reinterpret_cast<const RE::RETypeDefinition*>(ProviderContext->sdk->managed_object->get_type_definition(self));
    result["type"] = type->index;
    result["defaultCtor"] = uint32_t(type->defaultCtorIndex);
    for (const auto* name : {"BaseCount", "BaseValue", "Sentinel", "DerivedCount", "A", "B", "C", "D", "E", "F", "Extra"})
        if (auto* field = ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<int32_t>(name))
            result[name] = *field;
    result["Context"] = Field<uint64_t>(self, "Context");
    if (auto* field = ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<float>("Scale"))
        result["Scale"] = *field;
    if (auto* field = ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<void*>("Peer"))
        result["Peer"] = uintptr_t(*field);
    if (auto* field = ci::API::ManagedObject{ProviderContext->sdk, self}.get_field<void*>("Payload"))
        result["Payload"] = uintptr_t(*field);
    return result;
}

static void InitializeLua(lua_State* state) {
    sol::state_view lua{state};
    auto probe = lua.create_named_table("ci_constructor_probe");
    probe["snapshot"] = &Snapshot;
    probe["allocate"] = [](const char* name) {
        auto* vmctx = reframework::API::get()->get_vm_context();
        const auto* type = ProviderContext->sdk->tdb->find_type(name);
        Check(type != nullptr, "probe allocation type missing");
        auto* self = Allocate(vmctx, type);
        CheckInitialZero(self);
        return uintptr_t(self);
    };
    probe["release"] = [](uintptr_t address) {
        ProviderContext->sdk->managed_object->release(reinterpret_cast<void*>(address));
    };
    probe["native"] = [](sol::this_state state) {
        sol::state_view lua{state};
        auto result = lua.create_table();
        auto* vmctx = reframework::API::get()->get_vm_context();
        result["vmctx"] = uintptr_t(vmctx);
        result["managedCount"] = RE::read_offset_data<uint32_t>(vmctx, 0x78);
        result["managedDepth"] = RE::read_offset_data<uint32_t>(vmctx, 0x7C);
        Check(result.get<uint32_t>("managedCount") > 0 && result.get<uint32_t>("managedDepth") > 0, "probe needs a native managed call scope");
        const auto* type = ProviderContext->sdk->tdb->find_type("ClassInjector.Tests.CtorDerived");
        using FnMixedCtor = void (*)(void* vmContext, void* object, float value, void* firstObject, void* secondObject);
        using FnSixCtor = void (*)(void* vmContext, void* object, int32_t firstValue, int32_t secondValue,
                                   int32_t thirdValue, int32_t fourthValue, int32_t fifthValue, int32_t sixthValue);
        using FnRefCtor = void (*)(void* vmContext, void* object, int32_t* value);
        const auto mixed = reinterpret_cast<FnMixedCtor>(
            ProviderContext->sdk->method->get_function(ProviderContext->sdk->type_definition->find_method(
                type, ".ctor(System.Single, System.Object, System.Object)")));
        const auto six = reinterpret_cast<FnSixCtor>(
            ProviderContext->sdk->method->get_function(ProviderContext->sdk->type_definition->find_method(
                type, ".ctor(System.Int32, System.Int32, System.Int32, System.Int32, System.Int32, System.Int32)")));
        const auto ref = reinterpret_cast<FnRefCtor>(ProviderContext->sdk->method->get_function(
            ProviderContext->sdk->type_definition->find_method(type, ".ctor(System.Int32)")));
        Check(mixed && six && ref, "parameterized ctor entry missing");
        // 三个显式入口均为 FF25 直跳；没有 host 参数包装，也没有隐式父调用。
        for (auto* entry : {reinterpret_cast<void*>(mixed), reinterpret_cast<void*>(six), reinterpret_cast<void*>(ref)}) {
            const auto* code = static_cast<const uint8_t*>(entry);
            Check(code[0] == 0xFF && code[1] == 0x25, "explicit ctor is not a direct native tail jump");
        }
        std::array<void*, 3> objects{};
        try {
            for (auto& self : objects) {
                self = Allocate(vmctx, type);
                CheckInitialZero(self);
            }
            mixed(vmctx, objects[0], 1.25f, objects[1], objects[2]);
            Check(Field<int32_t>(objects[0], "BaseCount") == 1 && Field<int32_t>(objects[0], "DerivedCount") == 1, "parent constructor ran more or less than once");
            Check(Field<int32_t>(objects[0], "BaseValue") == 125 && Field<float>(objects[0], "Scale") == 1.25f, "floating-point ctor parameter changed");
            Check(Field<void*>(objects[0], "Peer") == objects[1] && Field<void*>(objects[0], "Payload") == objects[2], "register/stack reference ctor parameters changed");
            Check(Field<uint64_t>(objects[0], "Context") == uintptr_t(vmctx), "VM context changed");
            Field<int32_t>(objects[0], "Sentinel") = 0x12345678;
            mixed(vmctx, objects[0], 3.5f, nullptr, nullptr);
            Check(Field<int32_t>(objects[0], "Sentinel") == 0x12345678 && Field<int32_t>(objects[0], "BaseCount") == 2, "host cleared fields or repeated parent initialization");
            six(vmctx, objects[1], 7, -11, 13, -17, 19, -23);
            Check(Field<int32_t>(objects[1], "A") == 7 && Field<int32_t>(objects[1], "B") == -11 && Field<int32_t>(objects[1], "C") == 13 && Field<int32_t>(objects[1], "D") == -17 && Field<int32_t>(objects[1], "E") == 19 && Field<int32_t>(objects[1], "F") == -23, "six ctor parameters did not preserve stack ABI");
            int32_t value = 31;
            ref(vmctx, objects[2], &value);
            Check(value == 42 && Field<int32_t>(objects[2], "BaseValue") == 31, "ref ctor parameter was not written back");
            result["mixed"] = true;
            result["six"] = true;
            result["ref"] = true;
            result["initialZero"] = true;
            result["noHostClear"] = true;
            result["parentExactlyOnce"] = true;
        } catch (...) {
            for (auto* self : objects)
                if (self)
                    ProviderContext->sdk->managed_object->release(self);
            throw;
        }
        for (auto* self : objects)
            ProviderContext->sdk->managed_object->release(self);
        return result;
    };
}
} // namespace ClassInjectorConstructorTests

extern "C" __declspec(dllexport) void ci_on_initialize(const ci::ProviderContext* context) noexcept {
    using namespace ClassInjectorConstructorTests;
    ProviderContext = context;
    ObjectCtor = reinterpret_cast<FnObjectCtor>(context->sdk->method->get_function(
        context->sdk->type_definition->find_method(context->sdk->tdb->find_type("System.Object"), ".ctor()")));
}
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    if (!ClassInjectorConstructorTests::ProviderContext || !ClassInjectorConstructorTests::ObjectCtor) return false;
    parameter->functions->on_lua_state_created(ClassInjectorConstructorTests::InitializeLua);
    return true;
}
CI_REGISTER_PROVIDER(registration) {
    using namespace ClassInjectorConstructorTests;
    registration.register_func("BaseDefaultCtor", &BaseDefaultCtor, 1)
        .register_func("BaseValueCtor", &BaseValueCtor, 1)
        .register_func("MixedCtor", &MixedCtor, 1)
        .register_func("DerivedDefaultCtor", &DerivedDefaultCtor, 1)
        .register_func("SixCtor", &SixCtor, 1)
        .register_func("RefCtor", &RefCtor, 1);
}
