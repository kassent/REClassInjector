#include "ProviderTestAccess.h"
using namespace CIProviderTestAccess;
#include "ClassInjectorTestAccess.h"
#include "ClassMethods.h"
#include "ClassProviderContext.h"
#include "ClassProviders.h"
#include "ClassVirtualTable.h"
#include "CustomTypeBuildSpec.h"
#include "MethodTrampolines.h"
#include "NativeCallProbe.h"
#include "ProviderMetadataAPI.h"
#include "TypeDatabaseBuilder.h"
#include "TypeInjection.h"
#include <BranchTrampoline.h>
#include <HookUtils.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>

void TestInvokeSignatures(const RE::TDB& header);

namespace {
    // 独立排序测试显式进入构建模型，不让作者声明携带合成标志。
    static std::vector<ci::CustomTypeBuildSpec> OrderDeclarationsForTest(std::vector<ci::CustomTypeConfig> declarations) {
        std::vector<ci::CustomTypeBuildSpec> specs;
        for (auto& declaration : declarations) specs.push_back({std::move(declaration)});
        return ci::OrderTypeDeclarations(std::move(specs));
    }

    int g_ctorCalls{};
    void* g_ctorContext{};
    void* g_ctorSelf{};
    void ObjectCtor(void* context, void* self) {
        ++g_ctorCalls;
        g_ctorContext = context;
        g_ctorSelf = self;
    }
    void Check(bool value, const char* message) {
        if (!value) {
            throw std::runtime_error(message);
        }
    }
    // Test/report snapshot read from final tables, never retained by the production database.
    struct InstanceFieldSnapshot {
        std::string name;
        uint32_t offset;
        uint32_t type;
        bool reference;
    };
    static std::vector<InstanceFieldSnapshot> ReadInstanceFields(const ci::TypeDatabase& database, uint32_t index) {
        if (!index) return {};
        const auto& type = database.types[index];
        auto result = ReadInstanceFields(database, uint32_t(type.parentTypeIndex));
        const auto& implementation = database.typeImpls[type.implIndex];
        for (uint32_t i = 0; i < implementation.numMemberFields; ++i) {
            const auto& field = database.fields[type.memberFieldIndex + i];
            const auto& layout = database.fieldImpls[field.impl];
            if (layout.flags & 0x10) continue;
            result.push_back({database.strings.data() + layout.name, uint32_t(layout.offset),
                              uint32_t(field.type), database.IsReferenceType(uint32_t(field.type))});
        }
        return result;
    }
    // 离线对象没有游戏 SDK；仅模拟 get_field 所需的三项标准 API。
    // 实现 DLL 仍调用原 SDK 接口，不向生产注入器增加字段服务或初始化协议。
    const REFrameworkPluginInitializeParam* GetMockSdk() {
        static const REFrameworkManagedObject object{.get_type_definition = [](REFrameworkManagedObjectHandle self) {
            return reinterpret_cast<REFrameworkTypeDefinitionHandle>(**reinterpret_cast<void***>(self));
        }};
        static const REFrameworkTDBTypeDefinition type{.find_field = [](REFrameworkTypeDefinitionHandle definition,
                                                                        const char* name) {
            auto* value = reinterpret_cast<RE::RETypeDefinition*>(definition);
            return reinterpret_cast<REFrameworkFieldHandle>(const_cast<ci::FieldDef*>(ci::g_database->GetFieldDef(value, name)));
        }};
        static const REFrameworkTDBField field{
            .get_data_raw = [](REFrameworkFieldHandle definition, void* self, bool) -> void* {
                auto** info = *static_cast<void***>(self);
                auto* value = reinterpret_cast<const ci::FieldDef*>(definition);
                return static_cast<uint8_t*>(self) + int32_t(intptr_t(info[-1])) + ci::g_database->fieldImpls[value->impl].offset;
            }};
        static const REFrameworkSDKData sdk{.type_definition = &type, .field = &field, .managed_object = &object};
        static const REFrameworkPluginInitializeParam parameter{.sdk = &sdk};
        return &parameter;
    }

    void InitializeTestProvider(HMODULE module, bool runProviderInitialization = true) {
        const auto initializeREFramework =
            reinterpret_cast<REFPluginInitializeFn>(GetProcAddress(module, "reframework_plugin_initialize"));
        Check(initializeREFramework && initializeREFramework(GetMockSdk()), "test provider SDK initialization");
        const auto initializeProvider =
            reinterpret_cast<ci::FnInitializationCallback>(GetProcAddress(module, "ci_on_initialize"));
        if (initializeProvider && runProviderInitialization) {
            initializeProvider(&ci::GetContext());
        }
    }
    template <class F> void Rejects(F&& function) {
        bool rejected = false;
        try {
            function();
        } catch (const std::exception&) {
            rejected = true;
        }
        Check(rejected, "expected rejection");
    }

