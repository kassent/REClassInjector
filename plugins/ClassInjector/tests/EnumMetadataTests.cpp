#include "ClassInjectorTestAccess.h"
#include "ClassProviders.h"
#include "TypeDatabaseBuilder.h"
#include <algorithm>
#include <array>
#include <ci/Provider.hpp>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace ClassInjectorEnumMetadataTests {
static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// 合成TDB只验证元数据与常量编码；不执行虚构原生入口，不冒充真机反射。
struct Fixture {
    std::array<RE::RETypeDefinition, 7> types{};
    std::array<RE::RETypeImpl, 7> impls{};
    std::array<ci::FieldDef, 6> fields{};
    std::array<ci::FieldImpl, 3> fieldImpls{};
    std::vector<char> strings{0};
    std::vector<uint8_t> bytes = std::vector<uint8_t>(8);
    std::vector<int32_t> initializers{0, 4, 0};
    RE::TDB header{};

    uint32_t Text(const char* value) {
        auto offset = uint32_t(strings.size());
        strings.insert(strings.end(), value, value + std::strlen(value) + 1);
        return offset;
    }
    Fixture() {
        for (uint32_t i = 0; i < types.size(); ++i) { types[i].index = i; types[i].implIndex = i; }
        const auto system = Text("System"), tests = Text("Tests");
        for (auto [id, name] : {std::pair{1, "Object"}, {2, "Enum"}, {3, "Int32"}, {4, "Color"}, {5, "Color[]"}, {6, "Other"}}) {
            impls[id].nameOffset = Text(name);
            impls[id].namespaceOffset = id <= 3 ? system : tests;
        }
        types[1].objectType = types[2].objectType = 1;
        types[2].parentTypeIndex = 1;
        for (auto id : {3, 4, 6}) {
            types[id].objectType = 5; types[id].underlyingTypeIndex = 8;
            types[id].parentTypeIndex = id == 3 ? 1 : 2;
            types[id].instanceSize = 20; impls[id].fieldSize = 4;
        }
        types[4].arrayTypeIndex = 5; types[5].objectType = 2; types[5].elementTypeIndex = 4;
        const auto one = strings.size();
        strings.insert(strings.end(), {1, 0, 0, 0});
        initializers[2] = -int32_t(one); // 覆盖原生负偏移到stringPool的常量路径。
        fieldImpls[0] = {.attributes = 0, .flags = 0x4606, .name = Text("value__")};
        fieldImpls[1] = {.attributes = 0, .flags = 0x56, .initLo = 1, .name = Text("Zero")};
        fieldImpls[2] = {.attributes = 0, .flags = 0x56, .initLo = 2, .name = Text("One")};
        for (auto id : {4, 6}) {
            types[id].memberFieldIndex = id == 4 ? 0 : 3;
            impls[id].numMemberFields = 3;
            for (uint32_t j = 0; j < 3; ++j) fields[types[id].memberFieldIndex + j] = {.declaringType = uint64_t(id), .impl = j, .type = uint64_t(j ? id : 3)};
        }
        Refresh();
    }
    void Refresh() {
        header = {
            .version = 81,
            .numTypes = uint32_t(types.size()),
            .numFields = uint32_t(fields.size()),
            .numTypeImpl = uint32_t(impls.size()),
            .numFieldImpl = uint32_t(fieldImpls.size()),
            .numInitData = int32_t(initializers.size()),
            .numStringPool = uint32_t(strings.size()),
            .numBytePool = uint32_t(bytes.size()),
            .types = types.data(),
            .typeImpls = impls.data(),
            .fields = fields.data(),
            .fieldImpls = fieldImpls.data(),
            .initData = initializers.data(),
            .stringPool = strings.data(),
            .bytePool = bytes.data()};
    }
};

static int32_t Value(const ci::TypeDatabase& database, const ci::FieldDef& field) {
    const auto& impl = database.fieldImpls[field.impl];
    auto index = field.initHi << 10 | impl.initMid << 6 | impl.initLo;
    auto offset = database.initializers.at(index);
    int32_t result;
    const void* data = offset < 0 ? static_cast<const void*>(database.strings.data() - int64_t(offset)) : database.bytes.data() + offset;
    std::memcpy(&result, data, 4);
    return result;
}

