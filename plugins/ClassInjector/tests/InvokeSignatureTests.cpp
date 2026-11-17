#include "InvokeAdapters.h"
#include "ClassInjectorTestAccess.h"
#include "TypeDatabaseBuilder.h"
#include "TypeInjection.h"
#include "ClassProviderContext.h"
#include "MethodTrampolines.h"
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#ifdef CI_INVOKE_SIGNATURE_PROVIDER
#include <ci/Provider.hpp>
#include <lua.hpp>
#include <sol/sol.hpp>
#endif

namespace ClassInjectorSignatureTests {
#define MIXED_PARAMETERS float a, double b, int32_t c, uint32_t d, void* peer, bool flag, float g, double h, int64_t i, uint64_t j, int8_t k, uint8_t l, int16_t m, uint16_t n, uint16_t o, int32_t p
#define MIXED_VALUES a, b, c, d, peer, flag, g, h, i, j, k, l, m, n, o, p
struct Receipt {
    bool valid{};
    uintptr_t context{}, self{}, peer{};
    uint32_t calls{};
    int32_t entry{}, depth{};
};
static Receipt Last;
enum class Category : int32_t { Value = 3 };

// self 为 ClassInjector.Tests.Signatures，peer 声明为 System.Object；测试回执不持有托管引用。
static void Record(void* context, void* self, MIXED_PARAMETERS) noexcept {
    Last.valid = c == -17 && d == 0xF1234567 && flag && g == 2.5f && h == 1.0000000000000002 && i == -0x123456789ABCll && j == 0xFEDCBA9876543210ull && k == -101 && l == 231 && m == -12345 && n == 54321 && o == 0x4E2D && p == 3;
    Last.context = uintptr_t(context);
    Last.self = uintptr_t(self);
    Last.peer = uintptr_t(peer);
    ++Last.calls;
#ifdef CI_INVOKE_SIGNATURE_PROVIDER
    Last.entry = RE::read_offset_data<int32_t>(context, 0x78);
    Last.depth = RE::read_offset_data<int32_t>(context, 0x7C);
#endif
}

template<class T> static T ReturnMixed(void* context, void* self, MIXED_PARAMETERS) noexcept {
    Record(context, self, MIXED_VALUES);
    if constexpr (std::is_void_v<T>) return;
    else if constexpr (std::is_pointer_v<T>) return peer;
    else if constexpr (std::is_enum_v<T>) return Category::Value;
    else if constexpr (std::is_same_v<T, bool>) return true;
    else if constexpr (std::is_same_v<T, int8_t>) return -101;
    else if constexpr (std::is_same_v<T, uint8_t>) return 231;
    else if constexpr (std::is_same_v<T, int16_t>) return -12345;
    else if constexpr (std::is_same_v<T, uint16_t>) return 54321;
    else if constexpr (std::is_same_v<T, int32_t>) return -123456789;
    else if constexpr (std::is_same_v<T, uint32_t>) return 0xF1234567;
    else if constexpr (std::is_same_v<T, int64_t>) return -0x123456789ABCll;
    else if constexpr (std::is_same_v<T, uint64_t>) return 0xFEDCBA9876543210ull;
    else if constexpr (std::is_same_v<T, float>) return a;
    else return b;
}

static double Reverse(void*, void*, int32_t a, float b, void* c, double d, bool e, uint32_t f, float g, void* h, double i) noexcept {
    Last.valid = a == -7 && c == h && e && f == 0x81234567;
    ++Last.calls;
    return b + d + g + i;
}
static uint64_t Zero(void*, void*) noexcept { return 0xFEDCBA9876543210ull; }
static int32_t One(void*, void*, int32_t value) noexcept { return value * 2; }

static std::vector<ci::ParameterSpec> MixedParameters() {
    return {{"a", "System.Single"}, {"b", "System.Double"}, {"c", "System.Int32"}, {"d", "System.UInt32"}, {"peer", "System.Object"}, {"flag", "System.Boolean"}, {"g", "System.Single"}, {"h", "System.Double"}, {"i", "System.Int64"}, {"j", "System.UInt64"}, {"k", "System.SByte"}, {"l", "System.Byte"}, {"m", "System.Int16"}, {"n", "System.UInt16"}, {"o", "System.Char"}, {"p", "via.motion.SequenceCategory"}};
}

#ifndef CI_INVOKE_SIGNATURE_PROVIDER
static void Check(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
static bool ThrowMany{};
template<size_t> using IntArgument = int32_t;
template<size_t... I> static int64_t Many(void*, void*, IntArgument<I>... values) {
    if (ThrowMany) throw std::runtime_error("large-frame unwind canary");
    const std::array<int32_t, sizeof...(I)> arguments{values...};
    int64_t result{};
    for (size_t index = 0; index < arguments.size(); ++index) {
        Check(arguments[index] == int32_t(index + 1), "large parameter table position");
        result += arguments[index];
    }
    return result;
}
template<size_t... I> static auto ManyEntry(std::index_sequence<I...>) { return &Many<I...>; }
static void RefValues(void*, void*, double* value, int32_t* output, float factor, uint32_t id, void* peer, double addend) noexcept {
    *value = *value * factor + addend;
    *output = int32_t(id ^ uint32_t(uintptr_t(peer)));
}
static uint32_t ColorArgument(void*, void*, const uint32_t* color, float factor, uint32_t id, void*, double number) noexcept {
    return *color ^ id ^ uint32_t(factor + number);
}

static void Run(const RE::TDB& header) {
    std::vector<ci::MethodSpec> methods;
    auto Add = [&](const char* name, const char* result, void* address, std::vector<ci::ParameterSpec> arguments) {
        methods.push_back({.name = name, .returnType = result, .parameters = std::move(arguments), .binding = {.backend = ci::Backend::Cpp, .function = name, .version = 1, .address = address}});
    };
#define ADD_MIXED(NAME, TYPE, CPP) Add(NAME, TYPE, reinterpret_cast<void*>(&ReturnMixed<CPP>), MixedParameters())
    ADD_MIXED("Void", "System.Void", void);
    ADD_MIXED("Boolean", "System.Boolean", bool);
    ADD_MIXED("SByte", "System.SByte", int8_t);
    ADD_MIXED("Byte", "System.Byte", uint8_t);
    ADD_MIXED("Int16", "System.Int16", int16_t);
    ADD_MIXED("UInt16", "System.UInt16", uint16_t);
    ADD_MIXED("Char", "System.Char", uint16_t);
    ADD_MIXED("Int32", "System.Int32", int32_t);
    ADD_MIXED("UInt32", "System.UInt32", uint32_t);
    ADD_MIXED("Int64", "System.Int64", int64_t);
    ADD_MIXED("UInt64", "System.UInt64", uint64_t);
    ADD_MIXED("Single", "System.Single", float);
    ADD_MIXED("Double", "System.Double", double);
    ADD_MIXED("Object", "System.Object", void*);
    ADD_MIXED("Category", "via.motion.SequenceCategory", Category);
    ADD_MIXED("String", "System.String", void*);
    ADD_MIXED("Array", "System.Object[]", void*);
#undef ADD_MIXED
    Add("Reverse", "System.Double", reinterpret_cast<void*>(&Reverse), {{"a", "System.Int32"}, {"b", "System.Single"}, {"c", "System.Object"}, {"d", "System.Double"}, {"e", "System.Boolean"}, {"f", "System.UInt32"}, {"g", "System.Single"}, {"h", "System.Object"}, {"i", "System.Double"}});
    Add("Zero", "System.UInt64", reinterpret_cast<void*>(&Zero), {});
    Add("One", "System.Int32", reinterpret_cast<void*>(&One), {{"value", "System.Int32"}});
    std::vector<ci::ParameterSpec> many;
    for (size_t index = 0; index < 520; ++index) many.emplace_back("p" + std::to_string(index), "System.Int32");
    Add("Many", "System.Int64", reinterpret_cast<void*>(ManyEntry(std::make_index_sequence<520>{})), std::move(many));
    Add("RefValues", "System.Void", reinterpret_cast<void*>(&RefValues), {{"value", "System.Double", ci::ParameterModifier::Ref}, {"output", "System.Int32", ci::ParameterModifier::Out}, {"factor", "System.Single"}, {"id", "System.UInt32"}, {"peer", "System.Object"}, {"addend", "System.Double"}});
    Add("ColorArgument", "System.UInt32", reinterpret_cast<void*>(&ColorArgument), {{"color", "via.Color"}, {"factor", "System.Single"}, {"id", "System.UInt32"}, {"peer", "System.Object"}, {"number", "System.Double"}});
    auto previous = std::move(ci::g_database);
    ci::g_database = ci::BuildTDB(header, {{.name = "Tests.InvokeSignatures", .methods = methods}});
    auto& database = *ci::g_database;
    database.encodingBase = uintptr_t(GetModuleHandleW(nullptr));
    // 小签名可能已有原 recipe，离线强制同样经过生成器，确保零/一参数分支也被实际执行。
    for (auto& method : database.customMethods) {
        database.invokeSignatures[method.index - database.originalMethodCount] = ci::ClassifyInvokeSignature(database, database.methodDeclarations[method.index - database.originalMethodCount]);
        const auto& definition = database.methods[method.index];
        Check(bool(database.methodImpls[definition.impl].name & 0x10000000) == (database.methodDeclarations[method.index - database.originalMethodCount].returnType != "System.Void"), "CLR has-return metadata bit agrees with signature");
    }
    ci::CreateMethodTrampolines(database);
    ci::PrepareInvokeAdapters(database);
    auto* type = database.FindType("Tests.InvokeSignatures");
    const auto first = uint32_t(database.types[type->index].memberMethodIndex);
    int context{}, self{}, peer{};
    std::array<uintptr_t, 16> input{std::bit_cast<uint64_t>(1.25), std::bit_cast<uint64_t>(-9.5), uintptr_t(intptr_t(-17)), 0xF1234567, uintptr_t(&peer), 9, std::bit_cast<uint64_t>(2.5), std::bit_cast<uint64_t>(1.0000000000000002), uintptr_t(-0x123456789ABCll), 0xFEDCBA9876543210ull, uintptr_t(intptr_t(-101)), 231, uintptr_t(intptr_t(-12345)), 54321, 0x4E2D, 3};
    auto Invoke = [&](size_t index, const uintptr_t* args) {
        ci::InvokeFrame frame{.method = &database.methods[first + index], .arguments = args, .self = &self};
        Check(ci::InvokeGeneratedMethod(&frame, &context), "signature dispatch");
        DWORD64 base{};
        Check(RtlLookupFunctionEntry(DWORD64(database.FindMethod(first + uint32_t(index))->invokeAdapter), &base, nullptr) != nullptr, "all generated shapes have unwind");
        return uintptr_t(frame.result);
    };
    for (size_t index = 0; index < 17; ++index) {
        auto result = Invoke(index, input.data());
        Check(Last.valid && Last.context == uintptr_t(&context) && Last.self == uintptr_t(&self) && Last.peer == uintptr_t(&peer), "mixed floating/register/stack/narrow integer inputs");
        switch (index) {
        case 0: Check(!result, "Void result"); break;
        case 1: Check(result == 1, "Boolean result"); break;
        case 2: Check(int8_t(result) == -101, "SByte result"); break;
        case 3: Check(uint8_t(result) == 231, "Byte result"); break;
        case 4: Check(int16_t(result) == -12345, "Int16 result"); break;
        case 5: case 6: Check(uint16_t(result) == 54321, "UInt16/Char result"); break;
        case 7: Check(int32_t(result) == -123456789, "Int32 result"); break;
        case 8: Check(uint32_t(result) == 0xF1234567, "UInt32 result"); break;
        case 9: Check(int64_t(result) == -0x123456789ABCll, "Int64 result"); break;
        case 10: Check(result == 0xFEDCBA9876543210ull, "UInt64 result"); break;
        case 11: Check(std::bit_cast<double>(result) == 1.25, "Single widened result"); break;
        case 12: Check(std::bit_cast<double>(result) == -9.5, "Double result"); break;
        case 14: Check(int32_t(result) == 3, "enum result"); break;
        default: Check(result == uintptr_t(&peer), "managed Object/String/Array pointer result"); break;
        }
    }
    for (double sample : std::array<double, 4>{-0.0, INFINITY, -INFINITY, NAN}) {
        input[0] = input[1] = std::bit_cast<uint64_t>(sample);
        const auto single = std::bit_cast<double>(Invoke(11, input.data()));
        const auto real = std::bit_cast<double>(Invoke(12, input.data()));
        Check(std::isnan(sample) ? std::isnan(single) && std::isnan(real) : std::bit_cast<uint64_t>(single) == std::bit_cast<uint64_t>(sample) && std::bit_cast<uint64_t>(real) == std::bit_cast<uint64_t>(sample), "floating special values and signed zero");
    }
    uintptr_t reverse[]{uintptr_t(intptr_t(-7)), std::bit_cast<uint64_t>(0.5), uintptr_t(&peer), std::bit_cast<uint64_t>(3.25), 1, 0x81234567, std::bit_cast<uint64_t>(2.5), uintptr_t(&peer), std::bit_cast<uint64_t>(4.125)};
    Check(std::bit_cast<double>(Invoke(17, reverse)) == 10.375 && Last.valid, "integer then float register ordering");
    Check(Invoke(18, nullptr) == 0xFEDCBA9876543210ull, "zero arguments");
    uintptr_t one[]{uintptr_t(intptr_t(-17))};
    Check(int32_t(Invoke(19, one)) == -34, "one argument");
    std::array<uintptr_t, 520> large;
    for (size_t index = 0; index < large.size(); ++index) large[index] = index + 1;
    Check(Invoke(20, large.data()) == 135460, "520 arguments and guard-page stack probing");
    ThrowMany = true;
    bool unwound{};
    try { Invoke(20, large.data()); } catch (const std::runtime_error& error) { unwound = std::string_view(error.what()) == "large-frame unwind canary"; }
    ThrowMany = false;
    Check(unwound, "large frame unwind");
    double number = 4.5; int32_t output{};
    uintptr_t ref[]{uintptr_t(&number), uintptr_t(&output), std::bit_cast<uint64_t>(2.0), 0xF1234567, uintptr_t(&peer), std::bit_cast<uint64_t>(0.25)};
    Invoke(21, ref);
    Check(number == 9.25 && uint32_t(output) == (0xF1234567 ^ uint32_t(uintptr_t(&peer))), "generated ref/out and mixed floats");
    uint32_t color = 0x12345678;
    uintptr_t value[]{uintptr_t(&color), std::bit_cast<uint64_t>(1.0), 0xFEDCBA98, uintptr_t(&peer), std::bit_cast<uint64_t>(2.0)};
    Check(uint32_t(Invoke(22, value)) == (color ^ 0xFEDCBA98 ^ 3), "4-byte nonprimitive argument uses payload pointer");
    for (const auto* name : {"via.Color", "ace.ACTION_ID", "via.vec3"}) {
        auto invalid = methods[0]; invalid.returnType = name;
        Check(ci::ClassifyInvokeSignature(database, invalid).result == ci::InvokeValueKind::Unsupported, "hidden-buffer return not generated");
        bool rejected{};
        try { ci::BuildTDB(header, {{.name = "Tests.NoHiddenReturn", .methods = {invalid}}}); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "missing recipe with hidden return is rejected");
    }
    Check(database.FindMethod(first + 5)->invokeAdapter == database.FindMethod(first + 6)->invokeAdapter, "UInt16 and Char share ABI code");
    Check(database.FindMethod(first + 13)->invokeAdapter == database.FindMethod(first + 15)->invokeAdapter && database.FindMethod(first + 15)->invokeAdapter == database.FindMethod(first + 16)->invokeAdapter, "all managed reference returns share ABI code");
    std::cout << "PASS: generated signatures, all scalar/reference returns, mixed register/stack floats, enum, ref/out, 520 arguments/probing/unwind, code sharing and hidden-return rejection\n";
    ci::g_database = std::move(previous);
}
#else
static void InitializeLua(lua_State* state) {
    sol::state_view lua{state};
    auto probe = lua.create_named_table("ci_invoke_signature_probe");
    probe["receipt"] = [](sol::this_state state) {
        sol::state_view lua{state};
        auto result = lua.create_table();
        result["valid"] = Last.valid; result["context"] = Last.context; result["self"] = Last.self; result["peer"] = Last.peer;
        result["calls"] = Last.calls; result["entry"] = Last.entry; result["depth"] = Last.depth;
        return result;
    };
    probe["metadata"] = [](sol::this_state state) {
        sol::state_view lua{state}; auto result = lua.create_table();
        auto& api = reframework::API::get(); auto* tdb = reinterpret_cast<RE::TDB*>(api->tdb());
        auto* type = api->tdb()->find_type("ClassInjector.Tests.Signatures");
        for (auto* method : type->get_methods()) {
            auto& definition = static_cast<ci::MethodDef*>(tdb->methods)[method->get_index()];
            auto* list = reinterpret_cast<const ci::ParamList*>(static_cast<const uint8_t*>(tdb->bytePool) + definition.GetParameterOffset());
            auto item = lua.create_table();
            item["invokeId"] = list->invokeId; item["count"] = list->count; item["methodId"] = method->get_index();
            result[method->get_name()] = item;
        }
        return result;
    };
}
#endif
#undef MIXED_PARAMETERS
#undef MIXED_VALUES
} // namespace ClassInjectorSignatureTests

#ifndef CI_INVOKE_SIGNATURE_PROVIDER
void TestInvokeSignatures(const RE::TDB& header) { ClassInjectorSignatureTests::Run(header); }
#else
extern "C" __declspec(dllexport) bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* parameter) {
    reframework::API::initialize(parameter);
    parameter->functions->on_lua_state_created(ClassInjectorSignatureTests::InitializeLua);
    return true;
}
CI_REGISTER_PROVIDER(registration) {
    using namespace ClassInjectorSignatureTests;
    registration.register_func("Void", &ReturnMixed<void>, 1).register_func("Boolean", &ReturnMixed<bool>, 1);
    registration.register_func("SByte", &ReturnMixed<int8_t>, 1).register_func("Byte", &ReturnMixed<uint8_t>, 1);
    registration.register_func("Int16", &ReturnMixed<int16_t>, 1).register_func("UInt16", &ReturnMixed<uint16_t>, 1);
    registration.register_func("Char", &ReturnMixed<uint16_t>, 1).register_func("Int32", &ReturnMixed<int32_t>, 1);
    registration.register_func("UInt32", &ReturnMixed<uint32_t>, 1).register_func("Int64", &ReturnMixed<int64_t>, 1);
    registration.register_func("UInt64", &ReturnMixed<uint64_t>, 1).register_func("Single", &ReturnMixed<float>, 1);
    registration.register_func("Double", &ReturnMixed<double>, 1).register_func("Object", &ReturnMixed<void*>, 1);
    registration.register_func("Category", &ReturnMixed<Category>, 1).register_func("String", &ReturnMixed<void*>, 1);
    registration.register_func("Array", &ReturnMixed<void*>, 1).register_func("Reverse", &Reverse, 1);
    registration.register_func("Zero", &Zero, 1).register_func("One", &One, 1);
}
#endif