    ci::ProviderConfig ReadFixture(const char* directory, const char* filename) {
        auto source = std::filesystem::path(CI_TEST_MANIFESTS) / directory / filename;
        std::ifstream stream(source, std::ios::binary);
        std::string description{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        return ci::ParseProviderConfig(description, source);
    }

    struct FiveArgumentReceipt {
        void* context{};
        void* self{};
        void* first{};
        int32_t second{};
        void* third{};
        int32_t fourth{};
        uint32_t fifth{};
    };
    FiveArgumentReceipt g_five;
    void ReceiveFive(void* context, void* self, void* first, int32_t second, void* third, int32_t fourth, uint32_t fifth) {
        g_five = {context, self, first, second, third, fourth, fifth};
    }
    void ThrowFive(void*, void*, void*, int32_t, void*, int32_t, uint32_t) {
        throw std::runtime_error("five-argument unwind canary");
    }
    void TestFiveArgumentInvoker(const RE::TDB& header) {
        ci::MethodSpec generated{.name = "init", .parameters = {{"first", "System.Object"}, {"second", "System.Int32"}, {"third", "System.Object"}, {"fourth", "System.Int32"}, {"fifth", "System.UInt32"}}, .binding = {.backend = ci::Backend::Cpp, .function = "ReceiveFive", .version = 1, .address = reinterpret_cast<void*>(&ReceiveFive)}};
        auto borrowed = generated;
        borrowed.name = "original";
        borrowed.parameters[4].type = "System.Boolean";
        auto throwing = generated;
        throwing.name = "throwing";
        throwing.binding.address = reinterpret_cast<void*>(&ThrowFive);
        ci::CustomTypeConfig type{.name = "Tests.FiveArgumentInvoker", .methods = {generated, borrowed, throwing}};
        auto previous = std::move(ci::g_database);
        ci::g_database = ci::BuildTypeDatabase(header, {type});
        auto& database = *ci::g_database;
        database.encodingBase = uintptr_t(GetModuleHandleW(nullptr));
        ci::CreateMethodTrampolines(database);
        ci::PrepareInvokeAdapters(database);
        auto* runtimeType = database.FindType(type.name);
        auto index = uint32_t(database.types[runtimeType->index].memberMethodIndex);
        auto* init = database.FindMethod(index);
        auto* originalMethod = database.FindMethod(index + 1);
        auto* exceptionMethod = database.FindMethod(index + 2);
        Check(init && std::string_view(database.strings.data() + (database.methodImpls[database.methods[init->index].impl].name & 0x0FFFFFFF)) == "init" && init->invokeAdapter && !originalMethod->invokeAdapter, "missing recipe selects generated adapter only");
        Check(init->invokeAdapter == exceptionMethod->invokeAdapter, "same ABI shares generated code");
        auto* list = reinterpret_cast<const ci::ParamList*>(database.bytes.data() + database.methods[index].GetParameterOffset());
        auto* oldList = reinterpret_cast<const ci::ParamList*>(database.bytes.data() + database.methods[index + 1].GetParameterOffset());
        Check(list->invokeId == oldList->invokeId && list->invokeId == database.invokeDispatchId && list->count == 5, "generated and original routes share the anchor ID with distinct adapters");
        int context{}, self{}, first{}, third{};
        uintptr_t args[]{uintptr_t(&first), uintptr_t(intptr_t(-17)), uintptr_t(&third), 23, 0xF1234567};
        ci::InvokeFrame frame{.method = &database.methods[index], .arguments = args, .self = &self};
        Check(ci::InvokeGeneratedMethod(&frame, &context), "generated method dispatch");
        Check(g_five.context == &context && g_five.self == &self && g_five.first == &first && g_five.second == -17 && g_five.third == &third && g_five.fourth == 23 && g_five.fifth == 0xF1234567, "all five parameters, sign, stack and UInt32 high bits preserved");
        DWORD64 base{};
        Check(RtlLookupFunctionEntry(DWORD64(init->invokeAdapter), &base, nullptr) == &database.invokeAdapterCodes.front()->function, "dynamic invoker has registered unwind");
        frame.method = &database.methods[index + 1];
        Check(!ci::InvokeGeneratedMethod(&frame, &context), "borrowed recipe continues to original wrapper");
        frame.method = &database.methods[1];
        Check(!ci::InvokeGeneratedMethod(&frame, &context), "original engine declaration continues to original wrapper");
        frame.method = &database.methods[index];
        auto offset = database.methods[index].encodedOffset;
        database.methods[index].encodedOffset = database.methods[index + 2].encodedOffset;
        bool unwound{};
        try { ci::InvokeGeneratedMethod(&frame, &context); }
        catch (const std::runtime_error& error) { unwound = std::string_view(error.what()) == "five-argument unwind canary"; }
        Check(unwound, "current method entry is honored and exception unwinds through generated machine code");
        database.methods[index].encodedOffset = offset;
        ci::g_database = std::move(previous);
        std::cout << "PASS: real TDB five-argument JIT, shared anchor/original routing, UInt32 high bits, dynamic unwind and fresh entry resolution\n";
    }

    void TestPluginRegistration() {
        auto basic = ReadFixture("basic", "CIExampleProvider_ci.toml");
        auto other = ReadFixture("types", "ManagedTypes_ci.toml");
        auto valid = ci::ResolveClassProviders({basic, other});
        Check(valid.size() == 4 && valid[0].methods[0].binding.address, "valid independent plugin pairs");
        Check(basic.dllPath == std::filesystem::path(CI_TEST_PROVIDER), "same-name DLL beside manifest");
        Check(ci::LoadClassProviders({basic.dllPath.parent_path(), other.dllPath.parent_path()}).size() == 4, "provider declarations are collected across directories");
        Check(ci::LoadClassProviders({basic.dllPath.parent_path() / "absent", other.dllPath.parent_path()}).size() == 2, "missing directory does not skip other provider directories");
        Check(basic.types[0].methods[1].parameters[0].type == "System.Int32", "separate parameter declaration");

        auto bad = basic;
        bad.types[0].methods[1].binding.function = "Missing";
        bad.types.push_back({.name = "System.Diagnostics.Stopwatch", .fields = {{"Extra"}}, .extend = true});
        auto remaining = ci::ResolveClassProviders({bad, other});
        Check(remaining.size() == 2 && remaining[0].name == other.types[0].name,
              "missing function skips whole file including extension, keeps unrelated plugin");

        for (auto method : {0u, 1u, 2u}) {
            bad = basic;
            ++bad.types[0].methods[method].binding.version;
            Check(ci::ResolveClassProviders({bad, other}).size() == 2,
                  "ctor/instance/virtual version mismatch is atomic");
        }
        bad = basic;
        bad.dllPath = basic.dllPath.parent_path() / "Missing_ci.dll";
        Check(ci::ResolveClassProviders({bad, other}).size() == 2, "missing DLL skips whole plugin");
        for (auto filename : {"Accepted_exe.dll", "Accepted.dll"}) {
            bad = basic;
            bad.dllPath = basic.dllPath.parent_path() / filename;
            std::filesystem::copy_file(basic.dllPath, bad.dllPath, std::filesystem::copy_options::overwrite_existing);
            Check(ci::ResolveClassProviders({bad, other}).size() == 4, "provider filename needs no CI suffix");
            Check(GetModuleHandleW(bad.dllPath.c_str()) != nullptr, "provider without CI suffix is loaded");
            // host 会持有模块到进程退出；测试释放本次加载的引用后清理复制品。
            FreeLibrary(GetModuleHandleW(bad.dllPath.c_str()));
            std::filesystem::remove(bad.dllPath);
        }
        auto uppercase = basic;
        uppercase.dllPath = basic.dllPath.parent_path() / "CIEXAMPLEPROVIDER_CI.DLL";
        Check(ci::ResolveClassProviders({uppercase}).size() == 2, "DLL path is case insensitive");
        Check(ci::ResolveClassProviders({basic, basic}).empty(), "DLL may only have one TOML owner");

        // 生产扫描入口：同名标记决定加载，TOML 内容和旧 plugin.dll 字段不决定 DLL 身份。
        auto directory = basic.dllPath.parent_path() / "discovery";
        std::filesystem::create_directory(directory);
        auto file = directory / "Discovery.dll";
        auto marker = directory / "Discovery.toml";
        std::filesystem::copy_file(basic.dllPath, file, std::filesystem::copy_options::overwrite_existing);
        Check(ci::LoadClassProviders({directory}).empty() && !GetModuleHandleW(file.c_str()), "DLL without TOML is not loaded");
        auto unmatched = directory / "Unmatched.toml";
        std::ofstream(unmatched) << "";
        Check(ci::LoadClassProviders({directory}).empty() && !GetModuleHandleW(file.c_str()), "different-name TOML does not select DLL");
        for (auto text : {"", "not valid TOML [", "[plugin]\ndll = 'Missing.dll'\n"}) {
            { std::ofstream stream(marker); stream << text; }
            Check(ci::LoadClassProviders({directory}).empty(), "marker-only provider has no type declarations");
            auto module = GetModuleHandleW(file.c_str());
            Check(module != nullptr, "same-name TOML selects provider regardless of content");
            FreeLibrary(module);
        }
        std::filesystem::remove(marker);
        std::filesystem::remove(unmatched);
        std::filesystem::remove(file);
        std::filesystem::remove(directory);

        bad = basic;
        bad.types[0].methods[0].binding.version = 99;
        auto dependent = other;
        dependent.types[0].fields.push_back(
            {"Items", "System.Collections.Generic.List`1<" + basic.types[0].name + "[]>"});
        Check(ci::ResolveClassProviders({dependent, bad}).empty(), "generic/array field dependency rejection");
        dependent = other;
        dependent.types[0].methods[1].returnType = basic.types[0].name;
        Check(ci::ResolveClassProviders({bad, dependent}).empty(), "method return dependency rejection");
        dependent = other;
        dependent.types[0].methods[1].parameters[0].type = basic.types[0].name;
        Check(ci::ResolveClassProviders({bad, dependent}).empty(), "method parameter dependency rejection");
        dependent = other;
        dependent.types[0].fields.push_back({"Different", basic.types[0].name + "2"});
        Check(ci::ResolveClassProviders({bad, dependent}).size() == 2, "type dependency checks name boundaries");

        dependent = other;
        dependent.types[0].base = basic.types[0].name;
        ci::ProviderConfig transitive{.configPath = "transitive.toml",
                                  .dllPath = CI_TEST_INTERFACE_PLUGIN,
                                  .types = {{.name = "Tests.Transitive", .base = dependent.types[0].name}}};
        Check(ci::ResolveClassProviders({transitive, dependent, bad}).empty(), "transitive whole-plugin rejection");
        auto early = ReadFixture("class", "ClassInjectorTests_ci.toml");
        std::vector<ci::ClassProviderNotification> callbacks;
        auto accepted = ci::ResolveClassProviders({basic, early}, &callbacks);
        Check(!accepted.empty() && callbacks.size() == 2,
              "optional ready export is collected only for accepted providers");
        Check(callbacks[0].providerSource == accepted[0].providerSource && callbacks[0].providerSource->configPath == basic.configPath.string() &&
                  callbacks[0].providerSource->dllPath == basic.dllPath.string(), "accepted declaration and notification share Provider identity");
        std::weak_ptr<const ci::ProviderSource> callbackSource = callbacks[0].providerSource;
        accepted.clear();
        Check(!callbackSource.expired(), "pending callback retains identity after declarations are released");
        callbacks.clear();
        Check(callbackSource.expired(), "consumed/discarded callbacks release Provider identity");
        auto earlyBad = early;
        ++earlyBad.types[0].methods[1].binding.version;
        Check(ci::ResolveClassProviders({earlyBad}, &callbacks).empty() && callbacks.empty(),
              "version rejection suppresses provider notification");
        earlyBad = early;
        earlyBad.types[0].methods[1].binding.function = "Missing";
        Check(ci::ResolveClassProviders({earlyBad}, &callbacks).empty() && callbacks.empty(),
              "missing function suppresses provider notification");
        Check(ci::ResolveClassProviders({early, early}, &callbacks).empty() && callbacks.empty(),
              "duplicate manifest ownership suppresses provider notification");
        early.types[0].base = basic.types[0].name;
        Check(ci::ResolveClassProviders({early, bad}, &callbacks).empty() && callbacks.empty(),
              "dependency rejection suppresses provider notification");
        std::cout << "PASS: one TOML/one DLL, registered pointers, per-function versions, atomic skip, dependency "
                     "rejection\n";
    }

    void TestPatternScan() {
        auto module = GetModuleHandleW(nullptr);
        auto base = uintptr_t(module);
        Check(HookUtils::ScanIDAPattern("4D 5A") == base, "pattern default arguments");
        Check(HookUtils::ScanIDAPattern("4D 5A", 1) == base + 1, "pattern result offset");
        auto ntOffset = reinterpret_cast<IMAGE_DOS_HEADER*>(module)->e_lfanew;
        Check(HookUtils::ScanIDAPattern("4D 5A", 0, int32_t(offsetof(IMAGE_DOS_HEADER, e_lfanew)), 1) ==
                  base + 1 + ntOffset,
              "pattern relative address");
        std::cout << "PASS: ScanIDAPattern defaults, result offset, relative address\n";
    }

    struct Fixture {
        RE::TDB header{};
        std::vector<RE::RETypeDefinition> types{5};
        std::vector<RE::RETypeImpl> impls{5};
        std::vector<ci::MethodDef> methods{1};
        std::vector<ci::MethodImpl> methodImpls{1};
        std::vector<ci::Parameter> params{1};
        std::vector<ci::FieldDef> fields{1};
        std::vector<ci::FieldImpl> fieldImpls{1};
        std::vector<char> strings{0};
        std::vector<uint8_t> bytes{0, 0, 0, 0};
        uint32_t object{};
        uint32_t integer{};
        uint32_t voidType{};

        uint32_t AddString(std::string_view value) {
            auto offset = uint32_t(strings.size());
            strings.insert(strings.end(), value.begin(), value.end());
            strings.push_back(0);
            return offset;
        }
        uint32_t AppendData(const void* value, size_t size) {
            auto offset = uint32_t(bytes.size());
            auto* first = static_cast<const uint8_t*>(value);
            bytes.insert(bytes.end(), first, first + size);
            return offset;
        }
        uint32_t AddMethod(uint32_t returnType, uint16_t argumentCount) {
            ci::ParamList list{.count = argumentCount,
                               .invokeId = uint16_t(100 + methods.size()),
                               .returnParameter = uint32_t(params.size())};
            params.push_back({.type = returnType});
            std::vector<uint32_t> arguments;
            for (uint16_t i = 0; i < argumentCount; ++i) {
                arguments.push_back(uint32_t(params.size()));
                params.push_back({.type = integer});
            }
            auto offset = AppendData(&list, sizeof(list));
            if (!arguments.empty()) {
                AppendData(arguments.data(), arguments.size() * 4);
            }
            auto index = uint32_t(methods.size());
            methods.push_back({.declaringType = object,
                               .paramsLo = offset & 0x1FFF,
                               .impl = index,
                               .paramsHi = offset >> 13,
                               .encodedOffset = 123});
            methodImpls.push_back({.virtualIndex = -1, .flags = 0x86, .name = AddString("recipe")});
            return index;
        }
        Fixture() {
            std::vector<std::string> names{"System.Object", "System.Int32", "System.Void",
                                           "System.Diagnostics.Stopwatch"};
            std::ranges::sort(names, {}, [](auto& name) { return ci::NameHash(name); });
            for (uint32_t i = 1; i < types.size(); ++i) {
                auto& name = names[i - 1];
                if (name == "System.Object") {
                    object = i;
                }
                if (name == "System.Int32") {
                    integer = i;
                }
                if (name == "System.Void") {
                    voidType = i;
                }
                auto dot = name.rfind('.');
                impls[i].nameOffset = AddString(std::string_view(name).substr(dot + 1));
                impls[i].namespaceOffset = AddString(std::string_view(name).substr(0, dot));
                impls[i].numNativeEntries = 8;
                types[i].index = i;
                types[i].implIndex = i;
                types[i].fqnHash = ci::NameHash(name);
                types[i].objectType = 1;
                types[i].typeFlags = 0x41100001;
            }
            auto ctor = AddMethod(voidType, 0);
            methodImpls[methods[ctor].impl].name = AddString(".ctor");
            types[integer].objectType = 5;
            impls[integer].fieldSize = 4;
            types[voidType].objectType = 5;
            types[object].defaultCtorIndex = ctor;
            types[object].memberMethodIndex = ctor;
            impls[object].numMemberMethods = 1;
            auto intZero = AddMethod(integer, 0);
            auto virtualIntZero = AddMethod(integer, 0);
            methodImpls[virtualIntZero].flags |= 0x40;
            // 无反射入口的同形状记录在可执行 recipe 前，验证选择过程会跳过它。
            reinterpret_cast<ci::ParamList*>(bytes.data() + methods[virtualIntZero].GetParameterOffset())->invokeId = 0;
            methodImpls[AddMethod(integer, 0)].flags |= 0x40;
            AddMethod(integer, 1);
            AddMethod(integer, 2);
            AddMethod(voidType, 1);
            AddMethod(voidType, 2);
            auto referenceOne = AddMethod(object, 1);
            auto referenceVoid = AddMethod(voidType, 1);
            auto referenceTwo = AddMethod(voidType, 2);
            methodImpls[referenceTwo].flags |= 0x40;
            for (auto index : {referenceOne, referenceVoid, referenceTwo}) {
                auto* list = reinterpret_cast<ci::ParamList*>(bytes.data() + methods[index].GetParameterOffset());
                auto* ids = reinterpret_cast<uint32_t*>(list + 1);
                for (size_t i = 0; i < list->count; ++i) {
                    params[ids[i]].type = object;
                }
            }
            uint32_t slots[4]{intZero, intZero, intZero, intZero};
            types[object].unknown2C = AppendData(slots, sizeof(slots));
            impls[object].numManagedVtable = 4;
            header = {.version = 81,
                      .numTypes = uint32_t(types.size()),
                      .typesStartOfGenericsProbably = uint32_t(types.size()),
                      .numMethods = uint32_t(methods.size()),
                      .numFields = 1,
                      .numTypeImpl = uint32_t(impls.size()),
                      .numFieldImpl = 1,
                      .numMethodImpl = uint32_t(methodImpls.size()),
                      .numParams = uint32_t(params.size()),
                      .numStringPool = uint32_t(strings.size()),
                      .numBytePool = uint32_t(bytes.size()),
                      .types = types.data(),
                      .typeImpls = impls.data(),
                      .methods = methods.data(),
                      .methodImpls = methodImpls.data(),
                      .fields = fields.data(),
                      .fieldImpls = fieldImpls.data(),
                      .params = params.data(),
                      .stringPool = strings.data(),
                      .bytePool = bytes.data()};
        }
    };

    void TestOverrideSpecialName() {
        for (auto [name, sourceFlags, expectedFlags] : {std::tuple{"get_Value", 0xDC6, 0x8C6},
                                                       std::tuple{"Accessor", 0x9C6, 0x8C6},
                                                       std::tuple{"Ordinary", 0x1C6, 0xC6}}) {
            Fixture fixture;
            auto* slots = reinterpret_cast<const uint32_t*>(fixture.bytes.data() + fixture.types[fixture.object].unknown2C);
            auto source = slots[0];
            fixture.methodImpls[source].flags = uint16_t(sourceFlags);
            fixture.methodImpls[source].name = fixture.AddString(name);
            fixture.header.numStringPool = fixture.strings.size();
            fixture.header.stringPool = fixture.strings.data();
            ci::MethodSpec method{.name = name, .kind = ci::MethodKind::Override, .returnType = "System.Int32"};
            std::vector<ci::CustomTypeConfig> specs{{.name = "Tests.AccessorBase", .methods = {method}},
                                             {.name = "Tests.AccessorDerived", .base = "Tests.AccessorBase", .methods = {method}}};
            auto database = ci::BuildTypeDatabase(fixture.header, specs);
            for (auto& type : database->customTypes) {
                auto* override = database->FindMethod(type.virtualMethods[0]);
                Check(override != nullptr, "accessor override found");
                auto& impl = database->methodImpls[database->methods[override->index].impl];
                Check(impl.virtualIndex == 0, "accessor override retains inherited slot");
                Check(impl.flags == expectedFlags, "override preserves only source SpecialName, not Abstract/NewSlot");
                Check(type.virtualMethods.size() == 4 && std::ranges::all_of(type.virtualMethods, [&](auto id) { return id == override->index; }),
                      "accessor override replaces inherited slot aliases without adding slots");
            }
            Check(database->methodImpls[source].flags == sourceFlags, "override leaves source flags intact");
            fixture.methodImpls[source].flags |= 0x20;
            Rejects([&] { ci::BuildTypeDatabase(fixture.header, specs); });
        }
        std::cout << "PASS: SpecialName inheritance across two overrides, ordinary method flags, alias slots and final rejection\n";
    }

    void TestOriginalAccessorSpecialName(const RE::TDB& header) {
        ci::MethodSpec method{.name = "get_IdleActionNo", .kind = ci::MethodKind::Override, .returnType = "ace.ACTION_ID"};
        std::vector<ci::CustomTypeConfig> specs{{.name = "Tests.EnemyExtendAccessor", .base = "app.cEnemyExtendBase", .methods = {method}, .abstract = true}};
        auto database = ci::BuildTypeDatabase(header, specs);
        auto* type = database->FindType(specs[0].name);
        const auto* definition = database->GetMethodDef(&database->types[type->index], method.name);
        auto* override = definition ? database->FindMethod(uint32_t(definition - database->methods.data())) : nullptr;
        Check(override != nullptr, "real getter override found");
        auto& impl = database->methodImpls[database->methods[override->index].impl];
        Check(impl.virtualIndex >= 0, "real getter has a virtual slot");
        auto& parent = database->types[type->parent];
        auto* slots = reinterpret_cast<const uint32_t*>(database->bytes.data() + parent.unknown2C);
        auto& sourceImpl = database->methodImpls[database->methods[slots[impl.virtualIndex]].impl];
        Check(sourceImpl.flags == 0xDC6 && impl.flags == 0x8C6 && impl.virtualIndex == sourceImpl.virtualIndex &&
                  type->virtualMethods[impl.virtualIndex] == override->index &&
                  type->virtualMethods.size() == database->typeImpls[parent.implIndex].numManagedVtable,
              "real getter preserves SpecialName and original slot without inheriting Abstract/NewSlot");
        std::cout << "PASS: real TDB getter override DC6 -> 8C6, original slot and source flags preserved\n";
    }

    // 合成内嵌引用值类型，验证布局搬迁后两张位图按字段位置平移；不是游戏数据采样。
    static void TestEmbeddedReferenceBitmaps() {
        Fixture fixture;
        const auto payloadIndex = uint32_t(fixture.types.size());
        auto payload = fixture.types[fixture.integer];
        auto payloadImpl = fixture.impls[payload.implIndex];
        payload.index = payloadIndex;
        payload.implIndex = fixture.impls.size();
        payloadImpl.nameOffset = fixture.AddString("ReferencePayload");
        payloadImpl.namespaceOffset = fixture.AddString("Tests");
        payloadImpl.fieldSize = 16;
        const uint64_t payloadReferences = 3; // two references; only the second participates in cycle traversal
        const uint64_t payloadCycles = 2;
        std::memcpy(reinterpret_cast<uint8_t*>(&payloadImpl) + 0x20, &payloadReferences, 8);
        std::memcpy(reinterpret_cast<uint8_t*>(&payloadImpl) + 0x28, &payloadCycles, 8);
        fixture.types.push_back(payload);
        fixture.impls.push_back(payloadImpl);
        fixture.header.types = fixture.types.data();
        fixture.header.typeImpls = fixture.impls.data();
        fixture.header.numTypes = uint32_t(fixture.types.size());
        fixture.header.numTypeImpl = uint32_t(fixture.impls.size());
        fixture.header.stringPool = fixture.strings.data();
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        ci::CustomTypeConfig base{.name = "Tests.ReferencePrefix", .fields = {{"Prefix", "System.Object"}}};
        ci::CustomTypeConfig derived{.name = "Tests.EmbeddedReferences", .base = base.name,
                            .fields = {{"Payload", "Tests.ReferencePayload"}, {"Tail", "System.Object"}}};
        auto database = ci::BuildTypeDatabase(fixture.header, {base, derived});
        const auto* type = QueryType(database, derived.name);
        const auto* field = database->GetFieldDef(type, "Payload");
        const auto* tail = database->GetFieldDef(type, "Tail");
        const auto& impl = database->typeImpls[type->implIndex];
        Check(field && tail && database->fieldImpls[field->impl].offset == 16 &&
                  database->fieldImpls[tail->impl].offset == 32 && impl.fieldSize == 40,
              "embedded reference payload uses aligned field storage after the inherited prefix");
        uint64_t references{}, cycles{};
        std::memcpy(&references, reinterpret_cast<const uint8_t*>(&impl) + 0x20, 8);
        std::memcpy(&cycles, reinterpret_cast<const uint8_t*>(&impl) + 0x28, 8);
        Check(references == 29 && cycles == 25,
              "embedded reference/cycle maps shift by the field offset, preserve inherited slots and exclude padding");
        std::cout << "PASS: embedded value reference/cycle bitmaps, inherited prefix, alignment padding and following reference\n";
    }

    static void TestTrampolineOwnership() {
        BranchTrampoline first;
        BranchTrampoline second;
        Check(first.Create(64) && second.Create(64), "near code allocations");
        auto* firstCode = first.Allocate(16);
        auto* secondCode = second.Allocate(16);
        MEMORY_BASIC_INFORMATION firstPage{}, secondPage{};
        Check(VirtualQuery(firstCode, &firstPage, sizeof(firstPage)) &&
                  VirtualQuery(secondCode, &secondPage, sizeof(secondPage)) &&
                  firstPage.AllocationBase == secondPage.AllocationBase,
              "trampoline fixture shares an allocator page");
        uint8_t code[]{0xB8, 42, 0, 0, 0, 0xC3};
        HookUtils::SafeWriteBuf(uintptr_t(secondCode), code, sizeof(code));
        FlushInstructionCache(GetCurrentProcess(), secondCode, sizeof(code));
        first.Destroy();
        Check(VirtualQuery(secondCode, &secondPage, sizeof(secondPage)) && secondPage.State == MEM_COMMIT,
              "destroying one trampoline must preserve other allocations in its page");
        Check(first.Remain() == 0 && reinterpret_cast<int (*)()>(secondCode)() == 42,
              "destroy resets local storage without invalidating a neighboring entry");
        Check(first.Create(64), "destroyed trampoline can acquire new storage");
        std::cout << "PASS: trampoline suballocation ownership, neighboring code and explicit destroy/recreate\n";
    }

    static void TestMethodCodeOwnership() {
        Fixture fixture;
        auto value = +[](void* vmContext, void* object) -> int32_t {
            return 42;
        };
        ci::CustomTypeConfig declaration{
            .name = "Tests.OwnedEntries",
            .methods = {{.name = "First", .returnType = "System.Int32",
                         .binding = {.backend = ci::Backend::Cpp, .address = reinterpret_cast<void*>(value)}},
                        {.name = "Second", .returnType = "System.Int32",
                         .binding = {.backend = ci::Backend::Cpp, .address = reinterpret_cast<void*>(value)}}}};
        auto first = ci::BuildTypeDatabase(fixture.header, {declaration});
        auto second = ci::BuildTypeDatabase(fixture.header, {declaration});
        first->encodingBase = second->encodingBase = uintptr_t(GetModuleHandleW(nullptr));
        // Force both matching signatures through the generator, independent of native recipe availability.
        for (const auto& method : second->customMethods) {
            second->invokeSignatures[method.index - second->originalMethodCount] = ci::ClassifyInvokeSignature(*second, second->methodDeclarations[method.index - second->originalMethodCount]);
        }
        ci::CreateMethodTrampolines(*first);
        ci::CreateMethodTrampolines(*second);
        Check(!first->methodTrampolines.empty() && !second->methodTrampolines.empty(), "each database owns its near entries");
        Check(first->methodDeclarations.capacity() == 0 && second->methodDeclarations.capacity() == 0,
              "entry generation releases declarations and their storage");
        first.reset();
        auto call = reinterpret_cast<int32_t (*)(void*, void*)>(second->customMethods[0].entry);
        Check(call(nullptr, nullptr) == 42, "destroying an unpublished database preserves another database's code");

        declaration.methods[1].binding.address = nullptr;
        auto failed = ci::BuildTypeDatabase(fixture.header, {declaration});
        failed->encodingBase = second->encodingBase;
        Rejects([&] { ci::CreateMethodTrampolines(*failed); });
        Check(!failed->methodTrampolines.empty() && failed->methodDeclarations.capacity() == 0,
              "binding failure releases declarations after partial entry generation");
        failed.reset();
        Check(call(nullptr, nullptr) == 42, "partial build cleanup preserves unrelated entries");

        ci::PrepareInvokeAdapters(*second);
        Check(second->invokeSignatures.capacity() == 0 && second->invokeAdapterCodes.size() == 1 &&
                  second->customMethods[0].invokeAdapter == second->customMethods[1].invokeAdapter,
              "build signatures are released while identical methods share one adapter");
        ci::InvokeFrame frame{.method = &second->methods[second->customMethods[0].index]};
        second->customMethods[0].invokeAdapter(&frame, nullptr);
        Check(uintptr_t(frame.result) == 42, "adapter remains callable after build signatures are released");
        const auto adapter = DWORD64(second->customMethods[0].invokeAdapter);
        DWORD64 imageBase{};
        Check(RtlLookupFunctionEntry(adapter, &imageBase, nullptr) != nullptr, "owned adapter has registered unwind data");
        second.reset();
        Check(RtlLookupFunctionEntry(adapter, &imageBase, nullptr) == nullptr, "database destruction unregisters adapter unwind data");
        std::cout << "PASS: database-owned entries, partial build cleanup, transient signatures, adapter sharing and unwind lifetime\n";
    }

    static void TestClassVirtualTable() {
        Fixture fixture;
        ci::MethodSpec method{.name = "Value", .kind = ci::MethodKind::Virtual, .returnType = "System.Int32"};
        ci::CustomTypeConfig base{.name = "Tests.VirtualBase", .methods = {method}};
        method.kind = ci::MethodKind::Override;
        ci::CustomTypeConfig child{.name = "Tests.VirtualChild", .base = base.name, .methods = {method}};
        auto database = ci::BuildTypeDatabase(fixture.header, {base, child});
        auto* baseType = database->FindType(base.name);
        auto* childType = database->FindType(child.name);
        auto inherited = +[]() {};
        auto replacement = +[]() {};
        std::array<void*, 4> parentSlots{};
        parentSlots.fill(reinterpret_cast<void*>(inherited));
        std::array<void*, 5> baseSlots{}, childSlots{};
        std::array<void*, 3> parentInfo{parentSlots.data(), nullptr, &database->types[fixture.object]};
        std::array<void*, 3> baseInfo{baseSlots.data(), nullptr, &database->types[baseType->index]};
        std::array<void*, 3> childInfo{childSlots.data(), nullptr, &database->types[childType->index]};
        database->types[fixture.object].objectInfo = parentInfo.data() + 2;
        database->types[baseType->index].objectInfo = baseInfo.data() + 2;
        database->types[childType->index].objectInfo = childInfo.data() + 2;
        database->FindMethod(baseType->virtualMethods[4])->entry = reinterpret_cast<void*>(inherited);
        database->FindMethod(childType->virtualMethods[4])->entry = reinterpret_cast<void*>(replacement);
        const auto* active = ci::g_database.get();
        ci::UpdateVirtualTable(*database, *baseType);
        ci::UpdateVirtualTable(*database, *childType);
        Check(baseSlots[0] == parentSlots[0] && childSlots[0] == parentSlots[0] &&
                  baseSlots[4] == reinterpret_cast<void*>(inherited) && childSlots[4] == reinterpret_cast<void*>(replacement),
              "single-type update inherits native slots and writes the selected override");
        parentSlots[0] = reinterpret_cast<void*>(replacement);
        ci::UpdateVirtualTable(*database, *baseType);
        ci::UpdateVirtualTable(*database, *childType);
        Check(childSlots[0] == parentSlots[0], "later native entry installation is reflected by the next update");
        database->types[baseType->index].objectInfo = nullptr;
        ci::UpdateVirtualTable(*database, *baseType);
        ci::UpdateVirtualTable(*database, *childType);
        Check(childSlots[0] == parentSlots[0] && childSlots[4] == reinterpret_cast<void*>(replacement) &&
                  ci::g_database.get() == active, "abstract parent traversal uses only the explicitly supplied database");
        std::cout << "PASS: single-class VirtualTable updates, inherited native entries, overrides and abstract parents\n";
    }

    void TestRuntimeInitializationCallHook() {
        static int calls;
        // 替代生产安装入口的初始化；先存计数，再让真实 CALL 安装函数消费同一个池。
        Check(g_branchTrampoline.Create(900), "shared startup trampoline allocation");
        auto* originalCount = static_cast<uint32_t*>(g_branchTrampoline.Allocate(sizeof(uint32_t)));
        *originalCount = 123;
        auto remainingBeforeCall = g_branchTrampoline.Remain();
        auto native = +[]() { ++calls; };
        std::array<uint8_t, 32> originalBody{};
        std::memcpy(originalBody.data(), reinterpret_cast<void*>(native), originalBody.size());
        // 安装后的 CALL 仍指向生产 bridge；测试 caller 保留到进程结束。
        static BranchTrampoline page;
        Check(page.Create(64), "test caller code allocation");
        auto* caller = static_cast<uint8_t*>(page.Allocate(32));
        std::array<uint8_t, 14> code{0x48, 0x83, 0xEC, 0x28, 0xE8, 0, 0, 0, 0,
                                     0x48, 0x83, 0xC4, 0x28, 0xC3};
        auto call = uintptr_t(caller) + 4;
        auto delta = int64_t(uintptr_t(native)) - int64_t(call + 5);
        Check(delta >= INT32_MIN && delta <= INT32_MAX, "test native callee rel32 range");
        auto relative = int32_t(delta);
        std::memcpy(code.data() + 5, &relative, sizeof(relative));
        HookUtils::SafeWriteBuf(uintptr_t(caller), code.data(), code.size());
        Check(!ci::InstallRuntimeInitializationCall(0, native) &&
                  !ci::InstallRuntimeInitializationCall(call + 1, native) &&
                  !ci::InstallRuntimeInitializationCall(call, nullptr), "invalid call-site hook gates");
        Check(ci::InstallRuntimeInitializationCall(call, native) &&
                  HookUtils::ReadOffsetData(call, 1, 5) != uintptr_t(native), "real caller CALL bridge installed");
        Check(g_branchTrampoline.Remain() == remainingBeforeCall - 14 && *originalCount == 123,
              "CALL bridge consumes shared pool without replacing or overwriting count storage");
        Check(std::memcmp(originalBody.data(), reinterpret_cast<void*>(native), originalBody.size()) == 0,
              "native initialization entry/body remain intact for RF invoke-table disassembly");
        reinterpret_cast<void (*)()>(caller)();
        Check(calls == 1 && !ci::g_ready && *originalCount == 123 &&
                  std::memcmp(originalBody.data(), reinterpret_cast<void*>(native), originalBody.size()) == 0,
              "CALL bridge preserves native execution and fail-closed lifecycle without patching callee");
        std::cout << "PASS: shared count/CALL storage, actual native execution, intact callee entry/body for RF discovery\n";
    }

    void TestRuntimeReadyNotification() {
        ci::g_ready = false;
        ci::g_database.reset();
        static int calls;
        static void* observedConstructor;
        static bool observedReady;
        calls = 0;
        observedConstructor = nullptr;
        observedReady = false;
        std::vector<ci::ClassProviderNotification> callbacks{{[](const ci::ProviderContext* context) noexcept {
            ++calls;
            observedReady = ci::g_ready.load();
            observedConstructor = QueryMethod(context, QueryType(context, "System.Object"), ".ctor()");
        }}};
        Check(!ci::FinalizeRuntimeTypes() && !ci::CompleteRuntimeInitialization(callbacks) &&
                  callbacks.size() == 1 && !calls && !ci::g_ready,
              "unpublished metadata cannot finalize or notify");
        Fixture fixture;
        auto ctor = uint32_t(fixture.types[fixture.object].defaultCtorIndex);
        fixture.methodImpls[fixture.methods[ctor].impl].name = fixture.AddString(".ctor");
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        fixture.header.stringPool = fixture.strings.data();
        fixture.types[fixture.object].memberMethodIndex = ctor;
        fixture.impls[fixture.object].numMemberMethods = 1;
        auto specs = ci::ParseCustomTypeConfig(R"toml(
schema_version = 1
[[types]]
name = "Tests.ReadyChild"
base = "System.Object"
)toml", "ready.toml");
        ci::g_database = ci::BuildTypeDatabase(fixture.header, specs);
        auto& database = *ci::g_database;
        database.encodingBase = 0x10000000;
        database.extensions.push_back({fixture.object, 0});
        database.methods[ctor].encodedOffset = 0;
        Check(!ci::FinalizeRuntimeTypes() && !ci::CompleteRuntimeInitialization(callbacks) &&
                  !database.runtimeTypesReady && !ci::g_ready && !calls && callbacks.size() == 1 && database.extensions.size() == 1,
              "failed runtime shape validation leaves notification pending");

        auto& parent = database.types[fixture.object];
        auto& child = database.types[database.customTypes.front().index];
        std::array<void*, 4> parentSlots{}, childSlots{};
        std::array<void*, 3> parentInfo{parentSlots.data(), reinterpret_cast<void*>(0x10), &parent};
        std::array<void*, 3> childInfo{childSlots.data(), nullptr, &child};
        parent.typeInfo = &parentInfo;
        parent.instanceSize = 0x10 + database.typeImpls[parent.implIndex].fieldSize;
        child.typeInfo = &childInfo;
        parent.objectInfo = parentInfo.data() + 2;
        child.objectInfo = childInfo.data() + 2;
        Check(ci::FinalizeRuntimeTypes() && database.runtimeTypesReady && !ci::g_ready && !calls &&
                  callbacks.size() == 1 && database.extensions.capacity() == 0,
              "builder completion releases extension checks without publishing ready or notifying providers");
        auto& context = ci::GetContext();
        auto* type = QueryType(context, "System.Object");
        Check(!QueryMethod(context, type, ".ctor()") && database.methodQueries[type].at(".ctor()"),
              "pending entry caches its declaration rather than a permanent null declaration");

        // 模拟外层原生注册：更新同一 MethodDef 及父类虚表；不重新建表或清空查询缓存。
        database.methods[ctor].encodedOffset = 0x1234;
        auto expected = reinterpret_cast<void*>(database.encodingBase + 0x1234);
        parentSlots[0] = expected;
        Check(!childSlots[0] && ci::CompleteRuntimeInitialization(callbacks),
              "native registration completion resynchronizes inherited slots");
        Check(ci::g_ready && observedReady && calls == 1 && observedConstructor == expected &&
                  childSlots[0] == expected && callbacks.empty(),
              "provider sees installed native ctor, inherited slot and public ready before RF SDK");
        Check(ci::CompleteRuntimeInitialization(callbacks) && calls == 1,
              "completion and provider notification are one-shot");
        ci::g_ready = false;
        ci::g_database.reset();
        std::cout << "PASS: actual Engine runtime phases, failed shape gate, pending native ctor, live cached entry, "
                     "inherited slot resync and one-shot ready notification before RF SDK\n";
    }

    // 先于 API::initialize 运行，确保 ProviderContext 查询没有借用 RF SDK 的隐式初始化。
    void TestQueries() {
        auto& context = ci::GetContext();
        Check(!QueryType(context, "System.Object") && !QueryMethod(context, nullptr, "recipe()"),
              "queries before metadata return null");
        Fixture fixture;
        fixture.types[fixture.object].memberMethodIndex = 1;
        fixture.impls[fixture.object].numMemberMethods = uint16_t(fixture.methods.size() - 1);
        auto specs = ci::ParseCustomTypeConfig(R"toml(
schema_version = 1
[[types]]
name = "Tests.QueryBase"
[[types.methods]]
name = "Pick"
return_type = "System.Int32"
binding = {function = "QueryProbe", version = 1}
[[types.methods]]
name = "Pick"
return_type = "System.Int32"
parameters = [{name = "value", type = "System.Int32"}]
binding = {function = "QueryProbe", version = 1}
[[types.methods]]
name = "Pick"
return_type = "System.Int32"
parameters = [{name = "a", type = "System.Int32"}, {name = "b", type = "System.Int32"}]
binding = {function = "QueryProbe", version = 1}
[[types.methods]]
name = "Evaluate"
kind = "virtual"
return_type = "System.Int32"
binding = {function = "QueryProbe", version = 1}
[[types]]
name = "Tests.QueryDerived"
base = "Tests.QueryBase"
[[types.methods]]
name = "Evaluate"
kind = "override"
return_type = "System.Int32"
binding = {function = "QueryProbe", version = 1}
[[types.methods]]
name = "Pick"
return_type = "System.Int32"
parameters = [{name = "value", type = "System.Int32"}]
binding = {function = "QueryProbe", version = 1}
)toml", "query.toml");
        ci::g_database = ci::BuildTypeDatabase(fixture.header, specs);
        auto& database = *ci::g_database;
        database.encodingBase = 0x10000000;
        for (auto& method : database.customMethods) {
            database.methods[method.index].encodedOffset = int32_t(0x100 + method.index * 16);
        }
        auto* original = QueryType(context, "System.Object");
        auto* base = QueryType(context, "Tests.QueryBase");
        auto* derived = QueryType(context, "Tests.QueryDerived");
        Check(original == &database.types[fixture.object] && base && derived, "original/custom type handles");
        Check(!QueryType(context, " Tests.QueryBase ") && !QueryType(context, "tests.QueryBase") &&
                  !QueryType(context, "Tests.Missing"),
              "type names match RF exactly");
        auto* pick = QueryMethod(context, base, "Pick()");
        auto* pickOne = QueryMethod(context, base, "Pick(System.Int32)");
        Check(pick && pickOne && pick != pickOne, "exact overload signatures");
        Check(QueryMethod(context, base, "Pick") == pick && QueryMethod(context, derived, "Pick()") == pick,
              "bare name takes first overload and full signature searches parents");
        Check(QueryMethod(context, derived, "Pick") == QueryMethod(context, derived, "Pick(System.Int32)") &&
                  QueryMethod(context, derived, "Pick") != pickOne,
              "bare lookup prefers current declaring type");
        Check(QueryMethod(context, base, "Pick(System.Int32, System.Int32)") &&
                  !QueryMethod(context, base, "Pick(System.Int32,System.Int32)") &&
                  !QueryMethod(context, base, " Pick( System.Int32 ) "),
              "RF comma-space and exact whitespace matching");
        Check(QueryMethod(context, derived, "Evaluate()") != QueryMethod(context, base, "Evaluate()"),
              "nearest override declaration");
        const auto* overrideMethod = database.GetMethodDef(derived, "Evaluate()");
        database.methods[overrideMethod - database.methods.data()].encodedOffset = 0;
        Check(!QueryMethod(context, derived, "Evaluate()"), "abstract/missing override does not fall back to parent");
        Check(!QueryMethod(context, base, "Missing") &&
                  !QueryMethod(context, base, "Pick(System.Int32,System.Int32)") && !QueryMethod(context, base, "") &&
                  !QueryMethod(context, base, "Pick("),
              "missing/malformed queries return null");
        auto cacheCount = database.methodQueries[base].size();
        Check(QueryMethod(context, base, "Pick") == pick && !QueryMethod(context, base, "Missing") &&
                  database.methodQueries[base].size() == cacheCount && !database.methodQueries[base].at("Missing"),
              "successful and missing declarations are cached");
        std::atomic_bool parallelOk{true};
        std::array<std::thread, 4> threads;
        for (auto& thread : threads) {
            thread = std::thread([&] {
                for (int i = 0; i < 1000; ++i) {
                    if (QueryMethod(context, base, "Pick") != pick || QueryMethod(context, base, "Missing")) {
                        parallelOk = false;
                    }
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        Check(parallelOk, "parallel RF-style cache queries");
        auto originalEntry = reinterpret_cast<void*>(database.encodingBase + 123);
        Check(QueryMethod(context, derived, "recipe(System.Int32)") == originalEntry, "inherited original AOT method");
        // 原参数 ID 高位带编码标志，发布后总数可能跨越 bitmask 边界。
        for (uint32_t i = 1; i < database.originalMethodCount; ++i) {
            auto* list = reinterpret_cast<ci::ParamList*>(database.bytes.data() + database.methods[i].GetParameterOffset());
            auto* ids = reinterpret_cast<uint32_t*>(list + 1);
            for (uint32_t j = 0; j < list->count; ++j) {
                ids[j] |= database.originalParameterMask + 1;
            }
        }
        database.parameters.resize((database.originalParameterMask + 1) * 2);
        database.Publish(fixture.header);
        Check(QueryMethod(context, original, "recipe(System.Int32)") == originalEntry,
              "original parameter mask remains fixed after publish");
        const auto* method = database.GetMethodDef(base, "Pick(System.Int32)");
        auto* list = reinterpret_cast<ci::ParamList*>(database.bytes.data() + method->GetParameterOffset());
        auto id = *reinterpret_cast<uint32_t*>(list + 1);
        database.parameters[id].modifier = 2;
        Check(QueryMethod(context, base, "Pick(System.Int32)") == pickOne &&
                  !QueryMethod(context, base, "Pick(System.Int32&)"),
              "RF out lookup uses the element type without ampersand");
        database.parameters[id].modifier = 1;
        Check(QueryMethod(context, base, "Pick(System.Int32)") == pickOne &&
                  !QueryMethod(context, base, "Pick(System.Int32*)"),
              "RF pointer lookup also uses the element type");
        database.methods[method - database.methods.data()].encodedOffset = 0;
        Check(!QueryMethod(context, base, "Pick(System.Int32)"), "cached declaration reads its current entry");
        static int calls;
        static const ci::ProviderContext* received;
        std::vector<ci::ClassProviderNotification> callbacks{{[](const ci::ProviderContext* value) noexcept {
            ++calls;
            received = value;
        }}};
        ci::NotifyClassProviders(callbacks, ci::GetContext());
        ci::NotifyClassProviders(callbacks, ci::GetContext());
        Check(calls == 1 && received == &context && QueryType(received, "Tests.QueryBase") == base,
              "stable context and one-shot notification");
        ci::g_database.reset();
        Fixture genericFixture;
        genericFixture.types[genericFixture.object].memberMethodIndex = 1;
        genericFixture.impls[genericFixture.object].numMemberMethods = 2;
        auto genericRegistry = ci::BuildTypeDatabase(genericFixture.header, {});
        auto& impl = genericRegistry->typeImpls[genericRegistry->types[genericFixture.integer].implIndex];
        impl.nameOffset = int32_t(genericRegistry->strings.size());
        genericRegistry->strings.insert(genericRegistry->strings.end(), {'!', '0', '\0'});
        auto ReturnParameter = [&](uint32_t index) -> ci::Parameter& {
            auto* list = reinterpret_cast<const ci::ParamList*>(genericRegistry->bytes.data() +
                                                               genericRegistry->methods[index].GetParameterOffset());
            return genericRegistry->parameters[list->returnParameter];
        };
        ReturnParameter(1).type = genericFixture.integer;
        ReturnParameter(2).type = genericFixture.voidType;
        genericRegistry->methods[2].encodedOffset = 124;
        genericRegistry->encodingBase = 0x10000000;
        auto* genericType = &genericRegistry->types[genericFixture.object];
        auto expected = reinterpret_cast<void*>(genericRegistry->encodingBase + 124);
        Check(QueryMethod(genericRegistry, genericType, "recipe") == expected &&
                  QueryMethod(genericRegistry, genericType, "recipe()") == expected,
              "RF skips generic definitions in both passes");
        std::cout << "PASS: early queries before RF SDK, RF bare/full signatures, first overload, inheritance, exact "
                     "whitespace, modifier semantics, generic skip, positive/negative cache, parallel queries, "
                     "published parameter mask, live entry and one-shot context\n";
    }

    void TestSerializedFields() {
        auto parsed = ci::ParseProviderConfig(R"(
schema_version = 1
[plugin]
dll = "Test_ci.dll"
[[types]]
name = "Tests.Serialized"
fields = [
    { name = "Value", type = "System.Int32", serialize = true },
    { name = "RuntimeOnly", type = "System.Int32" }
]
)", "serialized.toml");
        Check(parsed.types[0].fields[0].serialize && !parsed.types[0].fields[1].serialize,
              "explicit RSZ declaration, runtime-only by default");
        Fixture fixture;
        auto database = ci::BuildTypeDatabase(fixture.header, parsed.types);
        auto first = uint32_t(database->types[database->customTypes[0].index].memberFieldIndex);
        Check(database->fieldImpls[database->fields[first].impl].flags == 0x4006 &&
                  database->fieldImpls[database->fields[first + 1].impl].flags == 6,
              "engine RSZ flag is independent from field visibility/layout");
        parsed.types[0].name = "System.Diagnostics.Stopwatch";
        parsed.types[0].extend = true;
        for (auto& type : fixture.types) {
            if (type.fqnHash == ci::NameHash(parsed.types[0].name)) {
                type.typeFlags |= 0x100;
                type.parentTypeIndex = fixture.object;
            }
        }
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, parsed.types); });
        std::cout << "PASS: serialized field declaration/engine flag, runtime-only default, unchanged original RSZ schema\n";
    }

    int32_t OutIdProbe(void* context, void* self, int32_t value, int32_t* output) noexcept {
        g_ctorContext = context;
        g_ctorSelf = self;
        *output = value >= 0 ? value + 1000 : 0;
        return value >= 0;
    }
    int32_t OutNumberProbe(void*, void*, int32_t* output) noexcept {
        *output = 99;
        return 1;
    }
    void TestOutParameters() {
        Fixture fixture;
        auto AddOutRecipe = [&](uint16_t count, bool isVirtual, uint32_t flags = 2) {
            auto index = fixture.AddMethod(fixture.integer, count);
            auto* list = reinterpret_cast<ci::ParamList*>(fixture.bytes.data() +
                                                        fixture.methods[index].GetParameterOffset());
            auto* ids = reinterpret_cast<uint32_t*>(list + 1);
            fixture.params[ids[count - 1]].modifier = 2;
            fixture.params[ids[count - 1]].flags = flags;
            if (isVirtual) {
                fixture.methodImpls[index].flags |= 0x40;
            }
            return index;
        };
        AddOutRecipe(2, false, 0); // ref 与 out 的 ABI 都是指针，但声明/recipe 不互相替代。
        auto byRefReturn = AddOutRecipe(2, false);
        auto* byRefList = reinterpret_cast<ci::ParamList*>(fixture.bytes.data() +
                                                         fixture.methods[byRefReturn].GetParameterOffset());
        fixture.params[byRefList->returnParameter].modifier = 2;
        auto staticRecipe = AddOutRecipe(2, false);
        fixture.methodImpls[staticRecipe].flags |= 0x10;
        auto outRecipe = AddOutRecipe(2, false);
        auto virtualRecipe = AddOutRecipe(1, true);
        fixture.methodImpls[virtualRecipe].name = fixture.AddString("TryGetNumber");
        auto interfaceIndex = uint32_t(fixture.types.size());
        fixture.types.push_back({.index = interfaceIndex,
                                 .implIndex = interfaceIndex,
                                 .typeFlags = 0x20,
                                 .unknown2C = fixture.AppendData(&virtualRecipe, sizeof(virtualRecipe)),
                                 .objectType = 1,
                                 .interfaceIndex = 0});
        fixture.impls.push_back({.nameOffset = int32_t(fixture.AddString("IOut")),
                                 .namespaceOffset = int32_t(fixture.AddString("Tests")),
                                 .numManagedVtable = 1});
        fixture.header.numTypes = uint32_t(fixture.types.size());
        fixture.header.typesStartOfGenericsProbably = fixture.header.numTypes;
        fixture.header.types = fixture.types.data();
        fixture.header.numTypeImpl = uint32_t(fixture.impls.size());
        fixture.header.typeImpls = fixture.impls.data();
        fixture.header.numMethods = uint32_t(fixture.methods.size());
        fixture.header.methods = fixture.methods.data();
        fixture.header.numMethodImpl = uint32_t(fixture.methodImpls.size());
        fixture.header.methodImpls = fixture.methodImpls.data();
        fixture.header.numParams = uint32_t(fixture.params.size());
        fixture.header.params = fixture.params.data();
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        fixture.header.stringPool = fixture.strings.data();
        fixture.header.numBytePool = uint32_t(fixture.bytes.size());
        fixture.header.bytePool = fixture.bytes.data();
        constexpr auto declaration = R"toml(schema_version = 1
[[types]]
name = "Tests.OutBase"
interfaces = ["Tests.IOut"]
[[types.methods]]
name = "TryGetId"
return_type = "System.Int32"
parameters = [{ name = "value", type = "System.Int32" }, { name = "id", type = "System.Int32", modifier = "out" }]
binding = { function = "OutIdProbe", version = 1 }
[[types.methods]]
name = "TryGetNumber"
kind = "virtual"
return_type = "System.Int32"
parameters = [{ name = "number", type = "System.Int32", modifier = "out" }]
binding = { function = "OutNumberProbe", version = 1 }
[[types]]
name = "Tests.OutDerived"
base = "Tests.OutBase"
[[types.methods]]
name = "TryGetNumber"
kind = "override"
return_type = "System.Int32"
parameters = [{ name = "number", type = "System.Int32", modifier = "out" }]
binding = { function = "OutNumberProbe", version = 1 }
)toml";
        auto specs = ci::ParseCustomTypeConfig(declaration, "out.toml");
        Check(specs[0].methods[1].parameters[0].modifier == ci::ParameterModifier::Value &&
                  specs[0].methods[1].parameters[1].modifier == ci::ParameterModifier::Out,
              "out parsed separately from parameter name/type");
        for (auto& type : specs) {
            for (auto& method : type.methods) {
                if (method.kind != ci::MethodKind::Constructor) {
                    method.binding.address = method.name == "TryGetId" ? reinterpret_cast<void*>(&OutIdProbe)
                                                                     : reinterpret_cast<void*>(&OutNumberProbe);
                }
            }
        }
        auto database = ci::BuildTypeDatabase(fixture.header, specs);
        database->encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        ci::CreateMethodTrampolines(*database);
        ci::PrepareInvokeAdapters(*database);
        auto& method = database->customMethods[1];
        const auto decoded = ci::ReadMethodDeclaration(*database, method.index);
        Check(database->methodDeclarations.capacity() == 0 && decoded.name == "TryGetId" &&
                  decoded.kind == ci::MethodKind::Instance && decoded.returnType == "System.Int32" &&
                  decoded.parameters.size() == 2 && decoded.parameters[0].name == "value" &&
                  decoded.parameters[0].modifier == ci::ParameterModifier::Value &&
                  decoded.parameters[1].name == "id" && decoded.parameters[1].modifier == ci::ParameterModifier::Out,
              "method declaration decoder reads final names, return and out direction after authoring data is released");
        auto* list = reinterpret_cast<const ci::ParamList*>(database->bytes.data() +
                                                          database->methods[method.index].GetParameterOffset());
        auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
        Check(list->invokeId == 100 + outRecipe, "skip by-value/ref/static/byref-return recipes");
        auto& argument = database->parameters[ids[1]];
        Check(argument.type == fixture.integer && argument.modifier == 2 && argument.flags == 2 &&
                  std::string_view(database->strings.data() + argument.name) == "id",
              "out metadata keeps the element type and parameter name");
        std::array<uint32_t, 3> buffer{0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC};
        uint64_t context{}, self{};
        auto call = reinterpret_cast<int32_t (*)(void*, void*, int32_t, int32_t*)>(method.entry);
        Check(call(&context, &self, 23, reinterpret_cast<int32_t*>(&buffer[1])) == 1 && buffer[1] == 1023 &&
                  buffer.front() == 0xCCCCCCCC && buffer.back() == 0xCCCCCCCC &&
                  g_ctorContext == &context && g_ctorSelf == &self,
              "native out writes through caller storage, vmctx/this and guards preserved");
        Check(call(&context, &self, -1, reinterpret_cast<int32_t*>(&buffer[1])) == 0 && buffer[1] == 0,
              "out also writes on false result");
        auto slot = database->methodImpls[database->methods[database->customMethods[2].index].impl].virtualIndex;
        auto* derived = database->FindMethod(database->customTypes[1].virtualMethods[slot]);
        Check(database->methodImpls[database->methods[derived->index].impl].virtualIndex == slot &&
                  reinterpret_cast<int32_t (*)(void*, void*, int32_t*)>(derived->entry)(
                      &context, &self, reinterpret_cast<int32_t*>(&buffer[1])) == 1 && buffer[1] == 99,
              "out virtual override retains the inherited slot");
        auto& baseDefinition = database->types[database->customTypes[0].index];
        auto* interfaceList = reinterpret_cast<const RE::REInterfaceList*>(
            database->bytes.data() + baseDefinition.getInterfaceListOffset());
        auto interfaceSlot = interfaceList->encodedTypes[0] >> 19;
        Check((interfaceList->encodedTypes[0] & 0x7FFFF) == interfaceIndex &&
                  database->customTypes[0].virtualMethods[interfaceSlot] == database->customMethods[2].index &&
                  database->customTypes[1].virtualMethods[interfaceSlot] == derived->index,
              "out interface signature and inherited override alias");
        auto broken = specs;
        broken[1].methods[1].parameters[0].modifier = ci::ParameterModifier::Value;
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, broken); });
        broken = specs;
        broken[0].methods[1].parameters[1].type = "System.Void";
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, broken); });
        for (auto modifier : {"in", "pointer", "invalid"}) {
            auto text = std::string(declaration);
            text.replace(text.find("modifier = \"out\""), 16, "modifier = \"" + std::string(modifier) + "\"");
            Rejects([&] { ci::ParseCustomTypeConfig(text, "invalid.toml"); });
        }
        for (auto modifier : {"123", "true", "''"}) {
            auto text = std::string(declaration);
            text.replace(text.find("modifier = \"out\""), 16, "modifier = " + std::string(modifier));
            Rejects([&] { ci::ParseCustomTypeConfig(text, "invalid.toml"); });
        }
        std::cout << "PASS: out parsing, metadata, recipe direction/return/static discrimination, native writes, "
                     "buffer guards, false result, virtual/interface override and invalid declarations\n";
    }

    struct Position {
        int32_t runtime;
        int32_t table;
        int32_t row;
    };
    static_assert(sizeof(Position) == 12);
    std::array<void*, 3> g_refArguments{};
    void MutatePosition(void* context, void* self, Position* position, void* work, void* opWork) noexcept {
        g_ctorContext = context;
        g_ctorSelf = self;
        g_refArguments = {position, work, opWork};
        position->runtime += 10;
        position->table += 20;
        position->row += 30;
    }
    int32_t ReplacePeer(void*, void*, void** peer, void* replacement, int32_t value) noexcept {
        auto previous = *peer;
        *peer = replacement;
        return previous == g_ctorSelf ? value : -1;
    }
    int32_t SumSix(void*, void*, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) noexcept {
        return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
    }
    Position ReturnPosition(void* context, void* self, Position* position, void* work, void* opWork) noexcept {
        MutatePosition(context, self, position, work, opWork);
        return *position;
    }

    void TestRefParameters() {
        Fixture fixture;
        auto positionType = uint32_t(fixture.types.size());
        fixture.types.push_back({.index = positionType, .implIndex = positionType, .objectType = 5});
        fixture.impls.push_back({.nameOffset = int32_t(fixture.AddString("Position")),
                                 .namespaceOffset = int32_t(fixture.AddString("Tests")),
                                 .fieldSize = sizeof(Position)});
        auto AddRecipe = [&](uint32_t result, uint32_t direction, bool isVirtual) {
            auto index = fixture.AddMethod(result, 3);
            auto* list = reinterpret_cast<ci::ParamList*>(fixture.bytes.data() +
                                                        fixture.methods[index].GetParameterOffset());
            auto* ids = reinterpret_cast<uint32_t*>(list + 1);
            fixture.params[ids[0]].type = positionType;
            fixture.params[ids[0]].modifier = 2;
            fixture.params[ids[0]].flags = direction;
            fixture.params[ids[1]].type = fixture.object;
            fixture.params[ids[2]].type = fixture.object;
            if (isVirtual) {
                fixture.methodImpls[index].flags |= 0x40;
            }
            return index;
        };
        AddRecipe(fixture.voidType, 2, true); // 相同指针 ABI 的 out 不应被选作 ref recipe。
        auto refRecipe = AddRecipe(fixture.voidType, 1, true);
        fixture.methodImpls[refRecipe].name = fixture.AddString("Mutate");
        AddRecipe(positionType, 0, false);
        fixture.AddMethod(fixture.integer, 6);
        auto peerRecipe = fixture.AddMethod(fixture.integer, 3);
        auto* peerList = reinterpret_cast<ci::ParamList*>(fixture.bytes.data() +
                                                        fixture.methods[peerRecipe].GetParameterOffset());
        auto* peerIds = reinterpret_cast<uint32_t*>(peerList + 1);
        fixture.params[peerIds[0]].type = fixture.object;
        fixture.params[peerIds[0]].modifier = 2;
        fixture.params[peerIds[1]].type = fixture.object;
        auto interfaceType = uint32_t(fixture.types.size());
        fixture.types.push_back({.index = interfaceType,
                                 .implIndex = interfaceType,
                                 .typeFlags = 0x20,
                                 .unknown2C = fixture.AppendData(&refRecipe, sizeof(refRecipe)),
                                 .objectType = 1,
                                 .interfaceIndex = 0});
        fixture.impls.push_back({.nameOffset = int32_t(fixture.AddString("IRef")),
                                 .namespaceOffset = int32_t(fixture.AddString("Tests")),
                                 .numManagedVtable = 1});
        fixture.header.numTypes = uint32_t(fixture.types.size());
        fixture.header.typesStartOfGenericsProbably = fixture.header.numTypes;
        fixture.header.types = fixture.types.data();
        fixture.header.numTypeImpl = uint32_t(fixture.impls.size());
        fixture.header.typeImpls = fixture.impls.data();
        fixture.header.numMethods = uint32_t(fixture.methods.size());
        fixture.header.methods = fixture.methods.data();
        fixture.header.numMethodImpl = uint32_t(fixture.methodImpls.size());
        fixture.header.methodImpls = fixture.methodImpls.data();
        fixture.header.numParams = uint32_t(fixture.params.size());
        fixture.header.params = fixture.params.data();
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        fixture.header.stringPool = fixture.strings.data();
        fixture.header.numBytePool = uint32_t(fixture.bytes.size());
        fixture.header.bytePool = fixture.bytes.data();
        auto specs = ci::ParseCustomTypeConfig(R"toml(schema_version = 1
[[types]]
name = "Tests.RefBase"
interfaces = ["Tests.IRef"]
[[types.methods]]
name = "Mutate"
kind = "virtual"
parameters = [
  { name = "position", type = "Tests.Position", modifier = "ref" },
  { name = "work", type = "System.Object" },
  { name = "opWork", type = "System.Object" }
]
binding = { function = "MutatePosition", version = 1 }
[[types]]
name = "Tests.RefDerived"
base = "Tests.RefBase"
[[types.methods]]
name = "Mutate"
kind = "override"
parameters = [
  { name = "position", type = "Tests.Position", modifier = "ref" },
  { name = "work", type = "System.Object" },
  { name = "opWork", type = "System.Object" }
]
binding = { function = "MutatePosition", version = 1 }
)toml", "ref.toml");
        specs[0].methods[1].binding.address = reinterpret_cast<void*>(&MutatePosition);
        specs[1].methods[1].binding.address = reinterpret_cast<void*>(&MutatePosition);
        specs[0].methods.push_back({.name = "SumSix", .returnType = "System.Int32",
                                    .parameters = {"a", "b", "c", "d", "e", "f"},
                                    .binding = {.backend = ci::Backend::Cpp,
                                                .address = reinterpret_cast<void*>(&SumSix)}});
        specs[0].methods.push_back({.name = "ReplacePeer", .returnType = "System.Int32",
                                    .parameters = {{"peer", "System.Object", ci::ParameterModifier::Ref},
                                                   {"replacement", "System.Object"}, {"value", "System.Int32"}},
                                    .binding = {.backend = ci::Backend::Cpp,
                                                .address = reinterpret_cast<void*>(&ReplacePeer)}});
        auto aggregate = specs[0].methods[1];
        aggregate.name = "ReturnPosition";
        aggregate.kind = ci::MethodKind::Instance;
        aggregate.returnType = "Tests.Position";
        aggregate.binding.address = reinterpret_cast<void*>(&ReturnPosition);
        specs[0].methods.push_back(aggregate);
        auto database = ci::BuildTypeDatabase(fixture.header, specs);
        database->encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        ci::CreateMethodTrampolines(*database);
        ci::PrepareInvokeAdapters(*database);
        auto& method = database->customMethods[1];
        auto* list = reinterpret_cast<const ci::ParamList*>(database->bytes.data() +
                                                          database->methods[method.index].GetParameterOffset());
        auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
        Check(list->count == 3 && list->invokeId == 100 + refRecipe &&
                  database->parameters[ids[0]].modifier == 2 && database->parameters[ids[0]].flags == 1,
              "three-argument ref selects ref recipe and preserves original In flag");
        for (auto methodIndex : {3u, 4u}) {
            auto* parameters = reinterpret_cast<const ci::ParamList*>(
                database->bytes.data() + database->methods[database->customMethods[methodIndex].index].GetParameterOffset());
            auto first = reinterpret_cast<const uint32_t*>(parameters + 1)[0];
            Check(database->parameters[first].modifier == 2 && database->parameters[first].flags == 0,
                  "unmarked ref recipe retains zero direction flags");
        }
        int context{}, self{};
        auto* work = reinterpret_cast<void*>(0x123456789ABull);
        auto* opWork = reinterpret_cast<void*>(0xABCDEF98765ull);
        struct {
            uint32_t before;
            Position value;
            uint32_t after;
        } buffer{0xCCCCCCCC, {1, 2, 3}, 0xCCCCCCCC};

        using FnMutate = void (*)(void* vmContext, void* object, Position* position, void* work, void* operatorWork);
        auto slot = database->methodImpls[database->methods[method.index].impl].virtualIndex;
        auto& derived = database->customTypes[1];
        auto* override = database->FindMethod(derived.virtualMethods[slot]);
        Check(override && database->methodImpls[database->methods[override->index].impl].virtualIndex == slot,
              "three-argument ref override retains virtual slot");
        const auto decoded = ci::ReadMethodDeclaration(*database, override->index);
        Check(decoded.kind == ci::MethodKind::Override && decoded.parameters.size() == 3 &&
                  decoded.parameters[0].type == "Tests.Position" &&
                  decoded.parameters[0].modifier == ci::ParameterModifier::Ref,
              "shared method declaration decoder preserves override kind and ref direction");
        reinterpret_cast<FnMutate>(override->entry)(&context, &self, &buffer.value, work, opWork);
        Check(buffer.value.runtime == 11 && buffer.value.table == 22 && buffer.value.row == 33 &&
                  buffer.before == 0xCCCCCCCC && buffer.after == 0xCCCCCCCC &&
                  g_ctorContext == &context && g_ctorSelf == &self &&
                  g_refArguments == std::array<void*, 3>{&buffer.value, work, opWork},
              "ref reads and updates 12-byte value, preserves vm/this, stack argument, pointer width and guards");
        auto& definition = database->types[derived.index];
        auto* interfaces = reinterpret_cast<const RE::REInterfaceList*>(
            database->bytes.data() + definition.getInterfaceListOffset());
        Check(derived.virtualMethods[interfaces->encodedTypes[0] >> 19] == override->index,
              "ref interface alias follows inherited override");
        auto sum = database->customMethods[2];
        Check(reinterpret_cast<int32_t (*)(void*, void*, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)>(
                  sum.entry)(&context, &self, 1, 2, 3, 4, 5, 6) == 91, "six explicit parameters preserve stack ABI");
        void* peer = &self;
        Check(reinterpret_cast<int32_t (*)(void*, void*, void**, void*, int32_t)>(database->customMethods[3].entry)(
                  &context, &self, &peer, work, 73) == 73 && peer == work,
              "ref managed reference receives existing pointer and writes through pointer-to-pointer");
        auto value = reinterpret_cast<Position (*)(void*, void*, Position*, void*, void*)>(
            database->customMethods[4].entry)(&context, &self, &buffer.value, work, opWork);
        Check(value.runtime == 21 && value.table == 42 && value.row == 63 &&
                  g_refArguments == std::array<void*, 3>{&buffer.value, work, opWork},
              "aggregate return plus three parameters preserves hidden return buffer and stack slots");
        auto overloads = specs;
        auto byValue = overloads[0].methods[3];
        byValue.parameters[0].modifier = ci::ParameterModifier::Value;
        overloads[0].methods.push_back(byValue);
        Check(OrderDeclarationsForTest(overloads).size() == 2, "by-value/ref overloads have distinct declarations");
        overloads[0].methods.back().parameters[0].modifier = ci::ParameterModifier::Out;
        Rejects([&] { OrderDeclarationsForTest(overloads); });
        auto broken = specs;
        broken[1].methods[1].parameters[0].modifier = ci::ParameterModifier::Out;
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, broken); });
        broken = specs;
        broken[0].methods[1].parameters[0].type = "System.Void";
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, broken); });
        broken = specs;
        broken[0].methods[2].parameters.push_back("g");
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, broken); }); // 无七参数 recipe，不伪造 invokeId。
        std::cout << "PASS: ref parsing, original flags, three/six arguments, virtual/interface override, value/reference "
                     "read-write, stack ABI, aggregate return and recipe rejection\n";
    }

    void TestExportRef(const RE::TDB& header) {
        auto plugin = ci::ParseProviderConfig(R"toml(schema_version = 1
[plugin]
dll = "ClassInjectorTests_ci.dll"
[[types]]
name = "Tests.ExportRef"
base = "ace.btable.cExportBTableBase"
abstract = true
[[types.methods]]
name = "updateTableInpl"
kind = "override"
parameters = [
  { name = "position", type = "ace.btable.BTableDef.POSITION", modifier = "ref" },
  { name = "work", type = "ace.btable.cCommandWork" },
  { name = "opWork", type = "ace.btable.cOperatorWork" }
]
binding = { function = "UpdateTableInplProbe", version = 1 }
)toml", std::filesystem::path(CI_TEST_MANIFESTS) / "class/ClassInjectorTests_ci.toml");
        Check(ci::LoadClassProviderBindings(plugin), "three-argument ref binding resolves through DLL registration");
        auto module = GetModuleHandleW(plugin.dllPath.c_str());
        Check(module && !GetProcAddress(module, "UpdateTableInplProbe"), "ref implementation need not be exported");
        auto getCall = reinterpret_cast<FnGetNativeCallProbe>(GetProcAddress(module, "GetNativeCallProbe"));
        Check(getCall != nullptr, "ref provider call probe");
        auto database = ci::BuildTypeDatabase(header, plugin.types);
        database->encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        ci::CreateMethodTrampolines(*database);
        ci::PrepareInvokeAdapters(*database);
        auto& method = database->customMethods[1];
        auto* list = reinterpret_cast<const ci::ParamList*>(database->bytes.data() +
                                                          database->methods[method.index].GetParameterOffset());
        auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
        auto& type = database->customTypes[0];
        auto& parent = database->types[type.parent];
        const auto slot = database->methodImpls[database->methods[method.index].impl].virtualIndex;
        auto originalId =
            reinterpret_cast<const uint32_t*>(database->bytes.data() + parent.unknown2C)[slot];
        auto* original = reinterpret_cast<const ci::ParamList*>(database->bytes.data() +
                                                               database->methods[originalId].GetParameterOffset());
        Check(list->count == 3 && list->invokeId == original->invokeId && list->invokeId == 45 &&
                  database->parameters[ids[0]].modifier == 2 && database->parameters[ids[0]].flags == 1 &&
                  type.virtualMethods[slot] == method.index,
              "real updateTableInpl ref POSITION/work/opWork, original invoke recipe, In flag and override slot");
        int context{}, self{};
        auto* work = reinterpret_cast<void*>(0x123456789ABull);
        auto* opWork = reinterpret_cast<void*>(0xABCDEF98765ull);
        struct {
            uint32_t before;
            Position value;
            uint32_t after;
        } buffer{0xCCCCCCCC, {7, 8, 9}, 0xCCCCCCCC};

        auto* query = QueryMethod(
            database, QueryType(database, "Tests.ExportRef"),
            "updateTableInpl(ace.btable.BTableDef.POSITION, ace.btable.cCommandWork, ace.btable.cOperatorWork)");
        Check(query == method.entry, "RF-style three-parameter ref query returns registered entry");
        reinterpret_cast<void (*)(void*, void*, Position*, void*, void*)>(query)(
            &context, &self, &buffer.value, work, opWork);
        auto captured = *getCall();
        Check(buffer.value.runtime == 17 && buffer.value.table == 28 && buffer.value.row == 39 &&
                  buffer.before == 0xCCCCCCCC && buffer.after == 0xCCCCCCCC &&
                  captured.vmctx == &context && captured.self == &self && captured.first == uintptr_t(&buffer.value) &&
                  captured.second == uintptr_t(work) && captured.third == uintptr_t(opWork),
              "real POSITION native entry reads/writes caller data and preserves third stack argument");
        std::cout << "PASS: real TDB81 updateTableInpl override, ref POSITION, invokeId/flags/virtual slot, "
                     "RF query and three-argument native entry\n";
    }

    void TestReferenceArrays(const RE::TDB& header) {
        ci::CustomTypeConfig node{.name = "Tests.ArrayNode", .fields = {{"Value", "System.Int32", true}, {"Children", "Tests.ArrayNode[]", true}}};
        ci::CustomTypeConfig holder{.name = "Tests.ArrayHolder", .fields = {{"Nodes", "Tests.ArrayNode[]", true}, {"Numbers", "System.Int32[]", true}}};
        auto metadata = ci::BuildTypeDatabase(header, {holder, node});
        auto* array = metadata->FindType("Tests.ArrayNode[]");
        Check(array && metadata->customTypes.size() == 3, "deduplicated implicit reference array identity");
        const auto& definition = metadata->types[array->index];
        auto element = metadata->ResolveType(node.name);
        auto parent = metadata->ResolveType("System.Array");
        Check(definition.objectType == 2 && definition.arrayRank == 1 && definition.elementTypeIndex == element &&
              definition.parentTypeIndex == parent && metadata->types[element].arrayTypeIndex == array->index,
              "native array kind/rank/element/parent and element reverse lookup");
        Check(!definition.defaultCtorIndex && !metadata->typeImpls[definition.implIndex].numMemberMethods &&
              !metadata->typeImpls[definition.implIndex].fieldSize, "array has no ordinary class constructor or fields");
          Check(definition.interfaceListOffset == metadata->types[parent].interfaceListOffset,
                "new arrays never advertise donor element generic interfaces");
          const auto prototype = metadata->ResolveType("System.Object[]");
          Check(header.types[prototype].typeInfo &&
                header.types[prototype].typeInfo == header.types[parent].typeInfo,
                "real native arrays share System.Array TypeInfo rather than class-unique TypeInfo");
          auto& runtimeArray = metadata->types[array->index];
          std::array<void*, 3> arrayInfo{};
          arrayInfo[2] = &runtimeArray;
          runtimeArray.typeInfo = metadata->types[parent].typeInfo;
          Check(!metadata->IsRuntimeTypeReady(*array), "shared TypeInfo alone does not make an array ready");
          runtimeArray.objectInfo = arrayInfo.data() + 2;
          Check(metadata->IsRuntimeTypeReady(*array), "fake native builder finalizes shared-TypeInfo array with own ObjectInfo");
          arrayInfo[2] = &metadata->types[parent];
          Check(!metadata->IsRuntimeTypeReady(*array), "array ObjectInfo must identify the actual element-specific type");
          arrayInfo[2] = &runtimeArray;
          runtimeArray.typeInfo = arrayInfo.data();
          Check(!metadata->IsRuntimeTypeReady(*array), "array readiness rejects unrelated TypeInfo");
          runtimeArray.typeInfo = metadata->types[parent].typeInfo;
          auto* runtimeNode = metadata->FindType(node.name);
          auto& nodeDefinition = metadata->types[runtimeNode->index];
          std::array<void*, 3> nodeInfo{};
          nodeInfo[2] = &nodeDefinition;
          nodeDefinition.objectInfo = nodeInfo.data() + 2;
          nodeDefinition.typeInfo = metadata->types[runtimeNode->parent].typeInfo;
          Check(!metadata->IsRuntimeTypeReady(*runtimeNode), "ordinary class must still have independent TypeInfo");
          nodeDefinition.typeInfo = nodeInfo.data();
          Check(metadata->IsRuntimeTypeReady(*runtimeNode), "ordinary class with own TypeInfo and ObjectInfo is ready");
          runtimeArray.typeInfo = runtimeArray.objectInfo = nullptr;
          nodeDefinition.typeInfo = nodeDefinition.objectInfo = nullptr;
        auto* field = metadata->GetFieldDef(&metadata->types[metadata->ResolveType(holder.name)], "Nodes");
        Check(field && metadata->IsReferenceType(uint32_t(field->type)) && field->type == array->index, "serialized array field is an actual managed array reference");
          Check(metadata->GetFullName(metadata->ResolveType("System.Int32[,]")) == "System.Int32[,]" &&
                metadata->GetFullName(metadata->ResolveType("System.Int32[]")) == "System.Int32[]",
                "rank-specific names do not alias multidimensional arrays to vectors");
          ci::CustomTypeConfig payload{.name = "Tests.ArrayPayload"};
          ci::CustomTypeConfig methodsOnly{
              .name = "Tests.ArrayMethods", .methods = {
                  {.name = "GetItems", .kind = ci::MethodKind::Abstract, .returnType = "Tests.ArrayPayload[]"},
                  {.name = "SetItems", .kind = ci::MethodKind::Abstract,
                   .parameters = {{"items", "Tests.ArrayPayload[]"}}}}, .abstract = true};
          auto signatureMetadata = ci::BuildTypeDatabase(header, {methodsOnly, payload});
          auto signatureArray = signatureMetadata->ResolveType("Tests.ArrayPayload[]");
          Check(signatureMetadata->customTypes.size() == 3 && signatureMetadata->types[signatureArray].objectType == 2,
                "method-only return/parameter references synthesize one array identity");
          ci::CustomTypeConfig argument{.name = "Tests.ArrayArgument"};
          ci::CustomTypeConfig generic{.name = "Tests.ArrayGeneric`1<Tests.ArrayArgument[]>",
                               .genericArguments = {"Tests.ArrayArgument[]"},
                               .genericDefinition = "Tests.ArrayGeneric`1"};
          auto genericMetadata = ci::BuildTypeDatabase(header, {generic, argument});
          auto genericArray = genericMetadata->ResolveType("Tests.ArrayArgument[]");
          auto* closed = genericMetadata->FindType(generic.name);
          const auto* descriptor = reinterpret_cast<const uint32_t*>(genericMetadata->bytes.data() +
                                    genericMetadata->types[closed->index].genericsOffset);
          Check(descriptor[1] == genericArray && genericMetadata->types[genericArray].objectType == 2 &&
                    genericMetadata->types[genericArray].elementTypeIndex == genericMetadata->ResolveType(argument.name),
                "array used only as a generic argument is synthesized before the closure");
          ci::CustomTypeConfig fakeArray{.name = "Tests.ArrayNode[]"};
          Rejects([&] { ci::BuildTypeDatabase(header, {holder, node, fakeArray}); });
        auto invalid = holder;
        invalid.fields[0].type = "Tests.Missing[]";
        Rejects([&] { ci::BuildTypeDatabase(header, {invalid}); });
        invalid.fields[0].type = "Tests.ArrayNode[][]";
        Rejects([&] { ci::BuildTypeDatabase(header, {node, invalid}); });
        invalid.fields[0].type = "Tests.ArrayNode[,]";
        Rejects([&] { ci::BuildTypeDatabase(header, {node, invalid}); });
          std::cout << "PASS: implicit reference arrays, recursive fields, rank identity, reverse lookup, native storage, shared-TypeInfo finalization and class-unique TypeInfo regression\n";
    }

    // The graph profile's 41 -> 42 change adds an empty polymorphic base and
    // narrows one reference field. It must not change data offsets or GC maps.
    void TestMotionSelectionInheritance(const RE::TDB& header) {
        const std::string prefix = "Tests.MotionSelection.";
        const auto id = prefix + "MotID", selection = prefix + "Selection";
        const auto single = prefix + "Single", leftRight = prefix + "LeftRight";
        const auto multiId = prefix + "MultiID", multi = prefix + "Multi";
        const auto leftRightMulti = prefix + "LeftRightMulti", play = prefix + "Play";
        std::vector<ci::CustomTypeConfig> oldTypes{
            {.name = id, .fields = {{"Bank", "System.Int32"}, {"Motion", "System.Int32"}}},
            {.name = single, .fields = {{"Id", id}}},
            {.name = leftRight, .fields = {{"Left", single}, {"Right", single}}},
            {.name = multiId, .fields = {{"A", id}, {"B", id}, {"C", id}, {"D", id}, {"E", id}, {"F", id}}},
            {.name = multi, .fields = {{"Id", multiId}}},
            {.name = leftRightMulti, .fields = {{"Left", multi}, {"Right", multi}}},
            {.name = play, .fields = {{"PlayType", "System.Int32"}, {"Info", "System.Object"}}}};
        auto newTypes = oldTypes;
        newTypes.push_back({.name = selection});
        for (auto& type : newTypes) {
            if (type.name == single || type.name == leftRight || type.name == multi || type.name == leftRightMulti)
                type.base = selection;
            if (type.name == play) type.fields[1].type = selection;
        }
        const auto oldImpls = std::vector<RE::RETypeImpl>(header.typeImpls, header.typeImpls + header.numTypeImpl);
        auto before = ci::BuildTypeDatabase(header, oldTypes);
        auto after = ci::BuildTypeDatabase(header, newTypes);
        auto Bitmap = [](const RE::RETypeImpl& impl, size_t offset) {
            uint64_t value{};
            Check(impl.fieldSize <= 0x200, "motion selection uses inline reference maps");
            std::memcpy(&value, reinterpret_cast<const uint8_t*>(&impl) + offset, sizeof(value));
            return value;
        };
        for (const auto& spec : oldTypes) {
            auto* a = before->FindType(spec.name);
            auto* b = after->FindType(spec.name);
            const auto& ai = before->typeImpls[before->types[a->index].implIndex];
            const auto& bi = after->typeImpls[after->types[b->index].implIndex];
            const auto beforeFields = ReadInstanceFields(*before, a->index);
            const auto afterFields = ReadInstanceFields(*after, b->index);
            Check(ai.fieldSize == bi.fieldSize && beforeFields.size() == afterFields.size(),
                  "empty polymorphic parent preserves motion-selection data size");
            Check(Bitmap(ai, 0x20) == Bitmap(bi, 0x20) && Bitmap(ai, 0x28) == Bitmap(bi, 0x28),
                  "empty polymorphic parent preserves both GC maps");
            for (size_t i = 0; i < beforeFields.size(); ++i) {
                Check(beforeFields[i].offset == afterFields[i].offset
                          && beforeFields[i].reference == afterFields[i].reference,
                      "narrowing Object to selection does not reinterpret scalar slots");
            }
        }
        auto Shape = [&](const std::string& name, uint32_t size, uint64_t bits) {
            auto* type = after->FindType(name);
            const auto& impl = after->typeImpls[after->types[type->index].implIndex];
            Check(impl.fieldSize == size && Bitmap(impl, 0x20) == bits && Bitmap(impl, 0x28) == bits,
                  "motion selection has exact reference/cycle bitmap");
        };
        Shape(selection, 0, 0);
        Shape(id, 8, 0);
        Shape(single, 8, 1);
        Shape(leftRight, 16, 3);
        Shape(multiId, 48, 0x3f);
        Shape(multi, 8, 1);
        Shape(leftRightMulti, 16, 3);
        Shape(play, 16, 2); // Int32 PlayType=2 is data, not a managed pointer.
        std::array<uint64_t, 2> payload{2, 0};
        const auto* playType = after->FindType(play);
        const auto& playImpl = after->typeImpls[after->types[playType->index].implIndex];
        auto map = Bitmap(playImpl, 0x20);
        for (size_t slot = 0; slot < payload.size(); ++slot)
            Check(!(map & (uint64_t(1) << slot)) || payload[slot] != 2,
                  "GC walk cannot select scalar PlayType value 2");
        for (uint32_t i = 0; i < header.numTypeImpl; ++i) {
            Check(std::memcmp(&oldImpls[i], &header.typeImpls[i], sizeof(RE::RETypeImpl)) == 0,
                  "motion selection metadata build never writes source native TypeImpl");
            if (header.typeImpls[i].fieldSize <= 0x200) {
                Check(std::memcmp(&oldImpls[i], &after->typeImpls[i], sizeof(RE::RETypeImpl)) == 0,
                      "new polymorphic classes leave original inline GC maps byte-identical");
            }
        }
        std::cout << "PASS: empty motion-selection inheritance, narrowed reference field, exact GC/cycle maps, "
                     "scalar2 exclusion and original native TypeImpl preservation\n";
    }

    // 真实 TDB 的浮点 recipe 保留完整返回类型；同尺寸整数/结构体不算兼容。
    static void TestFloatingReturnMetadata(const RE::TDB& header) {
        std::ifstream stream(CI_TEST_FLOAT_MANIFEST, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        auto plugin = ci::ParseProviderConfig(text, CI_TEST_FLOAT_MANIFEST);
        Check(ci::LoadClassProviderBindings(plugin), "floating return provider bindings");
        auto database = ci::BuildTypeDatabase(header, plugin.types);
        database->encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        const auto expectedMethods = database->methodDeclarations;
        ci::CreateMethodTrampolines(*database);
        ci::PrepareInvokeAdapters(*database);
        size_t methods = 0;
        for (const auto& method : database->customMethods) {
            const auto& declaration = expectedMethods[method.index - database->originalMethodCount];
            if (declaration.kind == ci::MethodKind::Constructor) continue;
            ++methods;
            const auto& definition = database->methods[method.index];
            const auto* list = reinterpret_cast<const ci::ParamList*>(database->bytes.data() + definition.GetParameterOffset());
            const auto expected = database->ResolveType(declaration.returnType);
            Check(list->invokeId && database->parameters[list->returnParameter].type == expected, "floating return type/invoke recipe preserved");
            Check(database->typeImpls[database->types[expected].implIndex].fieldSize ==
                      (declaration.returnType == "System.Single" ? 4 : 8), "floating return payload size");
            const auto* code = static_cast<const uint8_t*>(method.entry);
            Check(code[0] == 0xFF && code[1] == 0x25, "floating return entry is a native tail jump");
        }
        Check(methods == 6, "ordinary/virtual/override Single and Double methods built");
        // 移除所有 Double recipe 后应生成正确的 Double adapter，而非借 Int32 返回协议。
        std::vector<ci::Parameter> parameters(static_cast<const ci::Parameter*>(header.params), static_cast<const ci::Parameter*>(header.params) + header.numParams);
        const auto real = database->ResolveType("System.Double");
        const auto integer = database->ResolveType("System.Int32");
        for (auto& parameter : parameters) if (parameter.type == real) parameter.type = integer;
        auto missing = header;
        missing.params = parameters.data();
        auto generated = ci::BuildTypeDatabase(missing, plugin.types);
        size_t doubleAdapters{};
        for (const auto& method : generated->customMethods) {
            if (generated->methodDeclarations[method.index - generated->originalMethodCount].returnType == "System.Double") {
                Check(generated->invokeSignatures[method.index - generated->originalMethodCount].result == ci::InvokeValueKind::Double,
                      "missing Double recipe generates Double ABI");
                ++doubleAdapters;
            }
        }
        Check(doubleAdapters == 3, "ordinary/virtual/override Double generated adapters");
        std::cout << "PASS: real Single/Double ordinary/virtual/override returns, native tail entries and generated missing recipes\n";
    }

    // 可选插件清单验收：使用真实TDB布局构建全套元数据，而非从RSZ字段大小猜原生offset。
    void TestAdditionalProvider(const RE::TDB& header, const char* manifest, const char* output) {
        std::ifstream source(manifest, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        auto plugin = ci::ParseProviderConfig(text, std::filesystem::absolute(manifest));
        Check(ci::LoadClassProviderBindings(plugin), "additional provider bindings");
        auto metadata = ci::BuildTypeDatabase(header, plugin.types);
        auto registered = plugin.types.size();
        Check(registered != 0 && metadata->customTypes.size() >= registered, "all resource declarations and host types registered");
        // Provider manifests evolve independently of this fixture. Verify every
        // actual declaration, rather than pinning one historical 415-type pack.
        for (const auto& declared : plugin.types) {
            Check(std::ranges::count_if(metadata->customTypes, [&](const auto& type) {
                return type.name == declared.name;
            }) == 1, ("additional provider type registered exactly once: " + declared.name).c_str());
        }
        if (output) {
            std::ofstream stream(output, std::ios::binary);
            stream << "{\"scope\":\"TDB81 offline native metadata; live offsets/TypeInfo CRC still need validation\",\"types\":[";
            bool firstType = true;
            for (const auto& type : metadata->customTypes) {
                if (!firstType) stream << ',';
                firstType = false;
                const auto& definition = metadata->types[type.index];
                auto nativeParent = type.parent;
                while (nativeParent >= metadata->originalCount) nativeParent = uint32_t(metadata->types[nativeParent].parentTypeIndex);
                const auto& parent = metadata->types[nativeParent];
                auto prefix = parent.instanceSize - metadata->typeImpls[parent.implIndex].fieldSize;
                Check(prefix >= 16 && prefix < 65536, "native parent managed field block prefix");
                stream << "{\"name\":\"" << type.name << "\",\"fieldSize\":" << metadata->typeImpls[definition.implIndex].fieldSize
                       << ",\"managedFieldOffset\":" << prefix << ",\"nativeParent\":\"" << metadata->GetFullName(nativeParent)
                       << "\",\"kind\":" << definition.objectType << ",\"hash\":" << definition.fqnHash << ",\"fields\":[";
                bool firstField = true;
                for (const auto& field : ReadInstanceFields(*metadata, type.index)) {
                    if (!firstField) stream << ',';
                    firstField = false;
                    stream << "{\"name\":\"" << field.name << "\",\"type\":\"" << metadata->GetFullName(field.type)
                           << "\",\"offset\":" << field.offset << ",\"size\":"
                           << (field.reference ? 8 : metadata->typeImpls[metadata->types[field.type].implIndex].fieldSize)
                           << ",\"reference\":" << (field.reference ? "true" : "false") << '}';
                }
                stream << "]}";
            }
            stream << "]}\n";
        }
        std::cout << "PASS: additional provider " << registered << " declarations, " << metadata->customTypes.size()
                  << " class/array metadata shapes and inherited virtual slots\n";
    }

    void TestClosedTypes(const RE::TDB& header) {
        auto previousCtorCalls = g_ctorCalls;
        auto plugin = ReadFixture("closed", "ClassInjectorClosedTypes_ci.toml");
        Check(ci::LoadClassProviderBindings(plugin), "closed provider registration");
        auto specs = plugin.types;
        ci::g_database = ci::BuildTypeDatabase(header, specs);
        auto& database = *ci::g_database;
        auto definition = database.ResolveType("ClassInjector.Tests.ThinkTimerReset`1");
        auto enumeration = database.ResolveType("System.DayOfWeek");
        auto* value = database.FindType(specs[0].name);
        auto* derived = database.FindType(specs[1].name);
        auto* reference = database.FindType(specs[2].name);
        Check(database.customTypes.size() == 5 && value && derived && reference, "shared generated definition");
        auto& open = database.types[definition];
        auto& openImpl = database.typeImpls[open.implIndex];
        auto* openDescriptor = reinterpret_cast<const uint32_t*>(database.bytes.data() + open.genericsOffset);
        Check((open.typeFlags & 0x08000000) && openDescriptor[0] == (definition | (1u << 19)) &&
                  openDescriptor[1] == 0 && !open.defaultCtorIndex && !open.instanceSize &&
                  !openImpl.numMemberFields && !openImpl.numMemberMethods && !openImpl.fieldSize,
              "definition identity has no object layout or executable methods");
        Check(openDescriptor[2] == 0 && std::string_view(database.strings.data() + openDescriptor[3]) == "T",
              "open identity descriptor contains an unconstrained named parameter");
        for (auto* type : {value, reference}) {
            auto& metadata = database.types[type->index];
            auto* descriptor = reinterpret_cast<const uint32_t*>(database.bytes.data() + metadata.genericsOffset);
            Check(type->genericDefinition == definition && descriptor[0] == (definition | (1u << 19)) &&
                      !(metadata.typeFlags & 0x08000000) && database.GetFullName(type->index) == type->name &&
                      QueryType(database, type->name) == &metadata,
                  "closed type identity/full name/query");
        }
        auto* field = database.GetFieldDef(&database.types[value->index], "_TimerType");
        auto* id = database.GetFieldDef(&database.types[value->index], "_TimerUniqueId");
        auto* pointer = database.GetFieldDef(&database.types[reference->index], "_TimerType");
        Check(field->type == enumeration && !database.IsReferenceType(uint32_t(field->type)) &&
                  database.fieldImpls[field->impl].offset == 0 && database.fieldImpls[id->impl].offset == 4 &&
                  database.IsReferenceType(uint32_t(pointer->type)) &&
                  database.fieldImpls[database.GetFieldDef(&database.types[reference->index], "_TimerUniqueId")->impl].offset == 8,
              "enum and reference closures have separate storage shapes");
        Check(database.IsAssignable(derived->index, value->index), "ordinary subclass of custom closed type");
        database.encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        auto object = database.ResolveType("System.Object");
        database.methods[database.types[object].defaultCtorIndex].encodedOffset = 0x1000;
        ci::CreateMethodTrampolines(database);
        ci::PrepareInvokeAdapters(database);
        static bool initialized = false;
        if (!initialized) {
            InitializeTestProvider(GetModuleHandleW(plugin.dllPath.c_str()));
            initialized = true;
        }
        std::array<void*, 4> info{};
        info[1] = reinterpret_cast<void*>(16);
        info[2] = &database.types[derived->index];
        struct Object { void** info; uint64_t header; std::array<uint8_t, 32> fields; } self{info.data() + 2};
        uint64_t vmctx{};
        auto ctor = database.FindMethod(uint32_t(database.types[derived->index].defaultCtorIndex));
        reinterpret_cast<void (*)(void*, void*)>(ctor->entry)(&vmctx, &self);
        auto getter = QueryMethod(database, &database.types[derived->index], "get_TimerType()");
        auto setter = QueryMethod(database, &database.types[derived->index], "set_TimerType(System.DayOfWeek)");
        Check(getter && setter, "inherited enum method signatures");
        auto start = QueryMethod(database, &database.types[derived->index], "start(via.behaviortree.ActionArg)");
        auto implementation = std::ranges::find_if(database.customMethods, [&](auto& m) { return m.entry == start; });
        Check(implementation != database.customMethods.end(), "closed parent override found");
        const auto slot = database.methodImpls[database.methods[implementation->index].impl].virtualIndex;
        Check(slot >= 0 && derived->virtualMethods[slot] == implementation->index &&
                  value->virtualMethods[slot] != implementation->index,
              "closed parent virtual slot is overridden by ordinary child");
        for (int32_t i = 0; i < 100; ++i) {
            reinterpret_cast<void (*)(void*, void*, int32_t)>(setter)(&vmctx, &self, i - 50);
            Check(reinterpret_cast<int32_t (*)(void*, void*)>(getter)(&vmctx, &self) == i - 50,
                  "enum setter/getter Int32 ABI preserves signed values");
            reinterpret_cast<void (*)(void*, void*, void*)>(start)(&vmctx, &self, nullptr);
        }
        Check(*reinterpret_cast<int32_t*>(self.fields.data() + 4) == -100, "derived start binding");
        auto invalid = specs;
        invalid[0].name = "ClassInjector.Tests.BadName";
        Rejects([&] { ci::BuildTypeDatabase(header, invalid); });
        invalid = specs;
        invalid[2].base = "System.Diagnostics.Stopwatch";
        Rejects([&] { ci::BuildTypeDatabase(header, invalid); });
        invalid = specs;
        invalid[0].genericArguments = {"System.Object", "System.Object"};
        Rejects([&] { ci::BuildTypeDatabase(header, invalid); });
        invalid = specs;
        invalid[0].genericArguments = {"ClassInjector.Tests.ThinkTimerReset`1"};
        invalid[0].name = "ClassInjector.Tests.ThinkTimerReset`1<ClassInjector.Tests.ThinkTimerReset`1>";
        Rejects([&] { ci::BuildTypeDatabase(header, invalid); });
        if (database.typeNames.contains("System.Guid")) {
            invalid = specs;
            invalid[0].genericArguments = {"System.Guid"};
            invalid[0].name = "ClassInjector.Tests.ThinkTimerReset`1<System.Guid>";
            Rejects([&] { ci::BuildTypeDatabase(header, invalid); });
        }
        for (auto declaration : {
                 "generic_definition='Tests.Generic`1'",
                 "generic_arguments=['System.Int32']",
                 "generic_definition='Tests.Generic`1'\ngeneric_template='Tests.Template'\ngeneric_arguments=['System.Int32']"}) {
            Rejects([&] { ci::ParseCustomTypeConfig("schema_version=1\n[[types]]\nname='Tests.Bad'\n" +
                                                  std::string(declaration), "invalid_closed.toml"); });
        }
        auto multiple = specs;
        multiple[2].genericArguments[0] = multiple[1].name;
        multiple[2].fields[0].type = multiple[1].name;
        for (size_t i : {0u, 2u}) {
            multiple[i].genericDefinition = "ClassInjector.Tests.ThinkTimerReset`2";
            multiple[i].genericArguments.push_back("System.Int32");
            multiple[i].name = multiple[i].genericDefinition + '<' + multiple[i].genericArguments[0] + ",System.Int32>";
            multiple[i + 1].base = multiple[i].name;
        }
        auto two = ci::BuildTypeDatabase(header, multiple);
        auto& twoDefinition = two->types[two->ResolveType("ClassInjector.Tests.ThinkTimerReset`2")];
        auto* twoDescriptor = reinterpret_cast<const uint32_t*>(two->bytes.data() + twoDefinition.genericsOffset);
        Check(((twoDescriptor[0] >> 19) & 127) == 2 && twoDescriptor[2] == 0 && twoDescriptor[4] == 0 &&
                  std::string_view(two->strings.data() + twoDescriptor[3]) == "T0" &&
                  std::string_view(two->strings.data() + twoDescriptor[5]) == "T1",
              "two-argument definition owns complete parameter records");
        g_ctorCalls = previousCtorCalls;
        std::cout << "PASS: custom closed generic definition identity, enum/reference layouts, Int32 enum ABI, "
                     "ordinary derived override, constructor, queries and invalid declarations\n";
    }

    // Decode only the tables carried by the snapshot; their storage outlives the borrowed header.
    static void LoadTdbSnapshot(std::istream& file, RE::TDB& header,
                                std::vector<std::vector<uint8_t>>& tables) {
        std::array<char, 8> prefix{};
        file.read(prefix.data(), prefix.size());
        Check(file.good(), "TDB snapshot format header");
        const bool versioned = std::string_view(prefix.data(), prefix.size()) == std::string_view("CITDB\0\1\0", 8);
        if (!versioned) file.seekg(0);
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        Check(file.good() && header.version == 81, "TDB snapshot header");
        auto oldPool = uintptr_t(header.bytePool);
        auto LoadTable = [&](void*& pointer, size_t size) {
            tables.emplace_back(size);
            file.read(reinterpret_cast<char*>(tables.back().data()), size);
            Check(file.good(), "TDB snapshot table");
            pointer = tables.back().data();
        };
        void* types = header.types;
        LoadTable(types, header.numTypes * sizeof(RE::RETypeDefinition));
        header.types = static_cast<RE::RETypeDefinition*>(types);
        void* impls = header.typeImpls;
        LoadTable(impls, header.numTypeImpl * sizeof(RE::RETypeImpl));
        header.typeImpls = static_cast<RE::RETypeImpl*>(impls);
        LoadTable(header.methods, header.numMethods * sizeof(ci::MethodDef));
        LoadTable(header.methodImpls, header.numMethodImpl * sizeof(ci::MethodImpl));
        LoadTable(header.fields, header.numFields * sizeof(ci::FieldDef));
        LoadTable(header.fieldImpls, header.numFieldImpl * sizeof(ci::FieldImpl));
        LoadTable(header.params, header.numParams * sizeof(ci::Parameter));
        LoadTable(header.stringPool, header.numStringPool);
        LoadTable(header.bytePool, header.numBytePool);
        if (versioned) {
            Check(header.numInitData >= 0, "TDB snapshot initializer count");
            LoadTable(header.initData, size_t(header.numInitData) * sizeof(int32_t));
        } else {
            // Legacy snapshots never captured this table; the saved VA belongs to another process.
            header.initData = nullptr;
            header.numInitData = 0;
        }
        for (size_t i = 1; i < header.numTypeImpl; ++i) {
            if (header.typeImpls[i].fieldSize > 512) {
                for (auto offset : {32, 40}) {
                    auto* at = reinterpret_cast<uint8_t*>(&header.typeImpls[i]) + offset;
                    uintptr_t pointer;
                    std::memcpy(&pointer, at, 8);
                    if (pointer >= oldPool && pointer < oldPool + header.numBytePool) {
                        pointer = uintptr_t(header.bytePool) + pointer - oldPool;
                        std::memcpy(at, &pointer, 8);
                    }
                }
            }
        }
    }

    static void TestTdbSnapshot() {
        Fixture fixture;
        std::array<int32_t, 3> initializers{0, 4, -8};
        fixture.header.numInitData = initializers.size();
        fixture.header.initData = initializers.data();
        std::string legacy(reinterpret_cast<const char*>(&fixture.header), sizeof(fixture.header));
        for (auto [data, size] : {
                 std::pair<const void*, size_t>{fixture.types.data(), fixture.types.size() * sizeof(RE::RETypeDefinition)},
                 {fixture.impls.data(), fixture.impls.size() * sizeof(RE::RETypeImpl)},
                 {fixture.methods.data(), fixture.methods.size() * sizeof(ci::MethodDef)},
                 {fixture.methodImpls.data(), fixture.methodImpls.size() * sizeof(ci::MethodImpl)},
                 {fixture.fields.data(), fixture.fields.size() * sizeof(ci::FieldDef)},
                 {fixture.fieldImpls.data(), fixture.fieldImpls.size() * sizeof(ci::FieldImpl)},
                 {fixture.params.data(), fixture.params.size() * sizeof(ci::Parameter)},
                 {fixture.strings.data(), fixture.strings.size()}, {fixture.bytes.data(), fixture.bytes.size()}}) {
            legacy.append(static_cast<const char*>(data), size);
        }
        auto current = std::string("CITDB\0\1\0", 8) + legacy;
        current.append(reinterpret_cast<const char*>(initializers.data()), sizeof(initializers));
        for (bool versioned : {false, true}) {
            std::istringstream input(versioned ? current : legacy);
            RE::TDB header{};
            std::vector<std::vector<uint8_t>> tables;
            LoadTdbSnapshot(input, header, tables);
            Check(header.types != fixture.types.data() &&
                      std::memcmp(header.types, fixture.types.data(), fixture.types.size() * sizeof(RE::RETypeDefinition)) == 0,
                  "snapshot type table is relocated without changing records");
            if (versioned) {
                Check(header.initData != fixture.header.initData && header.numInitData == initializers.size() &&
                          std::memcmp(header.initData, initializers.data(), sizeof(initializers)) == 0,
                      "versioned snapshot relocates initializer offsets");
            } else {
                Check(!header.initData && header.numInitData == 0, "legacy snapshot discards the remote initializer pointer");
            }
        }
        current.pop_back();
        std::istringstream truncated(current);
        RE::TDB header{};
        std::vector<std::vector<uint8_t>> tables;
        Rejects([&] { LoadTdbSnapshot(truncated, header, tables); });
        std::cout << "PASS: versioned initializer capture, legacy pointer clearing and incomplete snapshot rejection\n";
    }

    static void TestDeclarationTypes() {
        const std::string valid = R"(schema_version=1
[[types]]
name='Tests.Parsed'
base='System.Object'
extend=false
abstract=true
generic_template=''
generic_definition=''
generic_arguments=[]
interfaces=[]
fields=[{name='Counter',type='System.Int32',serialize=false}]
methods=[{name='Read',kind='abstract',return_type='System.Void',override_of='',parameters=[]}]
)";
        Check(ci::ParseCustomTypeConfig(valid, "types.toml").size() == 1, "valid optional declaration types");
        for (const auto& [before, after] : {
                 std::pair{"schema_version=1", "schema_version=1.0"},
                 {"base='System.Object'", "base=123"},
                 {"extend=false", "extend='false'"}, {"abstract=true", "abstract=1"},
                 {"generic_template=''", "generic_template=123"},
                 {"generic_definition=''", "generic_definition=false"},
                 {"generic_arguments=[]", "generic_arguments={}"},
                 {"interfaces=[]", "interfaces='Tests.Interface'"},
                 {"fields=[{name='Counter',type='System.Int32',serialize=false}]", "fields={}"},
                 {"serialize=false", "serialize='false'"},
                 {"methods=[{name='Read',kind='abstract',return_type='System.Void',override_of='',parameters=[]}]", "methods={}"},
                 {"kind='abstract'", "kind=123"}, {"return_type='System.Void'", "return_type=123"},
                 {"override_of=''", "override_of=123"}, {"parameters=[]", "parameters={}"}}) {
            auto invalid = valid;
            invalid.replace(invalid.find(before), std::strlen(before), after);
            bool rejected = false;
            try { ci::ParseCustomTypeConfig(invalid, "types.toml"); }
            catch (const std::exception& error) {
                const std::string_view key(before, std::strchr(before, '=') - before);
                Check(std::string_view(error.what()).contains(key), "type error identifies its declaration key");
                rejected = true;
            }
            if (!rejected) throw std::runtime_error(std::string("wrong TOML type accepted: ") + after);
        }
        auto defaults = ci::ParseCustomTypeConfig("schema_version=1\n[[types]]\nname='Tests.Defaults'", "defaults.toml");
        Check(defaults[0].base == "System.Object" && defaults[0].methods[0].kind == ci::MethodKind::Constructor,
              "absent optional properties retain defaults");
        const std::string extension =
            "schema_version=1\n[[types]]\nname='Tests.Existing'\nextend=true\n"
            "fields=[{name='Peer',type='Tests.New[]'}]\n";
        for (const auto* property : {"base='System.Object'", "abstract=false", "generic_template=''",
                                     "generic_definition=''", "generic_arguments=[]", "interfaces=[]", "methods=[]",
                                     "base='Tests.Unused'", "abstract=true", "generic_template='Tests.Unused'",
                                     "generic_definition='Tests.Unused`1'", "generic_arguments=['Tests.Unused']",
                                     "interfaces=['Tests.Unused']", "methods=[{name='Unused'}]",
                                     "base=123", "methods='unused'"}) {
            auto provider = ci::ParseProviderConfig(
                extension + property + "\n[[types]]\nname='Tests.AfterExtension'", "extension.toml");
            Check(provider.types.size() == 2 && provider.types[0].extend &&
                      provider.types[0].fields.size() == 1 && provider.types[0].fields[0].type == "Tests.New[]" &&
                      provider.types[1].name == "Tests.AfterExtension" && provider.types[1].methods.size() == 1,
                  "unused extension properties preserve the extension and subsequent declarations");
            const auto& type = provider.types[0];
            Check(type.base == "System.Object" && !type.abstract && type.genericTemplate.empty() &&
                      type.genericDefinition.empty() && type.genericArguments.empty() && type.interfaces.empty() &&
                      type.methods.empty(),
                  "unused class properties are not interpreted or copied into field extensions");
        }
        for (const auto* fields : {"", "fields=[]", "fields={}"}) {
            Rejects([&] {
                ci::ParseCustomTypeConfig(
                    std::string("schema_version=1\n[[types]]\nname='Tests.Existing'\nextend=true\n") + fields,
                    "extension.toml");
            });
        }
        auto mixed = ci::ParseCustomTypeConfig(extension + R"toml(
[[types]]
name='Tests.New'
base='Tests.Base'
[[types]]
name='Tests.Base'
abstract=true
[[enums]]
name='Tests.Enum'
renames=[{fromName='First',to='Second'}]
[[enums]]
name='Tests.Enum'
renames=[{fromName='Second',to='Third'}]
)toml", "mixed.toml");
        Check(mixed.size() == 5 && mixed[0].extend && mixed[0].fields[0].type == "Tests.New[]" &&
                  mixed[0].methods.empty() && mixed[0].interfaces.empty() && mixed[0].genericArguments.empty() &&
                  mixed[0].genericTemplate.empty() && mixed[0].genericDefinition.empty() && !mixed[0].enumExtension,
              "field extension finishes without class-only declarations");
        Check(mixed[1].name == "Tests.New" && mixed[1].base == "Tests.Base" && !mixed[1].IsTypeExtension() &&
                  mixed[1].methods.size() == 1 && mixed[1].methods[0].kind == ci::MethodKind::Constructor &&
                  mixed[2].name == "Tests.Base" && mixed[2].abstract,
              "extension early exit preserves subsequent class parsing and declaration order");
        Check(!mixed[3].extend && mixed[3].enumExtension && mixed[3].methods.empty() &&
                  mixed[3].enumExtension->renames[0].to == "Second" &&
                  std::get<std::string>(mixed[4].enumExtension->renames[0].source) == "Second" &&
                  mixed[4].enumExtension->renames[0].to == "Third" && mixed[0].providerSource == mixed[4].providerSource,
              "mixed enum declarations preserve operation order and shared provenance");
        Rejects([] {
            ci::ParseCustomTypeConfig("schema_version=1\n[[types]]\nname='Tests.BadVersion'\n"
                                  "methods=[{name='Run',binding={function='Run',version=1.0}}]", "version.toml");
        });
        // Marker policy is independent from strict declaration parsing.
        for (const auto* marker : {"", "[invalid", "schema_version=1\n[[types]]\nname='Tests.Bad'\nfields={}"}) {
            auto plugin = ci::ParseProviderConfig(marker, "marker.toml");
            Check(plugin.types.empty() && plugin.configPath == "marker.toml" && plugin.dllPath.filename() == "marker.dll", "invalid declarations retain the DLL marker policy");
        }
        std::cout << "PASS: optional declaration types, defaults and marker policy\n";
    }

    static void TestAuthorNameBoundaries() {
        Fixture fixture;
        for (const auto& [body, member] : {
                 std::pair{R"(fields=[{name='Counter',type='System.Int32'},{name="Counter\u0000Hidden",type='System.Int32'}])", "field"},
                 {R"(methods=[{name="Read\u0000Hidden",binding={function='Read',version=1}}])", "method"},
                 {R"(methods=[{name='Read',parameters=[{name="Input\u0000Hidden",type='System.Int32'}],binding={function='Read',version=1}}])", "parameter"}}) {
            auto declarations = ci::ParseCustomTypeConfig(
                std::string("schema_version=1\n[[types]]\nname='Tests.AuthorNames'\n") + body, "author-names.toml");
            const auto& type = declarations[0];
            const auto& name = std::string_view(member) == "field" ? type.fields.back().name
                               : std::string_view(member) == "method" ? type.methods.back().name
                                                                     : type.methods.back().parameters[0].name;
            Check(name.contains('\0'), "TOML preserves the NUL that the native name pool cannot represent");
            bool rejected = false;
            try { ci::BuildTypeDatabase(fixture.header, std::move(declarations)); }
            catch (const std::runtime_error& error) {
                Check(std::string_view(error.what()).contains(std::string(member) + " name contains NUL"),
                      "NUL name rejection identifies the author member kind");
                rejected = true;
            }
            Check(rejected, "author member names containing NUL must not reach the native name pool");
        }
        for (const auto* name : {".Example", "Tests.", "."}) {
            auto declarations = ci::ParseCustomTypeConfig(
                std::string("schema_version=1\n[[types]]\nname='") + name + "'", "author-names.toml");
            Rejects([&] { ci::BuildTypeDatabase(fixture.header, declarations); });
        }
        auto declarations = ci::ParseCustomTypeConfig(
            "schema_version=1\n[[types]]\nname='Tests.Names.Accepted'\n"
            "fields=[{name='<Count>k__BackingField',type='System.Int32'}]", "author-names.toml");
        auto database = ci::BuildTypeDatabase(fixture.header, std::move(declarations));
        const auto* type = QueryType(database, "Tests.Names.Accepted");
        Check(type && database->GetFullName(uint32_t(type->index)) == "Tests.Names.Accepted" &&
                  database->GetFieldDef(type, "<Count>k__BackingField") && database->GetMethodDef(type, ".ctor()"),
              "qualified names, backing fields and constructor punctuation remain valid");
        std::cout << "PASS: author NUL name representation, qualified type boundaries and valid reflection names\n";
    }

    // 来源跟随声明、合成形状与待生成方法；最终元数据不持有 Provider 路径。
    static void TestProviderSources() {
        Fixture fixture;
        ci::CustomTypeConfig config{.name = "Tests.Generated[]"};
        Check(config.FormatDiagnosticContext() == "Tests.Generated[]", "diagnostic context without provider source");
        config.providerSource = std::make_shared<ci::ProviderSource>(ci::ProviderSource{"source.toml", "source.dll"});
        Check(config.FormatDiagnosticContext() == "source.toml [source.dll]: Tests.Generated[]",
              "diagnostic context preserves provider paths and type name");
        ci::CustomTypeBuildSpec generated{std::move(config)};
        generated.requestedByType = "Tests.Owner";
        Check(generated.FormatDiagnosticContext() == "source.toml [source.dll]: Tests.Generated[] (requested by Tests.Owner)",
              "generated type diagnostic preserves requesting type and exact text");
        auto ExpectError = [](auto operation, std::initializer_list<std::string_view> fragments) {
            try { operation(); }
            catch (const std::runtime_error& error) {
                for (auto fragment : fragments) Check(std::string_view(error.what()).contains(fragment), "declaration diagnostic lost source/type/member context");
                std::cout << "DIAGNOSTIC: " << error.what() << '\n';
                return;
            }
            throw std::runtime_error("expected declaration error");
        };
        auto Invalid = [&](const char* file, const char* body) {
            auto declarations = ci::ParseCustomTypeConfig(std::string("schema_version=1\n") + body, file);
            ci::BuildTypeDatabase(fixture.header, std::move(declarations));
        };
        ExpectError([&] { Invalid("field-origin.toml", "[[types]]\nname='Tests.BadField'\nfields=[{name='Broken',type='Tests.Absent'}]"); },
                    {"field-origin.toml", "field-origin.dll", "Tests.BadField", "field Broken", "Tests.Absent"});
        ExpectError([&] { Invalid("method-origin.toml", "[[types]]\nname='Tests.BadMethod'\nmethods=[{name='Fetch',parameters=[{name='Input',type='Tests.Absent'}],binding={function='Fetch',version=1}}]"); },
                    {"method-origin.toml", "Tests.BadMethod", "method Fetch", "Tests.Absent"});
        ExpectError([&] { Invalid("template-origin.toml", "[[types]]\nname='Tests.Closed'\ngeneric_template='Tests.Absent'\ngeneric_arguments=['System.Int32']"); },
                    {"template-origin.toml", "Tests.Closed", "Tests.Absent"});
        ExpectError([&] { Invalid("array-origin.toml", "[[types]]\nname='Tests.ArrayOwner'\nfields=[{name='Items',type='Tests.Absent[]'}]"); },
                    {"array-origin.toml", "Tests.Absent[]", "requested by Tests.ArrayOwner", "Tests.Absent"});
        ExpectError([&] { Invalid("generic-origin.toml", "[[types]]\nname='Tests.Box`1<System.Int32>'\nbase='System.Int32'\ngeneric_definition='Tests.Box`1'\ngeneric_arguments=['System.Int32']"); },
                    {"generic-origin.toml", "Tests.Box`1", "requested by Tests.Box`1<System.Int32>", "base must be"});
        auto first = ci::ParseCustomTypeConfig("schema_version=1\n[[types]]\nname='Tests.Duplicate'", "first-provider.toml");
        auto second = ci::ParseCustomTypeConfig("schema_version=1\n[[types]]\nname='Tests.Duplicate'", "second-provider.toml");
        first.push_back(std::move(second[0]));
        ExpectError([&] { ci::BuildTypeDatabase(fixture.header, first); }, {"first-provider.toml", "second-provider.toml", "Tests.Duplicate"});

        auto declarations = ci::ParseCustomTypeConfig("schema_version=1\n[[types]]\nname='Tests.BindingOwner'\nmethods=[{name='Fetch',binding={function='Unresolved',version=1}}]", "entry-origin.toml");
        std::weak_ptr<const ci::ProviderSource> providerSource = declarations[0].providerSource;
        auto database = ci::BuildTypeDatabase(fixture.header, std::move(declarations));
        Check(!providerSource.expired() && database->methodDeclarations.back().providerSource, "method declarations retain source until entry generation");
        database->encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        ExpectError([&] { ci::CreateMethodTrampolines(*database); }, {"entry-origin.toml", "Tests.BindingOwner.Fetch", "Unresolved"});
        Check(providerSource.expired() && database->methodDeclarations.empty(), "failed entry generation releases source with consumed declarations");
        std::cout << "PASS: declaration origins, field/method/template errors, synthesized shape provenance, duplicate owners and transient source lifetime\n";
    }

    static void TestGenericTemplateSubstitution() {
        Fixture fixture;
        ci::CustomTypeConfig box{
            .name = "Tests.Box`1<System.Diagnostics.Stopwatch>",
            .fields = {{"Value", "System.Diagnostics.Stopwatch"}},
            .genericArguments = {"System.Diagnostics.Stopwatch"},
            .genericDefinition = "Tests.Box`1"};
        auto integerBox = box;
        integerBox.name = "Tests.Box`1<System.Int32>";
        integerBox.fields[0].type = integerBox.genericArguments[0] = "System.Int32";
        ci::CustomTypeConfig similarName{.name = "System.Diagnostics.StopwatchExtra"};
        ci::CustomTypeConfig pair{
            .name = "Tests.Pair`2<System.Diagnostics.Stopwatch,System.Int32>",
            .fields = {{"First", "System.Diagnostics.Stopwatch"}, {"Second", "System.Int32"},
                       {"Nested", box.name}, {"Unchanged", similarName.name}},
            .genericArguments = {"System.Diagnostics.Stopwatch", "System.Int32"},
            .genericDefinition = "Tests.Pair`2"};
        auto source = ci::BuildTypeDatabase(fixture.header, {pair, box, integerBox, similarName});
        auto header = fixture.header;
        source->Publish(header);
        ci::CustomTypeConfig swapped{
            .name = "Tests.Pair`2<System.Int32,System.Diagnostics.Stopwatch>",
            .genericTemplate = pair.name,
            .genericArguments = {"System.Int32", "System.Diagnostics.Stopwatch"}};
        auto result = ci::BuildTypeDatabase(header, {swapped});
        auto* type = result->FindType(swapped.name);
        auto swappedFields = ReadInstanceFields(*result, type->index);
        Check(type && result->GetFullName(swappedFields[0].type) == "System.Int32" &&
                  result->GetFullName(swappedFields[1].type) == "System.Diagnostics.Stopwatch",
              "generic substitution must not replace its own output");
        Check(result->GetFullName(swappedFields[2].type) == integerBox.name &&
                  result->GetFullName(swappedFields[3].type) == similarName.name,
              "generic substitution traverses nested arguments without matching name prefixes");

        pair.name = "Tests.Pair`2<System.Diagnostics.Stopwatch,System.Diagnostics.Stopwatch>";
        pair.genericArguments[1] = "System.Diagnostics.Stopwatch";
        pair.fields[1].type = "System.Diagnostics.Stopwatch";
        auto repeated = ci::BuildTypeDatabase(fixture.header, {pair, box, integerBox, similarName});
        header = fixture.header;
        repeated->Publish(header);
        swapped.genericTemplate = pair.name;
        bool ambiguous = false;
        try { ci::BuildTypeDatabase(header, {swapped}); }
        catch (const std::exception& error) {
            ambiguous = std::string_view(error.what()).contains("ambiguous generic template argument");
        }
        Check(ambiguous, "repeated source argument with different replacements is ambiguous");
        swapped.name = "Tests.Pair`2<System.Int32,System.Int32>";
        swapped.genericArguments[1] = "System.Int32";
        result = ci::BuildTypeDatabase(header, {swapped});
        type = result->FindType(swapped.name);
        swappedFields = ReadInstanceFields(*result, type->index);
        Check(type && swappedFields[0].type == fixture.integer && swappedFields[1].type == fixture.integer,
              "repeated source argument with the same replacement is valid");
        std::cout << "PASS: single-pass generic template substitution and repeated arguments\n";
    }

    void TestClosedTypesMock() {
        Fixture fixture;
        auto AddType = [&](const char* space, const char* name, uint32_t parent, uint32_t kind, uint32_t size) {
            auto index = uint32_t(fixture.types.size());
            fixture.types.push_back({.index = index, .parentTypeIndex = parent, .implIndex = index, .objectType = kind});
            fixture.impls.push_back({.nameOffset = int32_t(fixture.AddString(name)),
                                    .namespaceOffset = int32_t(fixture.AddString(space)), .fieldSize = int32_t(size),
                                    .numNativeEntries = 8});
            return index;
        };
        auto enumeration = AddType("System", "Enum", fixture.object, 1, 0);
        auto value = AddType("System", "DayOfWeek", enumeration, 5, 4);
        fixture.types[value].underlyingTypeIndex = 8;
        auto action = AddType("via.behaviortree", "ActionArg", fixture.object, 1, 0);
        auto virtualVoid = fixture.AddMethod(fixture.voidType, 1);
        fixture.methodImpls[virtualVoid].flags |= 0x40;
        auto* list = reinterpret_cast<ci::ParamList*>(fixture.bytes.data() + fixture.methods[virtualVoid].GetParameterOffset());
        fixture.params[reinterpret_cast<uint32_t*>(list + 1)[0]].type = action;
        fixture.header.numTypes = fixture.types.size(); fixture.header.types = fixture.types.data();
        fixture.header.numTypeImpl = fixture.impls.size(); fixture.header.typeImpls = fixture.impls.data();
        fixture.header.numMethods = fixture.methods.size(); fixture.header.methods = fixture.methods.data();
        fixture.header.numMethodImpl = fixture.methodImpls.size(); fixture.header.methodImpls = fixture.methodImpls.data();
        fixture.header.numParams = fixture.params.size(); fixture.header.params = fixture.params.data();
        fixture.header.numStringPool = fixture.strings.size(); fixture.header.stringPool = fixture.strings.data();
        fixture.header.numBytePool = fixture.bytes.size(); fixture.header.bytePool = fixture.bytes.data();
        TestClosedTypes(fixture.header);
    }

    void TestFinalFields() {
        Fixture fixture;
        // 预留一项真正的原版 class；正常时是旁支，负例再将其挂到扩展目标下。
        auto originalChild = uint32_t(fixture.types.size());
        fixture.types.push_back({.index = originalChild, .parentTypeIndex = fixture.object, .implIndex = originalChild, .objectType = 1});
        fixture.impls.push_back({.nameOffset = int32_t(fixture.AddString("OriginalChild")), .namespaceOffset = int32_t(fixture.AddString("Tests"))});
        fixture.header.numTypes = uint32_t(fixture.types.size());
        fixture.header.typesStartOfGenericsProbably = fixture.header.numTypes;
        fixture.header.types = fixture.types.data();
        fixture.header.numTypeImpl = uint32_t(fixture.impls.size());
        fixture.header.typeImpls = fixture.impls.data();
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        fixture.header.stringPool = fixture.strings.data();
        auto baseline = ci::BuildTypeDatabase(fixture.header, {});
        auto index = baseline->ResolveType("System.Diagnostics.Stopwatch");
        auto& original = fixture.types[index];
        auto& originalImpl = fixture.impls[original.implIndex];
        original.typeFlags = 0x41100101; // Final + 原 compact arena 分配位。
        original.parentTypeIndex = fixture.object;
        original.defaultCtorIndex = fixture.types[fixture.object].defaultCtorIndex;
        original.memberFieldIndex = fixture.fields.size();
        originalImpl.fieldSize = 16;
        originalImpl.numMemberFields = 3;
        for (auto [name, type, offset, flags] :
             {std::tuple{"OriginalPeer", fixture.object, 0u, 6u}, std::tuple{"OriginalNumber", fixture.integer, 8u, 6u},
              std::tuple{"OriginalStatic", fixture.integer, 0u, 0x16u}}) {
            fixture.fields.push_back({.declaringType = index, .impl = fixture.fieldImpls.size(), .type = type});
            fixture.fieldImpls.push_back({.flags = uint16_t(flags), .offset = offset, .name = fixture.AddString(name)});
        }
        uint64_t originalBits = 1;
        std::memcpy(reinterpret_cast<uint8_t*>(&originalImpl) + 0x20, &originalBits, 8);
        std::memcpy(reinterpret_cast<uint8_t*>(&originalImpl) + 0x28, &originalBits, 8);
        fixture.header.numFields = uint32_t(fixture.fields.size());
        fixture.header.numFieldImpl = uint32_t(fixture.fieldImpls.size());
        fixture.header.fields = fixture.fields.data();
        fixture.header.fieldImpls = fixture.fieldImpls.data();
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        fixture.header.stringPool = fixture.strings.data();

        auto specs = ci::ParseCustomTypeConfig(R"toml(schema_version = 1
[[types]]
name = "System.Diagnostics.Stopwatch"
extend = true
base = "Tests.UnusedParent"
abstract = true
methods = [{ name = "Unused" }]
fields = [{ name = "Counter", type = "System.Int32" }, { name = "Peer", type = "System.Object" }]
)toml",
                                           "final_fields.toml");
        Check(specs.size() == 1 && specs[0].extend && specs[0].methods.empty(), "extension has no generated ctor");
        auto database = ci::BuildTypeDatabase(fixture.header, specs);
        auto& definition = database->types[index];
        auto& impl = database->typeImpls[definition.implIndex];
        Check(database->types.size() == fixture.types.size() && database->customTypes.empty() &&
                  database->customMethods.empty() && database->extensions.size() == 1 &&
                  database->extensions[0].index == index && database->extensions[0].originalFieldSize == 16,
              "extension preserves original type identity and method count");
        Check(definition.parentTypeIndex == original.parentTypeIndex &&
                  definition.defaultCtorIndex == original.defaultCtorIndex && definition.fqnHash == original.fqnHash &&
                  definition.typeCrc == original.typeCrc && definition.typeFlags == 0x40100101,
              "extension preserves identity, constructor and flags except arena allocation");
        Check(definition.implIndex != original.implIndex && impl.fieldSize == 32 && impl.numMemberFields == 5,
              "extension uses a private impl with total field size");
        Check(std::memcmp(database->fields.data(), fixture.fields.data(),
                          fixture.fields.size() * sizeof(ci::FieldDef)) == 0 &&
                  std::memcmp(database->fieldImpls.data(), fixture.fieldImpls.data(),
                              fixture.fieldImpls.size() * sizeof(ci::FieldImpl)) == 0 &&
                  std::memcmp(database->typeImpls.data(), fixture.impls.data(),
                              fixture.impls.size() * sizeof(RE::RETypeImpl)) == 0,
              "original field IDs, offsets and shared impls remain unchanged");
        for (size_t i = 0; i < originalImpl.numMemberFields; ++i) {
            Check(std::memcmp(&database->fields[definition.memberFieldIndex + i],
                              &fixture.fields[original.memberFieldIndex + i], sizeof(ci::FieldDef)) == 0,
                  "copied original member descriptor");
        }
        auto counter = database->fields[definition.memberFieldIndex + 3];
        auto peer = database->fields[definition.memberFieldIndex + 4];
        Check(database->fieldImpls[counter.impl].offset == 16 && database->fieldImpls[peer.impl].offset == 24,
              "extension fields start after this class, not its parent");
        for (size_t offset : {0x20, 0x28}) {
            uint64_t bits;
            std::memcpy(&bits, reinterpret_cast<uint8_t*>(&impl) + offset, 8);
            Check(bits == 9, "extension preserves and expands reference maps");
        }
        auto published = fixture.header;
        database->Publish(published);
        Check(published.numTypes == fixture.header.numTypes && published.numMethods == fixture.header.numMethods &&
                  published.numTypeImpl == fixture.header.numTypeImpl + 1 &&
                  published.numFields == fixture.header.numFields + 5,
              "extension-only publication");

        auto mixed = specs;
        mixed[0].fields[1].type = "Tests.ExtensionReference";
        mixed.push_back({.name = "Tests.ExtensionReference"});
        auto combined = ci::BuildTypeDatabase(fixture.header, mixed);
        Check(combined->FindType("Tests.ExtensionReference")->index == fixture.header.numTypes &&
                  combined->ResolveType(specs[0].name) == index,
              "extension references a new class without shifting custom type IDs");
        auto repeated = specs;
        repeated.push_back({.name = specs[0].name, .fields = {{"Another", "System.Object"}}, .extend = true});
        auto merged = ci::BuildTypeDatabase(fixture.header, repeated);
        auto& mergedImpl = merged->typeImpls[merged->types[index].implIndex];
        Check(mergedImpl.fieldSize == 40 && mergedImpl.numMemberFields == 6 && merged->extensions.size() == 1 &&
                  merged->extensions[0].originalFieldSize == 16,
              "multiple providers append to one final class");

        // 两种非 sealed flags 都应通过；仍只清 compact arena 位，不伪造 sealed。
        for (auto flags : {0x41100001u, 0x100001u}) {
            original.typeFlags = flags;
            auto leaf = ci::BuildTypeDatabase(fixture.header, specs);
            auto& leafDefinition = leaf->types[index];
            auto& leafImpl = leaf->typeImpls[leafDefinition.implIndex];
            Check(leafDefinition.typeFlags == (flags & ~0x01000000u) && leafDefinition.parentTypeIndex == original.parentTypeIndex && leafDefinition.defaultCtorIndex == original.defaultCtorIndex,
                  "non-sealed leaf preserves flags, parent and constructor");
            Check(leaf->types.size() == fixture.types.size() && leaf->customMethods.empty() && leafImpl.fieldSize == 32 && leafImpl.numMemberFields == 5 && leafDefinition.implIndex != original.implIndex,
                  "non-sealed leaf uses the original type ID and a private extended impl");
            Check(std::memcmp(leaf->fields.data(), fixture.fields.data(), fixture.fields.size() * sizeof(ci::FieldDef)) == 0 &&
                      std::memcmp(leaf->fieldImpls.data(), fixture.fieldImpls.data(), fixture.fieldImpls.size() * sizeof(ci::FieldImpl)) == 0 &&
                      std::memcmp(leaf->typeImpls.data(), fixture.impls.data(), fixture.impls.size() * sizeof(RE::RETypeImpl)) == 0,
                  "non-sealed extension preserves old field IDs, offsets and impls");
            Check(leaf->fieldImpls[leaf->fields[leafDefinition.memberFieldIndex + 3].impl].offset == 16 &&
                      leaf->fieldImpls[leaf->fields[leafDefinition.memberFieldIndex + 4].impl].offset == 24,
                  "non-sealed leaf appends after the original field block");
            for (size_t offset : {0x20, 0x28}) {
                uint64_t bits;
                std::memcpy(&bits, reinterpret_cast<uint8_t*>(&leafImpl) + offset, 8);
                Check(bits == 9, "non-sealed leaf preserves and expands both reference maps");
            }
        }

        // 声明故意逆序：新孙类/子类仍应在父类扩展完成后，按拓扑生成无重叠布局。
        auto descendants = ci::ParseCustomTypeConfig(R"toml(schema_version = 1
[[types]]
name = "Tests.LeafGrandchild"
base = "Tests.LeafDerived"
fields = [{ name = "GrandCounter", type = "System.Int32" }]
[[types]]
name = "Tests.LeafDerived"
base = "System.Diagnostics.Stopwatch"
fields = [{ name = "OwnPeer", type = "System.Object" }]
)toml", "leaf_derived.toml");
        descendants.insert(descendants.end(), specs.begin(), specs.end());
        auto derivedRegistry = ci::BuildTypeDatabase(fixture.header, descendants);
        auto* derived = derivedRegistry->FindType("Tests.LeafDerived");
        auto* grandchild = derivedRegistry->FindType("Tests.LeafGrandchild");
        Check(derived && grandchild && derived->parent == index && grandchild->parent == derived->index,
              "new derived classes keep the extended original parent chain");
        auto& derivedImpl = derivedRegistry->typeImpls[derivedRegistry->types[derived->index].implIndex];
        auto& grandchildImpl = derivedRegistry->typeImpls[derivedRegistry->types[grandchild->index].implIndex];
        Check(derivedImpl.fieldSize == 40 && grandchildImpl.fieldSize == 48 &&
                  derivedRegistry->fieldImpls[derivedRegistry->GetFieldDef(&derivedRegistry->types[derived->index], "OriginalNumber")->impl].offset == 8 &&
                  derivedRegistry->fieldImpls[derivedRegistry->GetFieldDef(&derivedRegistry->types[derived->index], "Peer")->impl].offset == 24 &&
                  derivedRegistry->fieldImpls[derivedRegistry->GetFieldDef(&derivedRegistry->types[derived->index], "OwnPeer")->impl].offset == 32 &&
                  derivedRegistry->fieldImpls[derivedRegistry->GetFieldDef(&derivedRegistry->types[grandchild->index], "GrandCounter")->impl].offset == 40,
              "new descendants append after the complete extended parent layout");
        for (auto* value : {&derivedImpl, &grandchildImpl}) {
            for (size_t offset : {0x20, 0x28}) {
                uint64_t bits;
                std::memcpy(&bits, reinterpret_cast<uint8_t*>(value) + offset, 8);
                Check(bits == 25, "new descendants inherit old and extended reference maps");
            }
        }

        fixture.types[originalChild].parentTypeIndex = index;
        std::string rejection;
        try {
            ci::BuildTypeDatabase(fixture.header, specs);
        } catch (const std::exception& error) {
            rejection = error.what();
        }
        Check(rejection.find("existing derived layout") != std::string::npos && rejection.find(specs[0].name) != std::string::npos && rejection.find("Tests.OriginalChild") != std::string::npos,
              "existing child rejects extension and identifies both conflicting types");
        Check(original.typeFlags == 0x100001 && originalImpl.fieldSize == 16 && originalImpl.numMemberFields == 3,
              "rejected extension leaves original metadata untouched");
        fixture.types[originalChild].parentTypeIndex = fixture.object;

        for (auto badFlags : {0x51100101u, 0x49100101u, 0x41100181u, 0x41100021u, 0x411000A1u, 0x49100001u, 0x51100001u}) {
            original.typeFlags = badFlags;
            Rejects([&] { ci::BuildTypeDatabase(fixture.header, specs); });
        }
        original.typeFlags = 0x41100101;
        original.objectType = 5;
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, specs); });
        original.objectType = 1;
        for (auto name : {"OriginalPeer", "OriginalNumber", "OriginalStatic"}) {
            auto duplicate = specs;
            duplicate[0].fields[0].name = name;
            Rejects([&] { ci::BuildTypeDatabase(fixture.header, duplicate); });
        }
        originalImpl.fieldSize = 512;
        originalBits = uint64_t(1) << 63;
        fixture.fieldImpls[fixture.fields[original.memberFieldIndex].impl].offset = 504;
        for (size_t offset : {0x20, 0x28}) {
            std::memcpy(reinterpret_cast<uint8_t*>(&originalImpl) + offset, &originalBits, 8);
        }
        auto large = ci::BuildTypeDatabase(fixture.header, specs);
        auto& largeImpl = large->typeImpls[large->types[index].implIndex];
        for (size_t offset : {0x20, 0x28}) {
            const uint64_t* bits;
            std::memcpy(&bits, reinterpret_cast<uint8_t*>(&largeImpl) + offset, 8);
            Check(largeImpl.fieldSize == 528 && bits[0] == originalBits && bits[1] == 2,
                  "extension crosses inline bitmap boundary");
        }
        std::array<uint64_t, 2> oldMap{originalBits, 1};
        originalImpl.fieldSize = 520;
        auto address = uintptr_t(oldMap.data());
        for (size_t offset : {0x20, 0x28}) {
            std::memcpy(reinterpret_cast<uint8_t*>(&originalImpl) + offset, &address, 8);
        }
        auto external = ci::BuildTypeDatabase(fixture.header, specs);
        auto& externalImpl = external->typeImpls[external->types[index].implIndex];
        const uint64_t* newMap;
        std::memcpy(&newMap, reinterpret_cast<uint8_t*>(&externalImpl) + 0x20, 8);
        Check(newMap != oldMap.data() && newMap[0] == originalBits && newMap[1] == 5 && oldMap[1] == 1,
              "extension copies existing external bitmap storage");
        std::cout << "PASS: original sealed/leaf fields, identity/IDs, private impl, static fields, reference maps, "
                     "new descendants, existing-child diagnostics, 512-byte transition, mixed declarations, repeated extensions, unsupported targets\n";
    }
    void TestParameterizedConstructors() {
        Fixture fixture;
        const auto recipeIndex = fixture.AddMethod(fixture.voidType, 6);
        const auto recipeOffset = fixture.methods[recipeIndex].GetParameterOffset();
        const auto expectedInvoke = reinterpret_cast<const ci::ParamList*>(fixture.bytes.data() + recipeOffset)->invokeId;
        fixture.header.numStringPool = uint32_t(fixture.strings.size());
        fixture.header.stringPool = fixture.strings.data();
        fixture.header.numMethods = uint32_t(fixture.methods.size());
        fixture.header.numMethodImpl = uint32_t(fixture.methodImpls.size());
        fixture.header.numParams = uint32_t(fixture.params.size());
        fixture.header.numBytePool = uint32_t(fixture.bytes.size());
        fixture.header.methods = fixture.methods.data();
        fixture.header.methodImpls = fixture.methodImpls.data();
        fixture.header.params = fixture.params.data();
        fixture.header.bytePool = fixture.bytes.data();
        static int parentCalls;
        static void* receivedContext;
        static void* receivedSelf;
        parentCalls = 0;
        using FnCtor = void (*)(void* vmContext, void* object);
        static const FnCtor parentCtor = [](void* vmctx, void* self) {
            ++parentCalls;
            receivedContext = vmctx;
            receivedSelf = self;
        };
        const auto sixCtor = +[](void* vmctx, void* self, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
            parentCtor(vmctx, self); // 父调用是业务实现，不是 host 的副作用。
            auto* values = static_cast<int32_t*>(self);
            std::array<int32_t, 6> arguments{a, b, c, d, e, f};
            std::copy(arguments.begin(), arguments.end(), values);
        };
        ci::MethodSpec noArgs{.name = ".ctor", .kind = ci::MethodKind::Constructor,
            .binding = {.backend = ci::Backend::Cpp, .address = reinterpret_cast<void*>(parentCtor)}};
        ci::MethodSpec sixArgs{.name = ".ctor", .kind = ci::MethodKind::Constructor,
            .parameters = {"a", "b", "c", "d", "e", "f"},
            .binding = {.backend = ci::Backend::Cpp, .address = reinterpret_cast<void*>(sixCtor)}};
        const std::vector<ci::CustomTypeConfig> specs{
            {.name = "Tests.ParameterizedBase", .methods = {noArgs}},
            {.name = "Tests.Parameterized", .base = "Tests.ParameterizedBase", .fields = {{"Sentinel"}}, .methods = {noArgs, sixArgs}},
            {.name = "Tests.ParameterizedOnly", .base = "Tests.ParameterizedBase", .methods = {sixArgs}}};
        auto previous = std::move(ci::g_database);
        ci::g_database = ci::BuildTypeDatabase(fixture.header, specs);
        auto& database = *ci::g_database;
        database.encodingBase = uintptr_t(parentCtor) - 0x1000;
        ci::CreateMethodTrampolines(database);
        ci::PrepareInvokeAdapters(database);
        const auto* type = QueryType(database, "Tests.Parameterized");
        const auto* only = QueryType(database, "Tests.ParameterizedOnly");
        Check(type && only && type->defaultCtorIndex && !only->defaultCtorIndex, "only parameterless ctor becomes the default");
        const auto signature = ".ctor(System.Int32, System.Int32, System.Int32, System.Int32, System.Int32, System.Int32)";
        auto* entry = QueryMethod(database, type, signature);
        const auto* code = static_cast<const uint8_t*>(entry);
        Check(code && code[0] == 0xFF && code[1] == 0x25, "constructor entry is an ordinary native tail jump");
        Check(QueryMethod(database, only, signature) && !QueryMethod(database, only, ".ctor()"),
              "constructors are not inherited during query");
        const auto* method = database.GetMethodDef(type, signature);
        const auto* list = reinterpret_cast<const ci::ParamList*>(database.bytes.data() + method->GetParameterOffset());
        Check(list->count == 6 && list->invokeId == expectedInvoke, "constructor selects its six-parameter invoke recipe, not Object.ctor");
        std::array<int32_t, 8> values{};
        values.fill(int32_t(0xCCCCCCCC));
        uint64_t contextMarker{};
        using FnSixCtor = void (*)(void* vmContext, void* object, int32_t firstValue, int32_t secondValue,
                                   int32_t thirdValue, int32_t fourthValue, int32_t fifthValue, int32_t sixthValue);
        reinterpret_cast<FnSixCtor>(entry)(&contextMarker, values.data(), 7, -11, 13, -17, 19, -23);
        Check(parentCalls == 1 && receivedContext == &contextMarker && receivedSelf == values.data(), "provider alone controls parent construction and receives original ctx/self");
        Check(values == std::array<int32_t, 8>{7, -11, 13, -17, 19, -23, int32_t(0xCCCCCCCC), int32_t(0xCCCCCCCC)}, "all register/stack parameters preserved without host clearing");
        auto reversed = specs;
        std::ranges::reverse(reversed[1].methods);
        auto reordered = ci::BuildTypeDatabase(fixture.header, reversed);
        const auto* reorderedType = QueryType(reordered, "Tests.Parameterized");
        Check(reorderedType->defaultCtorIndex == reordered->GetMethodDef(reorderedType, ".ctor()") - reordered->methods.data(), "default constructor index is independent of overload order");
        auto invalid = specs;
        invalid[1].methods[1].binding = {};
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, invalid); });
        invalid = specs;
        auto refOnly = sixArgs;
        refOnly.parameters[0].modifier = ci::ParameterModifier::Ref;
        invalid[1].methods.push_back(refOnly);
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, invalid); });
        invalid = specs;
        invalid.push_back({.name = "Tests.InvalidDefault", .base = "Tests.ParameterizedOnly", .methods = {{.name = ".ctor", .kind = ci::MethodKind::Constructor}}});
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, invalid); });
        ci::g_database = std::move(previous);
        std::cout << "PASS: explicit constructors, six parameters, provider-owned parent call, no host memset, exact invoke recipe, overload order, no inherited constructors and negative declarations\n";
    }
} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--registration-only") {
            TestPluginRegistration();
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--provider-registration-only") {
            const auto source = std::filesystem::absolute(argv[2]);
            std::ifstream stream(source, std::ios::binary);
            Check(bool(stream), "provider TOML is readable");
            const std::string description{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            auto plugin = ci::ParseProviderConfig(description, source);
            Check(!plugin.types.empty() && ci::LoadClassProviderBindings(plugin), "merged provider declarations and function versions");
            const auto module = GetModuleHandleW(plugin.dllPath.c_str());
            Check(module && GetProcAddress(module, "ci_register_provider") &&
                      GetProcAddress(module, "ci_on_initialize") &&
                      GetProcAddress(module, "reframework_plugin_initialize") &&
                      GetProcAddress(module, "reframework_plugin_required_version"),
                  "same DLL provides CI and REFramework entries");
            Check(!GetProcAddress(module, "RiseFrameworkSetMethodDispatch"), "retired cross-DLL dispatch export is absent");
            const auto reused = LoadLibraryExW(plugin.dllPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            Check(reused == module, "later load of the same plugin path reuses HMODULE");
            FreeLibrary(reused);
            std::cout << "PASS: merged DLL CI/RF exports, " << plugin.types.size() << " type declarations, function binding versions and HMODULE reuse\n";
            return 0;
        }
        TestEmbeddedReferenceBitmaps();
        TestTrampolineOwnership();
        TestMethodCodeOwnership();
        TestClassVirtualTable();
        TestTdbSnapshot();
        TestDeclarationTypes();
        TestAuthorNameBoundaries();
        TestProviderSources();
        TestGenericTemplateSubstitution();
        TestRuntimeInitializationCallHook();
        TestRuntimeReadyNotification();
        TestQueries();
        reframework::API::initialize(GetMockSdk());
        TestPatternScan();
        TestPluginRegistration();
        TestSerializedFields();
        TestOutParameters();
        TestRefParameters();
        TestClosedTypesMock();
        TestOverrideSpecialName();
        TestFinalFields();
        TestParameterizedConstructors();
        Check(ci::NameHash("System.Object") == 3218255520u, "engine hash mismatch");
        auto declarations = ci::LoadClassProviders({std::filesystem::path(CI_TEST_MANIFESTS) / "basic"});
        Check(declarations.size() == 2, "TOML class count");
        Check(declarations[0].methods[0].binding.backend == ci::Backend::Cpp, "cpp binding");
        Check(declarations[1].methods[1].binding.backend == ci::Backend::Cpp, "cpp override binding");
        auto provider = LoadLibraryW(RE::widen(CI_TEST_TYPE_PLUGIN).c_str());
        Check(provider != nullptr, "type plugin load");
        Check(GetProcAddress(provider, "ci_register_provider") != nullptr, "registration bridge export");
        Check(GetProcAddress(provider, "Add") == nullptr, "business functions must not be exported");
        auto pluginDeclarations = ci::LoadClassProviders({std::filesystem::path(CI_TEST_MANIFESTS) / "types"});
        const auto orderedPluginDeclarations = OrderDeclarationsForTest(pluginDeclarations);
        Check(orderedPluginDeclarations.size() == 2 && orderedPluginDeclarations[1].base == orderedPluginDeclarations[0].name,
              "type plugin inheritance");
        Check(pluginDeclarations[0].methods[0].binding.address != nullptr, "registered binding address");
        Fixture pluginFixture;
        auto pluginRegistry = ci::BuildTypeDatabase(pluginFixture.header, pluginDeclarations);
        Check(pluginRegistry->customTypes.size() == 2, "type plugin metadata expansion");
        Fixture fixture;
        auto database = ci::BuildTypeDatabase(fixture.header, declarations);
        Check(database->types.size() == 7, "type expansion");
        Check(std::memcmp(database->types.data(), fixture.types.data(),
                          fixture.types.size() * sizeof(RE::RETypeDefinition)) == 0,
              "original IDs/records changed");
        auto& base = database->customTypes[0];
        auto& derived = database->customTypes[1];
        Check(derived.parent == base.index && base.parent == fixture.object, "parent relation");
        const auto baseFields = ReadInstanceFields(*database, base.index);
        const auto derivedFields = ReadInstanceFields(*database, derived.index);
        Check(baseFields[0].offset == 0 && derivedFields[0].offset == 0 && derivedFields[1].offset == 8,
              "inherited field offsets");
        Check(database->typeImpls[database->types[derived.index].implIndex].fieldSize == 16,
              "inherited total field size");
        Check(database->types[derived.index].typeFlags == 0x00100001,
              "custom type must use ordinary heap allocation, not the prototype arena");
        Check(base.virtualMethods.size() == 5 && derived.virtualMethods.size() == 5, "override created an extra slot");
        auto* baseVirtual = database->FindMethod(base.virtualMethods[4]);
        auto* derivedVirtual = database->FindMethod(derived.virtualMethods[4]);
        Check(database->methodImpls[database->methods[baseVirtual->index].impl].virtualIndex == 4 &&
                  database->methodImpls[database->methods[derivedVirtual->index].impl].virtualIndex == 4 &&
                  baseVirtual->index != derivedVirtual->index,
              "slot identity");
        auto& add = database->customMethods[1];
        auto offset = database->methods[add.index].GetParameterOffset();
        auto* list = reinterpret_cast<ci::ParamList*>(database->bytes.data() + offset);
        auto* ids = reinterpret_cast<uint32_t*>(list + 1);
        Check(list->count == 1 && database->parameters[ids[0]].type == fixture.integer, "parameter list expansion");
        auto published = fixture.header;
        database->Publish(published);
        Check(published.numTypes == 7 &&
                  published.typesStartOfGenericsProbably == fixture.header.typesStartOfGenericsProbably,
              "partition boundary changed");
        Check(published.types == database->types.data() && published.bytePool == database->bytes.data(),
              "table publication");
        Check(database->FindMethod(derivedVirtual->index) == derivedVirtual, "method lookup after publication");
        Check(database->FindMethod(fixture.types[fixture.object].defaultCtorIndex) == nullptr,
              "original method lookup");
        Check(database->FindType(base.name) == &base && database->FindType("System.Object") == nullptr,
              "custom type lookup");
        Check(database->fieldImpls[database->GetFieldDef(&database->types[derived.index], "Counter")->impl].offset == 0 &&
                  database->GetFieldDef(&database->types[fixture.object], "Counter") == nullptr,
              "final inherited field declaration lookup");
        std::array<uintptr_t, 17> typeInfo{};
        typeInfo[12] = uintptr_t(&fixture.types[fixture.object]);
        database->types[fixture.object].typeInfo = typeInfo.data();
        auto republished = fixture.header;
        database->Publish(republished);
        Check(typeInfo[12] == uintptr_t(&database->types[fixture.object]), "TypeInfo back pointer rebasing");
        database->types[fixture.object].typeInfo = nullptr;

        auto bitmapFixture = fixture.header;
        bitmapFixture.typeImpls[1].fieldSize = 0x208;
        auto bitmap = uintptr_t(bitmapFixture.bytePool) + 4;
        std::memcpy(reinterpret_cast<uint8_t*>(&bitmapFixture.typeImpls[1]) + 0x20, &bitmap, 8);
        auto bitmapCopy = ci::BuildTypeDatabase(bitmapFixture, declarations);
        uintptr_t rebased;
        std::memcpy(&rebased, reinterpret_cast<uint8_t*>(&bitmapCopy->typeImpls[1]) + 0x20, 8);
        Check(rebased == uintptr_t(bitmapCopy->bytes.data()) + 4, "external bitmap rebasing");
        bitmapFixture.typeImpls[1].fieldSize = 0;
        std::memset(reinterpret_cast<uint8_t*>(&bitmapFixture.typeImpls[1]) + 0x20, 0, 8);
        auto reversed = declarations;
        std::ranges::reverse(reversed);
        Check(OrderDeclarationsForTest(reversed)[0].name == base.name, "inheritance ordering");
        reversed[0].base = reversed[1].name;
        reversed[1].base = reversed[0].name;
        Rejects([&] { OrderDeclarationsForTest(reversed); });
        auto badSignature = declarations;
        badSignature[1].methods[1].parameters.push_back("bad");
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, badSignature); });
        auto duplicateField = declarations;
        duplicateField[1].fields.push_back({.name = "Counter"});
        Rejects([&] { ci::BuildTypeDatabase(fixture.header, duplicateField); });

        ci::CustomTypeConfig references{
            .name = "Tests.References",
            .fields = {{"Number", "System.Int32"}, {"Object", "System.Object"}, {"Peer", "Tests.References"}}};
        ci::CustomTypeConfig childReferences{
            .name = "Tests.ChildReferences", .base = references.name, .fields = {{"Another", "System.Object"}}};
        auto referenceMetadata = ci::BuildTypeDatabase(fixture.header, {references, childReferences});
        auto& referenceBase = referenceMetadata->customTypes[0];
        auto& referenceChild = referenceMetadata->customTypes[1];
        auto& referenceImpl = referenceMetadata->typeImpls[referenceMetadata->types[referenceBase.index].implIndex];
        auto& childImpl = referenceMetadata->typeImpls[referenceMetadata->types[referenceChild.index].implIndex];
        uint64_t bits, childBits;
        std::memcpy(&bits, reinterpret_cast<uint8_t*>(&referenceImpl) + 0x20, 8);
        std::memcpy(&childBits, reinterpret_cast<uint8_t*>(&childImpl) + 0x20, 8);
        const auto referenceBaseFields = ReadInstanceFields(*referenceMetadata, referenceBase.index);
        const auto referenceChildFields = ReadInstanceFields(*referenceMetadata, referenceChild.index);
        Check(referenceBaseFields[1].offset == 8 && referenceBaseFields[2].offset == 16 &&
                  referenceImpl.fieldSize == 24 && bits == 6,
              "reference field alignment/map");
        Check(referenceChildFields[3].offset == 24 && childBits == 14, "inherited reference map");
        ci::CustomTypeConfig large{.name = "Tests.Large"};
        for (int i = 0; i < 65; ++i) {
            large.fields.push_back({"Reference" + std::to_string(i), "System.Object"});
        }
        auto largeMetadata = ci::BuildTypeDatabase(fixture.header, {large});
        auto& largeImpl = largeMetadata->typeImpls[largeMetadata->types[largeMetadata->customTypes[0].index].implIndex];
        uint64_t* largeMap;
        std::memcpy(&largeMap, reinterpret_cast<uint8_t*>(&largeImpl) + 0x20, 8);
        Check(largeImpl.fieldSize == 520 && largeMap[0] == UINT64_MAX && largeMap[1] == 1, "external reference bitmap");
        TestMotionSelectionInheritance(fixture.header);

        // A read-only snapshot of the running TDB exercises real nested types,
        // generic descriptors, inherited engine fields and actual invoke IDs.
        if (argc > 1) {
            std::ifstream file(argv[1], std::ios::binary);
            RE::TDB header{};
            std::vector<std::vector<uint8_t>> tables;
            LoadTdbSnapshot(file, header, tables);
            TestFiveArgumentInvoker(header);
            TestInvokeSignatures(header);
            TestFloatingReturnMetadata(header);
            TestExportRef(header);
            TestClosedTypes(header);
            TestOriginalAccessorSpecialName(header);
            TestReferenceArrays(header);
            TestMotionSelectionInheritance(header);
            if (argc > 2) TestAdditionalProvider(header, argv[2], argc > 3 ? argv[3] : nullptr);
            std::ifstream extensionFile(std::filesystem::path(CI_EXAMPLES_DIR) / "../tests/final_fields.toml");
            std::string extensionText{std::istreambuf_iterator<char>(extensionFile), std::istreambuf_iterator<char>()};
            auto extensions = ci::ParseCustomTypeConfig(extensionText, "final_fields.toml");
            extensions.push_back({.name = "app.cHunterWp03Handling", .fields = {{"CILeafCounter", "System.Int32"}, {"CILeafPeer", "System.Object"}}, .extend = true});
            auto extended = ci::BuildTypeDatabase(header, extensions);
            Check(extended->types.size() == header.numTypes && extended->extensions.size() == 3,
                  "real TDB extension-only type identity");
            for (auto& entry : extended->extensions) {
                auto& old = header.types[entry.index];
                auto& definition = extended->types[entry.index];
                auto oldCount = header.typeImpls[old.implIndex].numMemberFields;
                auto& impl = extended->typeImpls[definition.implIndex];
                Check(impl.numMemberFields == oldCount + 2 && definition.defaultCtorIndex == old.defaultCtorIndex,
                      "real final member count and constructor identity");
                Check(definition.typeFlags == (old.typeFlags & ~0x01000000u), "real extension preserves flags except arena allocation");
                for (size_t i = 0; i < oldCount; ++i) {
                    Check(std::memcmp(&extended->fields[definition.memberFieldIndex + i],
                                      static_cast<ci::FieldDef*>(header.fields) + old.memberFieldIndex + i,
                                      sizeof(ci::FieldDef)) == 0,
                          "real final original member descriptors");
                }
                Check(extended->fieldImpls[extended->fields[definition.memberFieldIndex + oldCount].impl].offset >=
                          entry.originalFieldSize,
                      "real final appended field offset");
            }
            auto handling = extended->ResolveType("app.cHunterWp03Handling");
            Check(!(header.types[handling].typeFlags & 0x100), "real handling is a non-sealed leaf");
            auto oldHandlingSize = header.typeImpls[header.types[handling].implIndex].fieldSize;
            auto newHandlingSize = extended->typeImpls[extended->types[handling].implIndex].fieldSize;
            std::cout << "PASS: real TDB sealed/leaf managed field extensions; app.cHunterWp03Handling fieldSize=" << oldHandlingSize << " -> " << newHandlingSize << '\n';
            auto classProvider = LoadLibraryW(RE::widen(CI_TEST_CLASS_PLUGIN).c_str());
            Check(classProvider != nullptr, "class test provider load");
            std::vector<ci::ClassProviderNotification> callbacks;
            auto classDeclarations = ci::LoadClassProviders({std::filesystem::path(CI_TEST_MANIFESTS) / "class"}, &callbacks);
            auto classMetadata = ci::BuildTypeDatabase(header, classDeclarations);
            const auto expectedClassMethods = classMetadata->methodDeclarations;
            auto* arg = classMetadata->FindType("ClassInjector.Tests.Arg");
            auto* cmd = classMetadata->FindType("ClassInjector.Tests.Cmd");
            auto* generic = classMetadata->FindType("ace.btable.cCommandAction`1<ClassInjector.Tests.Arg>");
            auto* behavior = classMetadata->FindType("ClassInjector.Tests.PlayerBehavior");
            auto* valueFields = classMetadata->FindType("ClassInjector.Tests.ValueFields");
            Check(valueFields != nullptr, "ValueType field test declaration");
            for (auto field : {"CIPosition", "CIOffset"}) {
                auto* data = classMetadata->GetFieldDef(&classMetadata->types[valueFields->index], field);
                Check(data && !classMetadata->IsReferenceType(uint32_t(data->type)) && classMetadata->fieldImpls[data->impl].offset % 16 == 0, "SIMD ValueType field alignment");
                Check(classMetadata->typeImpls[classMetadata->types[data->type].implIndex].fieldSize == 16,
                      "vec3 inline storage excludes the boxed object header");
            }
            for (auto& extension : classMetadata->extensions) {
                auto& target = classMetadata->types[extension.index];
                auto count = classMetadata->typeImpls[target.implIndex].numMemberFields;
                for (auto i = 0u; i < count; ++i) {
                    auto& field = classMetadata->fields[target.memberFieldIndex + i];
                    if (classMetadata->GetFullName(uint32_t(field.type)) == "via.vec3") {
                        Check(classMetadata->fieldImpls[field.impl].offset % 16 == 0, "extended SIMD field alignment");
                    }
                }
            }
            Check(behavior && behavior->parent == classMetadata->ResolveType("app.AppBehavior") &&
                      behavior->virtualMethods.size() == 19,
                  "AppBehavior hierarchy and inherited lifecycle slot count");
            for (auto [name, slot] : {std::pair{"doAwake", 16}, {"doStart", 17}, {"doOnDestroy", 18},
                                      {"update", 15}, {"lateUpdate", 6}}) {
                auto* method = classMetadata->FindMethod(behavior->virtualMethods[slot]);
                Check(method && expectedClassMethods[method->index - classMetadata->originalMethodCount].name == name &&
                          classMetadata->methodImpls[classMetadata->methods[method->index].impl].virtualIndex == slot,
                      "component lifecycle override must retain the engine slot");
            }
            auto invalidBehavior = classDeclarations;
            auto& invalidType = *std::ranges::find(invalidBehavior, behavior->name, &ci::CustomTypeConfig::name);
            std::ranges::find(invalidType.methods, "doAwake", &ci::MethodSpec::name)->name = "awake";
            Rejects([&] { ci::BuildTypeDatabase(header, invalidBehavior); });
            Check(arg && cmd && generic && generic->genericDefinition && cmd->parent == generic->index,
                  "real generic hierarchy");
            Check(classMetadata->GetFullName(generic->index) == generic->name, "real generic type identity");
            const auto argFields = ReadInstanceFields(*classMetadata, arg->index);
            const auto cmdFields = ReadInstanceFields(*classMetadata, cmd->index);
            Check(argFields[1].offset == 8 && argFields[2].reference &&
                      cmdFields[0].name == "<Tag>k__BackingField",
                  "existing fields and reference layout");
            auto& argImpl = classMetadata->typeImpls[classMetadata->types[arg->index].implIndex];
            std::memcpy(&bits, reinterpret_cast<uint8_t*>(&argImpl) + 32, 8);
            Check(bits == 126, "real Arg reference map");
            classMetadata->encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
            ci::CreateMethodTrampolines(*classMetadata);
            ci::PrepareInvokeAdapters(*classMetadata);
            auto getQueries = reinterpret_cast<FnGetEarlyQueryProbe>(GetProcAddress(classProvider, "GetEarlyQueryProbe"));
            Check(getQueries && getQueries()->notifications == 0 && !getQueries()->sdkInitialized,
                  "registration alone does not notify or initialize provider SDK");
            auto previous = std::move(ci::g_database);
            ci::g_database = std::move(classMetadata);
            ci::NotifyClassProviders(callbacks, ci::GetContext());
            ci::NotifyClassProviders(callbacks, ci::GetContext());
            classMetadata = std::move(ci::g_database);
            ci::g_database = std::move(previous);
            auto& queries = *getQueries();
            Check(queries.notifications == 1 && !queries.sdkInitializedAtReady &&
                      queries.context == &ci::GetContext() && queries.missingQueriesAreNull,
                  "actual DLL callback is one-shot before its SDK initialization");
            Check(queries.originalType == QueryType(classMetadata, "via.Prefab") &&
                      queries.customType == &classMetadata->types[arg->index],
                  "ready callback uses the published records");
            Check(queries.originalMethod && queries.originalOverload &&
                      queries.originalMethod != queries.originalOverload, "original overloaded AOT entries at ready");
            Check(queries.echo && queries.echo == queries.inheritedEcho && queries.outId &&
                      queries.constructor && queries.genericMethod, "custom/inherited/out/constructor/generic entries at ready");
            Check(QueryType(classMetadata, "System.Int32[]") &&
                      QueryType(classMetadata, "ace.btable.cCommandAction`1<ClassInjector.Tests.Arg>"),
                  "array and closed generic type names");
            auto* tuple = QueryType(classMetadata, "System.ValueTuple`2<System.UInt32,System.UInt32>");
            Check(tuple &&
                      QueryMethod(classMetadata, tuple, "Equals(System.ValueTuple`2<System.UInt32,System.UInt32>)"),
                  "generic comma stays within parameter type without a signature parser");
            for (auto& method : classMetadata->customMethods) {
                const auto& declaration = expectedClassMethods[method.index - classMetadata->originalMethodCount];
                if (method.declaringType == arg->index) {
                    if (declaration.name == "Echo") {
                        Check(queries.echo == method.entry, "cached Echo is its registered trampoline");
                    } else if (declaration.name == "TryGetId") {
                        Check(queries.outId == method.entry, "cached out is its registered trampoline");
                    } else if (declaration.name == ".ctor") {
                        Check(queries.constructor == method.entry, "cached constructor is its wrapper");
                    }
                }
            }
            auto vmctx = reinterpret_cast<void*>(0x1234567887654321ull);
            auto self = reinterpret_cast<void*>(0x3456788998765432ull);
            auto argument = reinterpret_cast<void*>(0x567899ABBA987654ull);
            auto echoEntry = reinterpret_cast<void* (*)(void*, void*, void*)>(queries.echo);
            Check(echoEntry(vmctx, self, argument) == argument, "early-cached native reference entry is callable");
            int32_t outId = 0;
            auto outEntry = reinterpret_cast<bool (*)(void*, void*, int32_t, int32_t*)>(queries.outId);
            Check(outEntry(vmctx, self, 23, &outId) && outId == 1023, "early-cached out entry is callable");
            // 此 provider 已经过 NotifyClassProviders；SDK 初始化测试不再手工重放 ready。
            InitializeTestProvider(classProvider, false);
            ci::NotifyClassProviders(callbacks, ci::GetContext());
            Check(getQueries()->sdkInitialized && !getQueries()->sdkInitializedAtReady &&
                      getQueries()->notifications == 1, "later provider SDK initialization does not replay notification");
            std::cout << "PASS: real TDB early provider notification, original overloads, custom/closed generic/array/"
                         "inherited/constructor/out queries and cached native calls before provider SDK\n";
            auto getCall = reinterpret_cast<FnGetNativeCallProbe>(GetProcAddress(classProvider, "GetNativeCallProbe"));
            Check(getCall != nullptr, "native call probe export");
            uint64_t contextMarker{}, selfMarker{};
            for (auto& method : classMetadata->customMethods) {
                const auto& declaration = expectedClassMethods[method.index - classMetadata->originalMethodCount];
                auto& definition = classMetadata->methods[method.index];
                auto* list = reinterpret_cast<const ci::ParamList*>(classMetadata->bytes.data() +
                                                                    definition.GetParameterOffset());
                Check(list->invokeId != 0, "real methods must use executable invoke recipes");
                for (auto j = 0u; j < list->count; ++j) {
                    auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
                    auto& argument = classMetadata->parameters[ids[j]];
                    auto modifier = declaration.parameters[j].modifier;
                    bool directionMatches = modifier == ci::ParameterModifier::Ref
                                                ? argument.flags <= 1
                                                : argument.flags == (modifier == ci::ParameterModifier::Out ? 2 : 0);
                    Check(argument.modifier == (modifier == ci::ParameterModifier::Value ? 0 : 2) &&
                              directionMatches,
                          "real parameter modifier/direction");
                }
                if (declaration.name == "TryGetId") {
                    std::array<int32_t, 3> out{-123456, -1, -654321};
                    auto call = reinterpret_cast<bool (*)(void*, void*, int32_t, int32_t*)>(method.entry);
                    Check(call(&contextMarker, &selfMarker, 23, &out[1]) && out[1] == 1023 &&
                              out.front() == -123456 && out.back() == -654321,
                          "real Int32 out ABI and guards");
                    Check(!call(&contextMarker, &selfMarker, -1, &out[1]) && out[1] == 0,
                          "real false result writes out");
                    auto& captured = *getCall();
                    Check(captured.vmctx == &contextMarker && captured.self == &selfMarker &&
                              captured.second == uintptr_t(&out[1]),
                          "real out vmctx/this and pointer width");
                }
                if (declaration.name == "TryGetScale") {
                    std::array<float, 3> out{-123.5f, -1.0f, -654.5f};
                    Check(reinterpret_cast<bool (*)(void*, void*, int32_t, float*)>(method.entry)(
                              &contextMarker, &selfMarker, 23, &out[1]) && out[1] == 5.75f &&
                              out.front() == -123.5f && out.back() == -654.5f,
                          "real Single out ABI and guards");
                }
                if (declaration.name == "TryGetPeer") {
                    std::array<void*, 3> out{reinterpret_cast<void*>(1), nullptr, reinterpret_cast<void*>(2)};
                    auto call = reinterpret_cast<bool (*)(void*, void*, int32_t, void**)>(method.entry);
                    Check(call(&contextMarker, &selfMarker, 0, &out[1]) && out[1] == &selfMarker &&
                              out.front() == reinterpret_cast<void*>(1) && out.back() == reinterpret_cast<void*>(2),
                          "real managed-reference out uses a pointer-to-pointer");
                    Check(!call(&contextMarker, &selfMarker, -1, &out[1]) && out[1] == nullptr,
                          "real managed-reference out clears caller storage");
                }
                if (declaration.name == "TryGetPair") {
                    int32_t first{}, second{};
                    Check(reinterpret_cast<bool (*)(void*, void*, int32_t*, int32_t*)>(method.entry)(
                              &contextMarker, &selfMarker, &first, &second) && first == -17 && second == 42,
                          "real two-out ABI");
                }
                if (declaration.name == "TryGetNumber") {
                    int32_t number{};
                    Check(reinterpret_cast<bool (*)(void*, void*, int32_t*)>(method.entry)(
                              &contextMarker, &selfMarker, &number) &&
                              number == (method.declaringType == arg->index ? 17 : 99),
                          "real virtual/override out ABI");
                }
                if (declaration.name == "Echo") {
                    Check(reinterpret_cast<uint64_t (*)(void*, void*, uintptr_t)>(method.entry)(
                              &contextMarker, &selfMarker, 0x123456789ABCull) == 0x123456789ABCull,
                          "full-width object parameter/return ABI");
                    auto& call = *getCall();
                    Check(call.vmctx == &contextMarker && call.self == &selfMarker && call.first == 0x123456789ABCull,
                          "native vmctx/this");
                }
                if (declaration.name == "execute") {
                    auto* ids = reinterpret_cast<const uint32_t*>(list + 1);
                    Check(list->count == 2 && declaration.parameters[0].name == "work" && declaration.parameters[1].name == "argument",
                          "generic template retains supplied parameter names in runtime specs");
                    Check(std::string_view(classMetadata->strings.data() + classMetadata->parameters[ids[0]].name) == "work" &&
                              std::string_view(classMetadata->strings.data() + classMetadata->parameters[ids[1]].name) == "argument",
                          "generic template emits supplied names into Parameter metadata");
                    std::array<uint32_t, 5> out;
                    out.fill(0xCCCCCCCC);
                    auto* returned =
                        reinterpret_cast<void* (*)(void*, void*, void*, uintptr_t, uintptr_t)>(method.entry)(
                            out.data() + 1, &contextMarker, &selfMarker, 0x123456789ABCull, 0xABCDEF987654ull);
                    Check(returned == out.data() + 1 && out[1] == 42 && out[2] == 1 && out[3] == 7 &&
                              out.front() == 0xCCCCCCCC && out.back() == 0xCCCCCCCC,
                          "hidden result buffer ABI");
                    auto& call = *getCall();
                    Check(call.vmctx == &contextMarker && call.self == &selfMarker && call.first == 0x123456789ABCull &&
                              call.second == 0xABCDEF987654ull,
                          "aggregate vmctx/this, register and stack arguments");
                }
                if (declaration.name == "onExecute" && method.declaringType == cmd->index) {
                    const auto slot = classMetadata->methodImpls[classMetadata->methods[method.index].impl].virtualIndex;
                    Check(slot >= 0 && generic->virtualMethods[slot] != method.index,
                          "real generic override slot");
                }
            }
            auto interfaceProvider = LoadLibraryW(RE::widen(CI_TEST_INTERFACE_PLUGIN).c_str());
            Check(interfaceProvider != nullptr, "interface test provider load");
            auto interfaceDeclarations = ci::LoadClassProviders({std::filesystem::path(CI_TEST_MANIFESTS) / "interface"});
            auto interfaceMetadata = ci::BuildTypeDatabase(header, interfaceDeclarations);
            auto& injected = *interfaceMetadata;
            auto InterfaceMethod = [&](std::string_view owner, std::string_view interfaceName, size_t slot) {
                auto* type = injected.FindType(owner);
                auto id = injected.ResolveType(interfaceName);
                auto* list = reinterpret_cast<const RE::REInterfaceList*>(
                    injected.bytes.data() + injected.types[type->index].getInterfaceListOffset());
                for (uint32_t i = 0; i < list->count; ++i) {
                    if ((list->encodedTypes[i] & 0x7FFFF) == id) {
                        return type->virtualMethods[(list->encodedTypes[i] >> 19) + slot];
                    }
                }
                throw std::runtime_error("interface entry missing");
            };
            auto probeName = "ClassInjector.Tests.InterfaceProbe";
            auto* probe = injected.FindType(probeName);
            Check(injected.IsAssignable(probe->index, injected.ResolveType("System.IDisposable")),
                  "interface assignment");
            Check(!injected.IsAssignable(injected.ResolveType("System.Object"),
                                         injected.ResolveType("System.IDisposable")),
                  "non-interface assignment");
            Check(injected.methodDeclarations[InterfaceMethod(probeName, "System.IDisposable", 0) - injected.originalMethodCount].binding.function ==
                      "DisposeProbe",
                  "interface slot mapping");
            Check(injected.methodDeclarations[InterfaceMethod("ClassInjector.Tests.DerivedInterfaceProbe", "System.IDisposable", 0) - injected.originalMethodCount].binding.function == "DisposeDerivedProbe",
                  "inherited interface virtual override");
            Check(injected.methodDeclarations[InterfaceMethod("ClassInjector.Tests.ReimplementedInterfaceProbe",
                                                      "System.ICloneable", 0) - injected.originalMethodCount].binding.function == "SelfProbe",
                  "interface reimplementation");
            Check(
                injected.methodDeclarations[InterfaceMethod("ClassInjector.Tests.ConcreteDisposable", "System.IDisposable", 0) - injected.originalMethodCount].kind == ci::MethodKind::Override,
                "abstract interface implementation");
            auto genericProbe = "ClassInjector.Tests.GenericInterfaceProbe";
            Check(injected.IsAssignable(injected.FindType(genericProbe)->index,
                                        injected.ResolveType("System.IDisposable")),
                  "transitive interface inheritance");
            Check(injected.methodDeclarations[InterfaceMethod(genericProbe, "System.Collections.IEnumerable", 0) - injected.originalMethodCount].name ==
                      "System.Collections.IEnumerable.GetEnumerator",
                  "explicit interface mapping");
            auto broken = interfaceDeclarations;
            std::erase_if(broken[0].methods, [](auto& method) { return method.name == "MoveNext"; });
            Rejects([&] { ci::BuildTypeDatabase(header, broken); });
            broken = interfaceDeclarations;
            broken[0].interfaces.push_back("System.Object");
            Rejects([&] { ci::BuildTypeDatabase(header, broken); });
            broken = interfaceDeclarations;
            broken[4].abstract = false;
            Rejects([&] { ci::BuildTypeDatabase(header, broken); });
            broken = interfaceDeclarations;
            broken.back().interfaces.push_back("System.IDisposable"); // Same dispatch index as ICollection<T>.
            broken.back().fields.push_back({"Counter", "System.Int32"});
            broken.back().methods.push_back(*std::ranges::find(broken[0].methods, "Dispose", &ci::MethodSpec::name));
            std::string conflict;
            try {
                ci::BuildTypeDatabase(header, broken);
            } catch (const std::exception& error) {
                conflict = error.what();
            }
            Check(conflict.find("conflicting interface dispatch index") != std::string::npos,
                  "interface index collision must be rejected before publication");
            injected.encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
            ci::CreateMethodTrampolines(injected);
            ci::PrepareInvokeAdapters(injected);
            InitializeTestProvider(interfaceProvider);
            Check(injected.FindMethod(InterfaceMethod(probeName, "System.IDisposable", 0))->entry != nullptr,
                  "interface C++ entry");
            ci::g_database = std::move(interfaceMetadata);
            std::array<void*, 3> probeInfo{nullptr, reinterpret_cast<void*>(16), &injected.types[probe->index]};
            struct InterfaceObject {
                void** info;
                uint64_t header;
                std::array<uint64_t, 4> fields{};
            } interfaceObject{.info = probeInfo.data() + 2};
            auto InterfaceEntry = [&](std::string_view interfaceName, std::string_view methodName) {
                auto id = injected.ResolveType(interfaceName);
                auto& type = injected.types[id];
                auto* ids = reinterpret_cast<const uint32_t*>(injected.bytes.data() + type.unknown2C);
                for (size_t slot = 0; slot < injected.typeImpls[type.implIndex].numManagedVtable; ++slot) {
                    auto& method = injected.methods[ids[slot]];
                    auto& impl = injected.methodImpls[method.impl];
                    if (std::string_view(injected.strings.data() + (impl.name & 0x0FFFFFFF)) == methodName) {
                        return injected.FindMethod(InterfaceMethod(probeName, interfaceName, slot))->entry;
                    }
                }
                throw std::runtime_error("interface method missing");
            };
            using FnVoid = void (*)(void* vmContext, void* object);
            using FnBoolean = bool (*)(void* vmContext, void* object);
            using FnObject = void* (*)(void* vmContext, void* object);
            using FnCompare = int32_t (*)(void* vmContext, void* object, void* other);
            auto moveNext = reinterpret_cast<FnBoolean>(InterfaceEntry("System.Collections.IEnumerator", "MoveNext"));
            Check(moveNext(&contextMarker, &interfaceObject) && moveNext(&contextMarker, &interfaceObject) &&
                      moveNext(&contextMarker, &interfaceObject) && !moveNext(&contextMarker, &interfaceObject),
                  "native Boolean return through interface mapping");
            auto* current = reinterpret_cast<void*>(0x123456789ABCull);
            auto* interfaceSelf = reinterpret_cast<reframework::API::ManagedObject*>(&interfaceObject);
            *interfaceSelf->get_field<void*>("Current") = current;
            Check(reinterpret_cast<FnObject>(InterfaceEntry("System.ICloneable", "Clone"))(
                      &contextMarker, &interfaceObject) == current &&
                      reinterpret_cast<FnCompare>(InterfaceEntry("System.IComparable", "CompareTo"))(
                          &contextMarker, &interfaceObject, current) == 123,
                  "native interface object and Int32 returns");
            reinterpret_cast<FnVoid>(InterfaceEntry("System.IDisposable", "Dispose"))(&contextMarker, &interfaceObject);
            Check(*interfaceSelf->get_field<int32_t>("Counter") == 1, "native interface Void method and SDK field API");
            std::cout << "PASS: existing interfaces, implicit/explicit mapping, inherited override, reimplementation, "
                         "abstract implementation, interface inheritance, reference compatibility, native calls\n";
            std::cout
                << "PASS: real TDB snapshot, existing engine parents, nested/closed field types, generic identity, "
                   "substituted abstract method, named template parameters, existing override, 64-bit pointers, aggregate return\n";
        }

        declarations[0].methods.push_back({.name = "Sum",
                                           .returnType = "System.Int32",
                                           .parameters = {"a", "b"},
                                           .binding = {.backend = ci::Backend::Cpp, .function = "Sum", .version = 2}});
        declarations[0].methods.push_back(
            {.name = "SetCounter",
             .parameters = {"value"},
             .binding = {.backend = ci::Backend::Cpp, .function = "SetCounter", .version = 1}});
        declarations.push_back({.name = "Tests.GrandChild",
                                .base = declarations[1].name,
                                .fields = {{"Extra", "System.Int32"}},
                                .methods = {{.name = ".ctor", .kind = ci::MethodKind::Constructor}}});
        ci::ProviderConfig nativePlugin{.configPath = "native.toml", .dllPath = CI_TEST_PROVIDER, .types = declarations};
        Check(ci::LoadClassProviderBindings(nativePlugin), "native test function registration");
        declarations = std::move(nativePlugin.types);
        ci::g_database = ci::BuildTypeDatabase(fixture.header, declarations);
        auto& native = *ci::g_database;
        native.encodingBase = uintptr_t(&ObjectCtor) - 0x1000;
        native.methods[native.types[fixture.object].defaultCtorIndex].encodedOffset = 0x1000;
        const auto expectedNativeMethods = native.methodDeclarations;
        ci::CreateMethodTrampolines(native);
        ci::PrepareInvokeAdapters(native);
        InitializeTestProvider(GetModuleHandleW(RE::widen(CI_TEST_PROVIDER).c_str()));
        auto& child = native.customTypes[1];
        std::array<void*, 16> info{};
        info[2] = &native.types[child.index];
        info[1] = reinterpret_cast<void*>(16);
        struct Object {
            void** info;
            uint64_t header;
            std::array<int32_t, 8> fields;
        } instance{.info = info.data() + 2};
        auto* self = reinterpret_cast<reframework::API::ManagedObject*>(&instance);
        native.types[child.index].objectInfo = instance.info;
        native.types[native.types[child.index].parentTypeIndex].objectInfo = nullptr;
        const auto& metadata = ci::GetMetadataAPI();
        const auto earlyOwner = reinterpret_cast<ci::TypeDefinitionHandle>(&native.types[child.index]);
        const auto earlyCounter = metadata.type_definition->find_field(earlyOwner, "Counter");
        const auto earlyBonus = metadata.type_definition->find_field(earlyOwner, "Bonus");
        Check(earlyCounter && earlyBonus && metadata.field->get_offset_from_fieldptr(earlyCounter) == 0 &&
                  metadata.field->get_offset_from_fieldptr(earlyBonus) == 8 &&
                  metadata.field->get_data_raw(earlyCounter, &instance, false) ==
                      reinterpret_cast<uint8_t*>(&instance) + 16 &&
                  metadata.field->get_data_raw(earlyBonus, &instance, false) ==
                      reinterpret_cast<uint8_t*>(&instance) + 24 &&
                  metadata.type_definition->get_valuetype_size(metadata.field->get_type(earlyCounter)) == 4 &&
                  !metadata.type_definition->find_field(earlyOwner, "Missing"),
              "REF field offsets, inherited actual-object addressing and payload without RF SDK");
        auto& bonusLayout = native.fieldImpls[native.fields[native.types[child.index].memberFieldIndex].impl];
        const auto oldAttributes = bonusLayout.attributes;
        bonusLayout.attributes = 0x10;
        Check(!metadata.field->is_static(earlyBonus), "attribute index is not Static flag");
        bonusLayout.flags |= 0x10;
        Check(metadata.field->is_static(earlyBonus), "static declaration remains queryable");
        bonusLayout.flags &= ~0x10;
        bonusLayout.attributes = oldAttributes;
        instance.fields.fill(int32_t(0xCCCCCCCC));
        uint64_t contextMarker{};
        auto* ctor = native.FindMethod(uint32_t(native.types[child.index].defaultCtorIndex));
        reinterpret_cast<void (*)(void*, void*)>(ctor->entry)(&contextMarker, &instance);
        Check(g_ctorCalls == 1 && g_ctorContext == &contextMarker && g_ctorSelf == &instance &&
                  *self->get_field<int32_t>("Counter") == 10 && *self->get_field<int32_t>("Bonus") == 100,
              "constructor chain/context/this/provider fields");
        Check(instance.fields[1] == int32_t(0xCCCCCCCC) && instance.fields[3] == int32_t(0xCCCCCCCC) && instance.fields[4] == int32_t(0xCCCCCCCC),
              "explicit constructor does not clear allocator-owned fields");
        for (auto& method : native.customMethods) {
            const auto& declaration = expectedNativeMethods[method.index - native.originalMethodCount];
            auto* list = reinterpret_cast<const ci::ParamList*>(native.bytes.data() +
                                                                native.methods[method.index].GetParameterOffset());
            Check(list->invokeId != 0, "unavailable invoke recipe must be skipped");
            if (declaration.binding.backend == ci::Backend::Cpp) {
                auto* entry = static_cast<uint8_t*>(method.entry);
                auto slot = HookUtils::ReadOffsetData(uintptr_t(entry), 2, 6);
                Check(entry[0] == 0xFF && entry[1] == 0x25 && *reinterpret_cast<void**>(slot) == declaration.binding.address,
                      "provider method/constructor must be a direct native tail jump");
            }
            if (declaration.name == "Add") {
                Check(reinterpret_cast<int32_t (*)(void*, void*, int32_t)>(method.entry)(&contextMarker, &instance,
                                                                                         5) == 15,
                      "one-argument entry ABI");
            }
            if (declaration.name == "Sum") {
                Check(reinterpret_cast<int32_t (*)(void*, void*, int32_t, int32_t)>(method.entry)(
                          &contextMarker, &instance, -7, 29) == 22,
                      "two-argument entry ABI");
            }
            if (declaration.name == "SetCounter") {
                reinterpret_cast<void (*)(void*, void*, int32_t)>(method.entry)(&contextMarker, &instance, 42);
            }
        }
        auto* evaluate = native.FindMethod(child.virtualMethods[4]);
        Check(reinterpret_cast<int32_t (*)(void*, void*)>(evaluate->entry)(&contextMarker, &instance) == 142,
              "zero-argument return ABI");
        auto& grandChild = native.customTypes[2];
        info[2] = &native.types[grandChild.index];
        instance.fields.fill(int32_t(0xCCCCCCCC));
        auto* builtinCtor = native.FindMethod(uint32_t(native.types[grandChild.index].defaultCtorIndex));
        Check(expectedNativeMethods[builtinCtor->index - native.originalMethodCount].binding.address == nullptr, "generated constructor has no provider body");
        reinterpret_cast<void (*)(void*, void*)>(builtinCtor->entry)(&contextMarker, &instance);
        Check(g_ctorCalls == 2 && *self->get_field<int32_t>("Counter") == 10 &&
                  *self->get_field<int32_t>("Bonus") == 100 && *self->get_field<int32_t>("Extra") == int32_t(0xCCCCCCCC),
              "generated default constructor forwards parent without clearing fields");
        std::cout
            << "PASS: function registration, TOML, C++ bindings, hash, shadow tables, inheritance, fields, invoke "
               "recipes, virtual override, partition, cycles, Win64 entries (0/1/2 args), provider DLL, "
               "native tail jumps, constructor chain\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