static void Run() {
    Fixture fixture;
    auto declarations = ci::ParseCustomTypeConfig(R"(
schema_version=1
[[enums]]
name="Tests.Color"
renames=[{fromName="Zero",to="Nil"}]
appends=[{name="Negative",value=-7},{name="Min",value=-2147483648},{name="Max",value=2147483647},{name="Alias",value=0}]
)", "enum-tests.toml");
    auto database = ci::BuildTypeDatabase(fixture.header, declarations);
    auto& type = database->types[4];
    Check(database->types.size() == fixture.types.size() && type.arrayTypeIndex == 5 && type.instanceSize == 20, "enum/array identity and layout");
    Check(database->typeImpls[type.implIndex].numMemberFields == 7, "new member count");
    Check(std::memcmp(database->fields.data(), fixture.fields.data(), sizeof(fixture.fields)) == 0, "old global field IDs preserved");
    Check(std::equal(fixture.bytes.begin(), fixture.bytes.end(), database->bytes.begin()) && std::equal(fixture.strings.begin(), fixture.strings.end(), database->strings.begin()), "original pool prefixes preserved");
    Check(std::equal(fixture.initializers.begin(), fixture.initializers.end(), database->initializers.begin()), "old initializer indices preserved");
    const auto begin = type.memberFieldIndex;
    const std::array<int32_t, 6> expected{0, 1, -7, INT32_MIN, INT32_MAX, 0};
    for (size_t i = 0; i < expected.size(); ++i) Check(Value(*database, database->fields[begin + i + 1]) == expected[i], "Int32 constant roundtrip");
    Check(std::string_view(database->strings.data() + database->fieldImpls[database->fields[begin + 1].impl].name) == "Nil", "rename visible");
    Check(std::string_view(database->strings.data() + database->fieldImpls[database->fields[4].impl].name) == "Zero", "shared impl copy-on-write");
    auto target = fixture.header;
    database->Publish(target);
    Check(target.initData == database->initializers.data() && target.numInitData == 7, "initializer publication");
    Check(target.numTypes == fixture.header.numTypes, "no new types");
    auto Reject = [&](const std::vector<ci::CustomTypeConfig>& specs) {
        try { ci::BuildTypeDatabase(fixture.header, specs); } catch (const std::exception&) { return; }
        throw std::runtime_error("invalid declaration was accepted");
    };
    auto bad = declarations; bad[0].enumExtension->renames[0].source = "Missing"; Reject(bad);
    bad = declarations; bad[0].enumExtension->renames[0].to = "One"; Reject(bad);
    bad = declarations; bad[0].enumExtension->appends[0].name = "One"; Reject(bad);
    bad = declarations; bad[0].name = "System.Int32"; Reject(bad);
    bad = declarations; bad[0].name = "Tests.Missing"; Reject(bad);
    Check(fixture.header.types[4].memberFieldIndex == 0 && fixture.fieldImpls[1].name == database->fieldImpls[1].name, "failure leaves original metadata unchanged");
    // 纯改名只保留常量索引：即便离线输入没有捕获initData，也不应读取该指针。
    auto renameOnly = declarations;
    renameOnly[0].enumExtension->appends.clear();
    auto incomplete = fixture.header; incomplete.initData = nullptr;
    auto renamed = ci::BuildTypeDatabase(incomplete, renameOnly);
    Check(renamed->initializers.empty(), "rename-only must not copy initializer data");
    auto& renamedField = renamed->fields[renamed->types[4].memberFieldIndex + 1];
    auto& renamedImpl = renamed->fieldImpls[renamedField.impl];
    Check(renamedField.initHi == fixture.fields[1].initHi && renamedImpl.initMid == fixture.fieldImpls[1].initMid && renamedImpl.initLo == fixture.fieldImpls[1].initLo, "rename preserves the exact constant index");
    auto boundary = declarations; boundary[0].enumExtension->renames.clear(); boundary[0].enumExtension->appends.resize(1);
    fixture.initializers.resize(65535); fixture.Refresh();
    auto edge = ci::BuildTypeDatabase(fixture.header, boundary);
    auto& last = edge->fields.back(); auto& lastImpl = edge->fieldImpls[last.impl];
    Check(last.initHi == 63 && lastImpl.initMid == 15 && lastImpl.initLo == 63 && edge->initializers.size() == 65536, "last 16-bit initializer index");
    fixture.initializers.resize(65536); fixture.Refresh(); Reject(boundary);
    for (auto text : {
             "schema_version=1\n[[enums]]\nname='Tests.Color'\nappends=[{name='TooBig',value=2147483648}]",
             "schema_version=1\n[[enums]]\nname='Tests.Color'\nrenames='bad'",
             "schema_version=1\n[[enums]]\nname='Tests.Color'\nrenames=[{fromName=0,to='WrongTomlSource'}]",
             "schema_version=1\n[[enums]]\nname='Tests.Color'\nappends='bad'"}) {
        bool rejected = false;
        try { ci::ParseCustomTypeConfig(text, "bad.toml"); } catch (const std::exception&) { rejected = true; }
        Check(rejected, "invalid TOML enum accepted");
    }
    std::cout << "PASS: existing enum rename/append, Int32 zero/negative/bounds/alias, signed pool offsets, COW, stable IDs, publication, errors and 16-bit index boundary\n";
}

