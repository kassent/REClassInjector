#include "ClassInjectorTestAccess.h"
#include "ClassProviderContext.h"
#include "ClassProviders.h"
#include "ProviderMetadataAPI.h"
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <Windows.h>
#include <array>
#include <ci/API.hpp>
#include <ci/Provider.hpp>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace ProviderSDKTests {
    static void Check(bool value, const char* text) {
        if (!value)
            throw std::runtime_error(text);
    }

    static std::vector<int> trace;

    static void TestRegistration() {
        struct Collected {
            uint32_t functionCalls{};
            uint32_t enumCalls{};
            bool rejectEnum{};
            std::vector<std::string> names;
        } collected;

        const ci::RegistrationAPI api{
            .context = &collected,
            .register_func =
                [](void* context, const char* name, void* address, uint32_t version) {
                    auto& collected = *static_cast<Collected*>(context);
                    ++collected.functionCalls;
                    return address && version == 7 && std::string_view(name) != "Rejected";
                },
            .register_enum =
                [](void* context, const char* typeName, const ci::EnumRename* renames, size_t renameCount,
                   const ci::EnumMember* additions, size_t additionCount) {
                    auto& collected = *static_cast<Collected*>(context);
                    ++collected.enumCalls;
                    Check(std::string_view(typeName) == "Tests.Color", "enum type was not forwarded");
                    if (renameCount) {
                        Check(renameCount == 2 && renames[0].kind == ci::EnumMatchKind::Name &&
                                  std::string_view(renames[0].from_name) == "Zero" &&
                                  renames[1].kind == ci::EnumMatchKind::Value && renames[1].from_value == 0,
                              "mixed name/value zero selectors or rename order changed");
                        collected.names.emplace_back(renames[0].to_name);
                        collected.names.emplace_back(renames[1].to_name);
                    }
                    if (additionCount) {
                        Check(additionCount == 1 && additions[0].value == -1, "addition payload changed");
                        collected.names.emplace_back(additions[0].name);
                    }
                    return !collected.rejectEnum;
                }};
        const auto function = +[](void* vmContext, void* object) noexcept {};
        ci::Registration registration(api);
        Check(!registration.has_errors(), "empty registration reported an error");
        registration.register_func("First", function, 7).register_func("Second", function, 7);
        registration.register_enum("Tests.Color", {.renames = {ci::EnumRename::by_name("Zero", "Nil"),
                                                               ci::EnumRename::by_value(0, "Canonical")}});
        registration.register_enum("Tests.Color", {.additions = {{.name = "Negative", .value = -1}}});
        registration.register_enum("Tests.Color", {.renames = {ci::EnumRename::by_name("Zero", "Nil"),
                                                               ci::EnumRename::by_value(0, "Canonical")},
                                                   .additions = {{.name = "Both", .value = -1}}});
        Check(!registration.has_errors() && collected.functionCalls == 2 && collected.enumCalls == 3 &&
                  collected.names ==
                      std::vector<std::string>{"Nil", "Canonical", "Negative", "Nil", "Canonical", "Both"},
              "named enum changes or function binding failed");
        registration.register_func("Rejected", function, 7).register_func("AcceptedAfterFailure", function, 7);
        Check(registration.has_errors(), "successful binding cleared an earlier registration error");
        ci::Registration enumFailure(api);
        collected.rejectEnum = true;
        enumFailure.register_enum("Tests.Color", {.additions = {{.name = "Rejected", .value = -1}}});
        collected.rejectEnum = false;
        enumFailure.register_func("Accepted", function, 7);
        Check(enumFailure.has_errors(), "enum callback rejection was lost");
        auto functionsOnly = api;
        functionsOnly.register_enum = nullptr;
        ci::Registration unavailable(functionsOnly);
        unavailable.register_func("Accepted", function, 7);
        Check(!unavailable.has_errors(), "function-only registration failed");
        unavailable.register_enum("Tests.Color", {.additions = {{.name = "Unavailable", .value = -1}}});
        Check(unavailable.has_errors(), "missing optional enum callback was accepted");
        std::cout
            << "PASS: snake_case registration, named changes, ordered selectors, zero, additions and sticky errors\n";
    }

    static void TestPhases() {
        std::vector<ci::ClassProviderNotification> pending{
            {[](const ci::ProviderContext* context) noexcept {
                 trace.push_back(1);
             },
             {},
             [](const ci::ProviderContext* context) noexcept {
                 trace.push_back(3);
             }},
            {[](const ci::ProviderContext* context) noexcept {
                 trace.push_back(2);
             },
             {}},
            {nullptr, {}, [](const ci::ProviderContext* context) noexcept {
                 trace.push_back(4);
             }}};
        ci::ProviderContext context{};
        ci::NotifyClassProviders(pending, context);
        ci::NotifyClassProviders(pending, context);
        Check(pending.empty() && trace == std::vector<int>{1, 2, 3, 4}, "two phase order / final-only / consume once");
    }

    static void TestFinalOnlyProvider(const char* path) {
        ci::ProviderConfig config{.configPath = "final-only.toml", .dllPath = std::filesystem::absolute(path)};
        std::vector<ci::ClassProviderNotification> pending;
        const auto declarations = ci::ResolveClassProviders({config}, &pending);
        Check(declarations.empty() && pending.size() == 1 && !pending[0].initialize && pending[0].postInitialize,
              "final-only DLL must be accepted and queued");
        const auto module = GetModuleHandleW(config.dllPath.c_str());
        Check(GetProcAddress(module, "ci_on_post_initialize") && !GetProcAddress(module, "ci_on_initialize"),
              "post-initialization-only provider export contract");
        auto calls = reinterpret_cast<uint32_t (*)()>(GetProcAddress(module, "ci_test_final_phase_calls"));
        Check(calls && calls() == 0, "no early notification");
        ci::ProviderContext context{};
        ci::NotifyClassProviders(pending, context);
        ci::NotifyClassProviders(pending, context);
        Check(calls() == 1, "loaded final-only provider notified exactly once");
    }

    static void TestMetadata() {
        auto previous = std::move(ci::g_database);

        struct Restore {
            std::unique_ptr<ci::TypeDatabase> previous;

            ~Restore() {
                ci::g_database = std::move(previous);
            }
        } restore{std::move(previous)};

        ci::g_database = std::make_unique<ci::TypeDatabase>();
        auto& database = *ci::g_database;
        database.types.resize(4);
        database.typeImpls.resize(4);
        database.strings.push_back(0);
        for (uint32_t i = 1; i < 3; ++i) {
            database.types[i].index = database.types[i].implIndex = i;
            database.typeImpls[i].nameOffset = database.AppendString(i == 1 ? "Int32" : "Owner");
            database.typeImpls[i].namespaceOffset = database.AppendString(i == 1 ? "System" : "Tests");
        }
        database.types[1].objectType = 5;
        database.types[1].underlyingTypeIndex = 8;
        database.typeImpls[1].fieldSize = 4;
        database.types[2].objectType = 1;
        database.typeNames = {{"System.Int32", 1}, {"Tests.Owner", 2}};
        database.types[3].index = database.types[3].implIndex = 3;
        database.types[3].objectType = 5;
        database.types[3].underlyingTypeIndex = 11;
        database.typeImpls[3].fieldSize = 8;
        database.typeImpls[3].nameOffset = database.AppendString("UInt64");
        database.typeImpls[3].namespaceOffset = database.AppendString("System");
        database.typeNames.emplace("System.UInt64", 3);
        database.typeImpls[2].numMemberFields = 3;
        database.fieldImpls.resize(3);
        database.fields.resize(3);
        database.fieldImpls[0].name = database.AppendString("Value");
        database.fieldImpls[0].offset = 4;
        database.fieldImpls[1].name = database.AppendString("MAX");
        database.fieldImpls[1].flags = 0x50;
        database.fieldImpls[1].initLo = 1;
        database.fields[0] = {.declaringType = 2, .impl = 0, .type = 1};
        database.fields[1] = {.declaringType = 2, .impl = 1, .type = 1};
        database.bytes.resize(4);
        int32_t zero = 0;
        database.initializers = {0, int32_t(database.AppendBytes(&zero, 4))};
        database.fieldImpls[2].name = database.AppendString("Wide");
        database.fieldImpls[2].flags = 0x50;
        database.fieldImpls[2].initLo = 2;
        database.fields[2] = {.declaringType = 2, .impl = 2, .type = 3};
        const uint64_t maximum = UINT64_MAX;
        database.initializers.push_back(int32_t(database.AppendBytes(&maximum, sizeof(maximum))));
        Check(ci::GetContext().sdk->tdb == ci::GetMetadataAPI().tdb, "provider context publishes metadata SDK");
        ci::API api{*ci::GetContext().sdk};
        auto owner = api.tdb().find_type("Tests.Owner");
        Check(bool(owner) && !api.tdb().find_type("Missing"), "type lookup");
        auto fields = owner.get_fields();
        Check(fields.size() == 3 && fields[1].is_static() && fields[1].is_literal(), "declared static/literal fields");
        uint64_t wide{};
        std::memcpy(&wide, owner.find_field("Wide").get_init_data(), sizeof(wide));
        Check(wide == UINT64_MAX && std::string_view(owner.get_full_name()) == "Tests.Owner",
              "UInt64 initializer and borrowed full name");
        auto max = owner.find_field("MAX");
        Check(bool(max) && database.GetFieldDef(&database.types[2], "MAX") == nullptr,
              "declaration versus instance lookup");
        int32_t value = -1;
        Check(max.get_init_data() != nullptr, "zero constant has data");
        std::memcpy(&value, max.get_init_data(), sizeof(value));
        Check(value == 0, "zero constant value");
        database.initializers[1] = INT32_MIN;
        Check(max.get_init_data() == nullptr, "bad signed pool offset");
        const int32_t negative = -7;
        const auto offset = database.strings.size();
        const auto* data = reinterpret_cast<const char*>(&negative);
        database.strings.insert(database.strings.end(), data, data + sizeof(negative));
        database.initializers[1] = -int32_t(offset);
        std::memcpy(&value, max.get_init_data(), sizeof(value));
        Check(value == -7, "negative string pool offset");
        const auto& sdk = ci::GetMetadataAPI();
        ci::FieldHandle canary = reinterpret_cast<ci::FieldHandle>(uintptr_t(0x1234));
        uint32_t count = 99;
        Check(sdk.type_definition->get_fields(owner.handle, &canary, sizeof(canary), &count) ==
                      ci::ERROR_OUT_TOO_SMALL &&
                  count == 0 && canary == reinterpret_cast<ci::FieldHandle>(uintptr_t(0x1234)),
              "byte capacity / no partial write");
        auto field = owner.find_field("Value");

        struct Info {
            intptr_t base;
            uintptr_t type;
        } info{16, 0};

        std::array<uint8_t, 32> object{};
        const auto infoAddress = reinterpret_cast<uintptr_t>(&info.type);
        std::memcpy(object.data(), &infoAddress, sizeof(infoAddress));
        Check(field.get_data_raw(object.data()) == object.data() + 20, "actual object field base");
        Check(field.get_data_raw(object.data() + 16, true) == object.data() + 20, "unboxed payload addressing");
        Check(max.get_data_raw(object.data()) == max.get_init_data(),
              "literal field reads initializer, not object storage");
        Check(owner.get_methods().empty(), "empty methods buffer");
        Check(field.get_type().handle == api.tdb().find_type("System.Int32").handle, "field type handle");
    }

    static void TestRawInitializers() {
        struct Restore {
            std::unique_ptr<ci::TypeDatabase> previous;

            ~Restore() {
                ci::g_database = std::move(previous);
            }
        } restore{std::move(ci::g_database)};

        ci::g_database = std::make_unique<ci::TypeDatabase>();
        auto& database = *ci::g_database;
        database.types.resize(7);
        database.typeImpls.resize(7);
        database.fields.resize(7);
        database.fieldImpls.resize(7);
        database.initializers.resize(7);
        database.bytes.resize(1); // Offset zero means no initializer, not numeric value zero.
        database.strings.push_back(0);
        for (uint32_t index = 1; index < database.types.size(); ++index) {
            database.types[index].index = index;
            database.types[index].implIndex = index;
            database.types[index].objectType = 5;
        }
        database.typeImpls[1].fieldSize = sizeof(int32_t);
        database.typeImpls[2].fieldSize = sizeof(int64_t);
        database.typeImpls[3].fieldSize = 12;
        database.types[4].objectType = 1; // Reference initializer encodes pool data, not an object.
        database.typeImpls[4].fieldSize = 16;
        database.types[5].objectType = 1;
        database.typeImpls[5].numMemberFields = uint32_t(database.fields.size());
        const char* names[]{"Rva32", "Rva64", "RvaBlob", "Text", "InstanceDefault", "UnknownLayout", "NoData"};
        const uint32_t types[]{1, 2, 3, 4, 1, 6, 1};
        for (uint32_t index = 0; index < database.fields.size(); ++index) {
            database.fields[index] = {.declaringType = 5, .impl = index, .type = types[index]};
            database.fieldImpls[index].name = database.AppendString(names[index]);
            database.fieldImpls[index].flags =
                0x133; // Real TDB RVA shape: Static + InitOnly + HasFieldRVA, no Literal.
            database.fieldImpls[index].initLo = index + 1;
        }
        database.fieldImpls[3].flags = 0x56;
        database.fieldImpls[4].flags = 0x6; // Instance metadata initializer: neither Static nor Literal.
        database.fieldImpls[6].flags = 0;
        database.fieldImpls[6].initLo = 0;
        const int32_t zero = 0;
        const int64_t negative = INT64_MIN + 5;
        const std::array<uint8_t, 12> blob{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        const int32_t defaultValue = 37;
        database.initializers[1] = int32_t(database.AppendBytes(&zero, sizeof(zero)));
        database.initializers[2] = int32_t(database.AppendBytes(&negative, sizeof(negative)));
        database.initializers[3] = int32_t(database.AppendBytes(blob.data(), blob.size()));
        database.initializers[4] = -int32_t(database.AppendString("pool"));
        database.initializers[5] = int32_t(database.AppendBytes(&defaultValue, sizeof(defaultValue)));
        database.initializers[6] = database.initializers[1];

        const ci::API::TypeDefinition owner{ci::GetContext().sdk,
                                            reinterpret_cast<ci::TypeDefinitionHandle>(&database.types[5])};
        const auto rva32 = owner.find_field("Rva32");
        const auto rva64 = owner.find_field("Rva64");
        const auto rvaBlob = owner.find_field("RvaBlob");
        Check(rva32.is_static() && !rva32.is_literal(), "RVA regression must not use a literal fixture");
        Check(rva32.get_init_data() && rva64.get_init_data(), "nonliteral numeric initializer was filtered");
        int32_t actual32 = -1;
        int64_t actual64 = 0;
        std::memcpy(&actual32, rva32.get_init_data(), sizeof(actual32));
        std::memcpy(&actual64, rva64.get_init_data(), sizeof(actual64));
        Check(actual32 == zero && actual64 == negative, "nonliteral Int32/Int64 initializer bytes changed");
        Check(rvaBlob.get_init_data() && std::memcmp(rvaBlob.get_init_data(), blob.data(), blob.size()) == 0,
              "RVA value payload initializer was filtered");
        const auto text = owner.find_field("Text").get_init_data();
        Check(text && std::string_view(static_cast<const char*>(text)) == "pool",
              "raw reference initializer is not pool data");
        const auto instanceDefault = owner.find_field("InstanceDefault");
        Check(!instanceDefault.is_static() && !instanceDefault.is_literal() && instanceDefault.get_init_data(),
              "raw initializer incorrectly applies static/literal policy");
        std::memcpy(&actual32, instanceDefault.get_init_data(), sizeof(actual32));
        Check(actual32 == defaultValue, "instance metadata default bytes changed");
        Check(rva32.get_data_raw(nullptr) == nullptr, "initializer lookup must not expose runtime static storage");
        Check(!owner.find_field("UnknownLayout").get_init_data() && !owner.find_field("NoData").get_init_data(),
              "missing value layout or absent initializer was accepted");
        const auto savedBlobOffset = database.initializers[3];
        database.initializers[3] = int32_t(database.bytes.size() - blob.size() + 1);
        Check(!rvaBlob.get_init_data(), "truncated RVA value payload was accepted");
        database.initializers[3] = savedBlobOffset;
        database.fieldImpls[6].initMid = 1;
        Check(!owner.find_field("NoData").get_init_data(), "out-of-range initializer index was accepted");
        auto capturedInitializers = std::move(database.initializers);
        database.header.initData = capturedInitializers.data();
        database.header.numInitData = int32_t(capturedInitializers.size());
        Check(rva64.get_init_data() && std::memcmp(rva64.get_init_data(), &negative, sizeof(negative)) == 0,
              "header initializer table fallback changed");
        database.header.initData = nullptr;
        Check(!rva64.get_init_data(), "missing initializer table was accepted");
        std::cout
            << "PASS: raw nonliteral RVA/default/reference initializers, zero, widths, bounds and table fallback\n";
    }
} // namespace ProviderSDKTests

int main(int argc, char** argv) {
    try {
        ProviderSDKTests::TestMetadata();
        ProviderSDKTests::TestRegistration();
        ProviderSDKTests::TestRawInitializers();
        ProviderSDKTests::TestPhases();
        if (argc > 1)
            ProviderSDKTests::TestFinalOnlyProvider(argv[1]);
        std::cout << "PASS: SDK metadata, initializer pools, field addressing, buffers and two-phase notifications\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