static void TestReviewRegressions() {
    Fixture fixture;
    const std::string renameText = "schema_version=1\n[[enums]]\nname='Tests.Color'\nrenames=[{fromName='Zero',to='Nil'}]";
    auto renames = ci::ParseCustomTypeConfig(renameText, "rename-only.toml");
    Check(!renames[0].extend && renames[0].IsTypeExtension(), "enum operations do not duplicate the class extension flag");
    auto renamed = ci::BuildTypeDatabase(fixture.header, renames);
    auto published = fixture.header;
    renamed->Publish(published);
    Check(published.initData == fixture.header.initData && published.numInitData == fixture.header.numInitData, "rename-only publication must preserve the original initializer table");

    bool rejected = false;
    try { ci::ParseCustomTypeConfig("types='bad'\n" + renameText, "mixed-invalid.toml"); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected, "mixed declarations must reject non-array types");
    Check(ci::ParseCustomTypeConfig("types=[]\n" + renameText, "mixed-valid.toml").size() == 1, "mixed declarations must accept an empty types array");
    Check(ci::ParseCustomTypeConfig("types=[{name='Tests.Added'}]\n" + renameText, "mixed-valid.toml").size() == 2, "mixed declarations must retain both class and enum declarations");
    auto enumProvider = ci::ParseProviderConfig(renameText, "enum-provider.toml");
    Check(enumProvider.types.size() == 1 && enumProvider.types[0].enumExtension &&
              enumProvider.types[0].enumExtension->renames[0].to == "Nil",
          "provider parser recognizes an enums-only config instead of treating it as an empty marker");
    auto mixedProvider = ci::ParseProviderConfig("types=[{name='Tests.Added'}]\n" + renameText, "mixed-provider.toml");
    Check(mixedProvider.types.size() == 2 && mixedProvider.types[0].name == "Tests.Added" &&
              mixedProvider.types[1].enumExtension && mixedProvider.types[1].name == "Tests.Color",
          "provider parser retains both types and enums in a mixed config");
    Check(ci::ParseCustomTypeConfig("schema_version=1\nenums=[]", "empty-enums.toml").empty(),
          "an empty enums array is valid");
    for (const auto* prefix : {"", "types=[]\n"}) {
        std::string error;
        try {
            ci::ParseCustomTypeConfig(std::string("schema_version=1\n") + prefix + "enums={}", "invalid-enums.toml");
        } catch (const std::runtime_error& failure) { error = failure.what(); }
        Check(error.contains("enums must be an array"), "enums uses the same array contract as types");
    }

    // TOML字符串按长度比较，TDB名称按NUL终止；只锁定这一表示契约，不扩展字符规则。
    for (auto operation : {
             R"(appends=[{name="One\u0000extra",value=2}])",
             R"(renames=[{fromName="Zero",to="\u0000"}])",
             R"(renames=[{fromName="Zero\u0000extra",to="Nil"}])"}) {
        rejected = false;
        try {
            auto specs = ci::ParseCustomTypeConfig(std::string("schema_version=1\n[[enums]]\nname='Tests.Color'\n") + operation, "nul-name.toml");
            ci::BuildTypeDatabase(fixture.header, specs);
        } catch (const std::exception&) { rejected = true; }
        Check(rejected, "enum names containing NUL must be rejected");
    }

    // near-limit输入是合成边界：成员块复制3条FieldDef，改名只新增1条FieldImpl。
    constexpr size_t FIELD_IMPL_LIMIT = 1u << 19;
    std::vector<ci::FieldImpl> crowded(fixture.fieldImpls.begin(), fixture.fieldImpls.end());
    crowded.resize(FIELD_IMPL_LIMIT - 2);
    auto nearLimit = fixture.header;
    nearLimit.fieldImpls = crowded.data();
    nearLimit.numFieldImpl = uint32_t(crowded.size());
    auto edge = ci::BuildTypeDatabase(nearLimit, renames);
    Check(edge->fields.size() == fixture.fields.size() + 3 && edge->fieldImpls.size() == FIELD_IMPL_LIMIT - 1, "reserve FieldDef and FieldImpl by their separate increments");

    auto chain = renames;
    chain[0].enumExtension->renames = {{.source = "Zero", .to = "First"}, {.source = "First", .to = "Second"}, {.source = "Second", .to = "Third"}, {.source = "Third", .to = "Last"}};
    auto chained = ci::BuildTypeDatabase(fixture.header, chain);
    Check(chained->fieldImpls.size() == fixture.fieldImpls.size() + 4, "count every chained rename implementation");
    rejected = false;
    try { ci::BuildTypeDatabase(nearLimit, chain); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected, "rename implementations must respect their own packed limit");

    auto unchanged = renames;
    unchanged[0].enumExtension->renames[0].to = "Zero";
    auto noOp = ci::BuildTypeDatabase(nearLimit, unchanged);
    Check(noOp->fieldImpls.size() == crowded.size() && noOp->strings == fixture.strings, "same-name rename must not allocate a string or implementation");
    unchanged[0].enumExtension->renames[0] = {.source = "Missing", .to = "Missing"};
    rejected = false;
    try { ci::BuildTypeDatabase(fixture.header, unchanged); }
    catch (const std::exception& error) {
        const std::string_view message(error.what());
        rejected = message.contains("rename-only.toml") && message.contains("Tests.Color") && message.contains("Missing");
    }
    Check(rejected, "same-name rename must still require an existing source");
    std::cout << "PASS: mixed declaration validation, NUL name boundary, rename-only publication, separate table limits, chained and no-op renames\n";
}

// 纯无变化声明不复制成员/类型实现；仍走真实目标与源名称校验。
static void TestNoOpStorage() {
    Fixture fixture;
    ci::CustomTypeConfig unchanged{.name = "Tests.Color", .enumExtension = ci::EnumExtensionSpec{}};
    for (bool byValue : {false, true}) {
        unchanged.enumExtension->renames = {{.source = byValue ? std::variant<std::string, int32_t>{0} : std::variant<std::string, int32_t>{"Zero"}, .to = "Zero"}};
        auto database = ci::BuildTypeDatabase(fixture.header, {unchanged, unchanged});
        Check(database->fields.size() == fixture.fields.size() && database->typeImpls.size() == fixture.impls.size(), "repeated no-op must not copy member or type implementation blocks");
        Check(database->types[4].memberFieldIndex == fixture.types[4].memberFieldIndex && database->types[4].implIndex == fixture.types[4].implIndex, "no-op must retain original member/implementation indices");
        Check(database->fieldImpls.size() == fixture.fieldImpls.size() && database->strings == fixture.strings && database->bytes == fixture.bytes && database->initializers.empty(), "no-op must not append records, names or initializer data");
    }
    unchanged.enumExtension->renames.clear();
    auto empty = ci::BuildTypeDatabase(fixture.header, {unchanged});
    Check(empty->fields.size() == fixture.fields.size() && empty->typeImpls.size() == fixture.impls.size(), "empty provider extension must not allocate metadata");

    // 两张复制表各只剩不到一个成员块/实现的容量，no-op 不应被虚增预留拒绝。
    std::vector<ci::FieldDef> fields(fixture.fields.begin(), fixture.fields.end());
    std::vector<RE::RETypeImpl> implementations(fixture.impls.begin(), fixture.impls.end());
    fields.resize((1u << 20) - 1);
    implementations.resize((1u << 18) - 1);
    auto crowded = fixture.header;
    crowded.fields = fields.data(); crowded.numFields = uint32_t(fields.size());
    crowded.typeImpls = implementations.data(); crowded.numTypeImpl = uint32_t(implementations.size());
    unchanged.enumExtension->renames = {{.source = "Zero", .to = "Zero"}};
    auto edge = ci::BuildTypeDatabase(crowded, {unchanged});
    Check(edge->fields.size() == fields.size() && edge->typeImpls.size() == implementations.size(), "no-op must reserve zero FieldDef and TypeImpl records at packed limits");

    auto changed = unchanged;
    changed.enumExtension->renames[0].to = "Renamed";
    changed.enumExtension->appends.push_back({.name = "Added", .value = -7});
    auto tail = unchanged;
    tail.enumExtension->renames = {{.source = "Renamed", .to = "Renamed"}, {.source = int32_t(-7), .to = "Added"}};
    auto mixed = ci::BuildTypeDatabase(fixture.header, {unchanged, changed, tail});
    Check(mixed->fields.size() == fixture.fields.size() + 4 && mixed->typeImpls.size() == fixture.impls.size() + 1, "no-ops around real changes must copy exactly one member block and implementation");
    for (const char* source : {"Missing", "value__"}) {
        unchanged.enumExtension->renames = {{.source = source, .to = source}};
        bool rejected = false;
        try { ci::BuildTypeDatabase(fixture.header, {unchanged}); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected, "no-op still validates existing non-reserved source");
    }
    unchanged.name = "System.Int32";
    unchanged.enumExtension->renames.clear();
    bool rejected = false;
    try { ci::BuildTypeDatabase(fixture.header, {unchanged}); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected, "empty operation must still target an existing Int32 enum");
    std::cout << "PASS: enum no-op zero writes, exact member/type capacities, ordered mixed edits and source validation\n";
}

static void TestValueSelectedRenames() {
    Fixture fixture;
    auto ByValue = [](int32_t value, const char* to) {
        ci::CustomTypeConfig type{.name = "Tests.Color", .enumExtension = ci::EnumExtensionSpec{}};
        type.enumExtension->renames.push_back({.source = value, .to = to});
        return type;
    };
    auto Name = [](const ci::TypeDatabase& database, uint32_t slot) {
        const auto& field = database.fields[database.types[4].memberFieldIndex + slot];
        return std::string_view(database.strings.data() + database.fieldImpls[field.impl].name);
    };
    // 名称声明、按值声明、追加和后续按名/按值调用按顺序合并，不产生第二种编辑实现。
    auto specs = ci::ParseCustomTypeConfig("schema_version=1\n[[enums]]\nname='Tests.Color'\nrenames=[{fromName='Zero',to='First'}]\nappends=[{name='Alias',value=0},{name='Negative',value=-7},{name='Min',value=-2147483648},{name='Max',value=2147483647}]", "before-value.toml");
    auto values = ci::ParseCustomTypeConfig(R"(
schema_version=1
[[enums]]
name='Tests.Color'
renames=[{fromValue=0,to='Canonical'}, {fromName='Canonical',to='Interim'},
         {fromValue=0,to='Canonical'}, {fromValue=1,to='FromStringPool'},
         {fromValue=-7,to='Signed'}, {fromValue=-2147483648,to='Lower'},
         {fromValue=2147483647,to='Upper'}]
)", "mixed-sources.toml");
    Check(std::get<int32_t>(values[0].enumExtension->renames[0].source) == 0 && std::get<std::string>(values[0].enumExtension->renames[1].source) == "Canonical", "TOML must preserve explicit source name/value kinds");
    specs.push_back(values[0]);
    auto tail = ci::ParseCustomTypeConfig("schema_version=1\n[[enums]]\nname='Tests.Color'\nrenames=[{fromName='Canonical',to='Final'}]", "after-value.toml");
    specs.push_back(tail[0]);
    auto database = ci::BuildTypeDatabase(fixture.header, specs);
    Check(Name(*database, 1) == "Final" && Name(*database, 2) == "FromStringPool" && Name(*database, 3) == "Alias", "source order, signed pool and canonical alias selection");
    const std::array<int32_t, 6> expected{0, 1, 0, -7, INT32_MIN, INT32_MAX};
    for (uint32_t i = 0; i < expected.size(); ++i) Check(Value(*database, database->fields[database->types[4].memberFieldIndex + i + 1]) == expected[i], "value-selected rename changed a constant");
    Check(Name(*database, 4) == "Signed" && Name(*database, 5) == "Lower" && Name(*database, 6) == "Upper", "appended signed values selected");
    Check(std::string_view(database->strings.data() + database->fieldImpls[database->fields[4].impl].name) == "Zero", "value-selected rename modified shared impl");

    auto single = ByValue(0, "Renamed");
    auto renamed = ci::BuildTypeDatabase(fixture.header, {single});
    auto published = fixture.header;
    renamed->Publish(published);
    Check(Name(*renamed, 1) == "Renamed" && renamed->initializers.empty() && published.initData == fixture.header.initData && published.numInitData == fixture.header.numInitData, "value-selected rename must read but not copy/publish the constant table");
    Check(renamed->types[4].arrayTypeIndex == 5 && renamed->types[4].instanceSize == 20 && std::memcmp(renamed->fields.data(), fixture.fields.data(), sizeof(fixture.fields)) == 0, "value-selected rename must preserve IDs and layout");

    Fixture signedFixture;
    const int32_t negative = -7, minimum = INT32_MIN;
    std::memcpy(signedFixture.bytes.data() + 4, &negative, sizeof(negative));
    std::memcpy(signedFixture.strings.data() - int64_t(signedFixture.initializers[2]), &minimum, sizeof(minimum));
    auto signedOriginal = ci::BuildTypeDatabase(signedFixture.header, {ByValue(negative, "ByteNegative"), ByValue(minimum, "StringMinimum")});
    Check(Name(*signedOriginal, 1) == "ByteNegative" && Name(*signedOriginal, 2) == "StringMinimum", "original negative/Int32 minimum constants from both pools");
    Fixture packedFixture;
    constexpr uint32_t PACKED_INDEX = (1u << 10) | (2u << 6) | 9;
    packedFixture.fields[1].initHi = 1; packedFixture.fieldImpls[1].initMid = 2; packedFixture.fieldImpls[1].initLo = 9;
    packedFixture.initializers.resize(PACKED_INDEX + 1); packedFixture.initializers[PACKED_INDEX] = 4; packedFixture.Refresh();
    auto packed = ci::BuildTypeDatabase(packedFixture.header, {single});
    Check(Name(*packed, 1) == "Renamed", "value selection must decode all three packed initializer slices");

    // 按值的no-op必须先归一，避免预检把无新增impl的操作误算为容量溢出。
    constexpr size_t FIELD_IMPL_LIMIT = 1u << 19;
    std::vector<ci::FieldImpl> crowded(fixture.fieldImpls.begin(), fixture.fieldImpls.end());
    crowded.resize(FIELD_IMPL_LIMIT - 1);
    auto nearLimit = fixture.header;
    nearLimit.fieldImpls = crowded.data(); nearLimit.numFieldImpl = uint32_t(crowded.size());
    auto noOp = ci::BuildTypeDatabase(nearLimit, {ByValue(0, "Zero")});
    Check(noOp->fieldImpls.size() == crowded.size() && noOp->strings == fixture.strings, "value-selected no-op must allocate neither name nor impl");

    auto Reject = [&](const RE::TDB& input, const std::vector<ci::CustomTypeConfig>& declarations) {
        try { ci::BuildTypeDatabase(input, declarations); } catch (const std::exception&) { return; }
        throw std::runtime_error("invalid value-selected rename was accepted");
    };
    Reject(fixture.header, {ByValue(9, "Missing")});
    Reject(fixture.header, {ByValue(0, "One")});
    Reject(fixture.header, {ByValue(0, "value__")});
    auto invalidType = single; invalidType.name = "System.Int32";
    Reject(fixture.header, {invalidType});
    invalidType.name = "Tests.Missing";
    Reject(fixture.header, {invalidType});
    Reject(nearLimit, {single});
    auto incomplete = fixture.header; incomplete.initData = nullptr;
    Reject(incomplete, {single});
    incomplete = fixture.header; incomplete.numInitData = 1;
    Reject(incomplete, {single});
    // 池偏移0表示无初始化值，偏移正负都需包含完整Int32。
    for (auto offset : {0, 7, INT32_MAX, INT32_MIN, -int32_t(fixture.strings.size() - 2)}) {
        auto invalid = fixture.initializers; invalid[1] = offset;
        incomplete = fixture.header; incomplete.initData = invalid.data();
        Reject(incomplete, {single});
    }
    Check(fixture.types[4].memberFieldIndex == 0 && fixture.fieldImpls[1].name == renamed->fieldImpls[1].name, "failed value selection changed original metadata");
    const ci::RegistrationAPI unavailableAPI{};
    ci::Registration unavailable(unavailableAPI);
    unavailable.register_enum("Tests.Color", {.renames = {ci::EnumRename::by_value(0, "Zero")}});
    Check(unavailable.has_errors(), "unavailable enum callback must fail registration");
    std::cout << "PASS: value-selected canonical rename, mixed declaration order, appended zero/negative/bounds, signed pools, COW, exact no-op capacity, stable publication and invalid sources\n";
}

static void TestProvider(const std::filesystem::path& file) {
    ci::ProviderConfig plugin{.configPath = file.parent_path() / "EnumProbe.toml", .dllPath = file};
    auto accepted = ci::ResolveClassProviders({plugin});
    Check(accepted.size() == 2 && accepted[0].enumExtension && accepted[0].enumExtension->appends[0].value == 987654321, "dynamic provider enum collection");
    Check(accepted[0].providerSource && accepted[0].providerSource == accepted[1].providerSource &&
          accepted[0].providerSource->configPath == plugin.configPath.string() && accepted[0].providerSource->dllPath == file.string(),
          "dynamic enum declarations retain their Provider origin");
    const auto& first = accepted[0].enumExtension->renames;
    const auto& second = accepted[1].enumExtension->renames;
    Check(first.size() == 4 && std::get<std::string>(first[0].source) == "CI_Thursday" && std::get<int32_t>(first[1].source) == 2 && std::get<int32_t>(first[2].source) == 0 && first[2].to == "CI_SundayByCpp", "unified provider name/value input must be copied before return");
    Check(second.size() == 4 && std::get<int32_t>(second[0].source) == 987654321 && std::get<std::string>(second[1].source) == "CI_SundayByCpp", "unified provider must preserve mixed source order");
    ci::CustomTypeConfig bad{.name = "Tests.Unavailable"};
    bad.methods.push_back({.name = "Missing", .binding = {.backend = ci::Backend::Cpp, .function = "Missing", .version = 1}});
    plugin.types.push_back(bad);
    Check(ci::ResolveClassProviders({plugin}).empty(), "failed provider leaked its dynamic enum declarations");
    std::cout << "PASS: dynamic POD enum registration and atomic discard with a failing function binding\n";
}

static void TestRenameSourceValidation() {
    // 配置存在性独立于值：0合法，缺省、双源、错误类型、旧from拼法均不混淆。
    for (const auto* rename : {
        "{to='Nil'}", "{fromName='Zero',fromValue=0,to='Nil'}",
        "{fromName='',to='Nil'}", "{fromName=0,to='Nil'}",
        "{fromValue='0',to='Nil'}", "{fromValue=0.0,to='Nil'}",
        "{fromValue=false,to='Nil'}", "{fromValue=2147483648,to='Nil'}",
        "{fromValue=-2147483649,to='Nil'}", "{fromValue=0}",
        "{fromValue=0,to=''}", "{from='Zero',to='Nil'}",
        "{from='Zero',fromValue=0,to='Nil'}"
    }) {
        bool rejected = false;
        try { ci::ParseCustomTypeConfig(std::string("schema_version=1\n[[enums]]\nname='Tests.Color'\nrenames=[") + rename + "]", "invalid-source.toml"); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected, "invalid/ambiguous TOML rename source accepted");
    }
    const auto name = ci::EnumRename::by_name("Zero", "Nil");
    const auto zero = ci::EnumRename::by_value(0, "Nil");
    Check(name.kind == ci::EnumMatchKind::Name && std::string_view(name.from_name) == "Zero" &&
              zero.kind == ci::EnumMatchKind::Value && zero.from_value == 0,
          "POD factories must select a source explicitly without a zero sentinel");
    std::cout << "PASS: unified fromName/fromValue XOR, required target, strict Int32, obsolete from rejection and tagged POD factories\n";
}
}
int main(int argc, char** argv) {
    try {
        ClassInjectorEnumMetadataTests::Run();
        ClassInjectorEnumMetadataTests::TestReviewRegressions();
        ClassInjectorEnumMetadataTests::TestNoOpStorage();
        ClassInjectorEnumMetadataTests::TestValueSelectedRenames();
        ClassInjectorEnumMetadataTests::TestRenameSourceValidation();
        if (argc > 1) ClassInjectorEnumMetadataTests::TestProvider(argv[1]);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
