#include "ClassInjectorTestAccess.h"
#include "ClassProviderContext.h"
#include "ClassProviders.h"
#include "MethodTrampolines.h"
#include "ProviderMetadataAPI.h"
#include "ProviderTestAccess.h"
#include "TypeDatabaseBuilder.h"
#include "TypeInjection.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <utility>

using namespace CIProviderTestAccess;

namespace ClassInjectorFieldLayoutTests {
    struct ObjectInfo {
        intptr_t FieldBase;
        ci::Type* Type;
    };

    // 合成对象只用于验证公开 API 的 ABI、实例寻址与引用操作，不冒充 VM 创建的对象。
    struct Object {
        const ci::Type* const* Info;
        int32_t ReferenceCount;
        uint32_t HeaderPadding;
        std::array<uint8_t, 64> Fields;
    };

    static_assert(offsetof(Object, ReferenceCount) == 8 && offsetof(Object, Fields) == 16);

    static Object factoryObject{};

    // 只扫描这个合成 CALL，不执行缓冲区中的机器码。
    alignas(16) static std::array<uint8_t, 0x140> setupControlCode{};
    alignas(16) static std::array<uint8_t, 0x90> setBodyCode{};

    struct ActionArguments {
        void* VmContext;
        const ci::Type* ActionType;
        void* Target;
        const ci::MethodDef* Method;
        bool HeapAllocate;
    };

    static ActionArguments actionArguments{};
    static uint32_t actionCalls{};
    static RE::SystemString stringObject{};
    static void* stringVmContext{};
    static int32_t stringUnits{};
    static uint32_t stringCalls{};
    static bool stringAllocatorEnabled{true};
    static bool stringAllocationFails{};
    static bool stringLengthInvalid{};

    static RE::SystemString* FastAllocateStringFixture(void* vmContext, int32_t length) {
        stringVmContext = vmContext;
        stringUnits = length;
        ++stringCalls;
        if (stringAllocationFails || length < 0 || length >= std::size(stringObject.data))
            return nullptr;
        stringObject.size = length + (stringLengthInvalid ? 1 : 0);
        stringObject.referenceCount = 7;
        std::fill(std::begin(stringObject.data), std::end(stringObject.data), wchar_t(0x5A5A));
        return &stringObject;
    }

    static void* CreateActionFixture(void* vmContext, const ci::Type* type, void* target, const ci::MethodDef* method,
                                     bool heapAllocate) {
        actionArguments = {vmContext, type, target, method, heapAllocate};
        ++actionCalls;
        return &factoryObject;
    }

    static const ci::Type* GetFixtureType(const char* name) {
        return std::strcmp(name, "app.GUI030205PartsBox") == 0 ||
                       std::strcmp(name, "app.cCountedGameObjectHolder") == 0 ||
                       (stringAllocatorEnabled && std::strcmp(name, "System.String") == 0)
                   ? &ci::g_database->types[3]
                   : nullptr;
    }

    static void* GetFixtureMethod(const ci::Type* type, const char* signature) {
        if (std::strcmp(signature, "set_Body(via.GameObject)") == 0)
            return setBodyCode.data();
        if (std::strcmp(signature, "FastAllocateString(System.Int32)") == 0)
            return reinterpret_cast<void*>(&FastAllocateStringFixture);
        return std::strcmp(signature, "setupControl()") == 0 ? setupControlCode.data() : nullptr;
    }

    static std::vector<std::pair<char, void*>> referenceEvents;

    static void RetainFixtureObject(void* object) {
        referenceEvents.emplace_back('+', object);
        ++static_cast<Object*>(object)->ReferenceCount;
    }

    static void ReleaseFixtureObject(void* object) {
        referenceEvents.emplace_back('-', object);
        --static_cast<Object*>(object)->ReferenceCount;
    }

    // 仅本测试进程临时替换真实表中的生命周期入口；不新增生产测试接口。
    struct RestoreContext {
        ci::SDKAPI& Published;
        ci::SDKAPI Original;

        ~RestoreContext() {
            Published = Original;
        }
    };

    static ci::TypeDefinitionHandle FixtureFindType(const char* name) {
        return Handle(GetFixtureType(name));
    }

    static ci::MethodHandle FixtureFindMethod(ci::TypeDefinitionHandle type, const char* name) {
        auto* entry = GetFixtureMethod(reinterpret_cast<const ci::Type*>(type), name);
        return entry ? reinterpret_cast<ci::MethodHandle>(entry)
                     : ci::GetMetadataAPI().type_definition->find_method(type, name);
    }

    static void* FixtureGetFunction(ci::MethodHandle method) {
        auto* address = reinterpret_cast<void*>(const_cast<ci::MethodHandleTag*>(method));
        if (address == setupControlCode.data() || address == setBodyCode.data() ||
            address == reinterpret_cast<void*>(&FastAllocateStringFixture))
            return address;
        return ci::GetMetadataAPI().method->get_function(method);
    }

    // 用最小原生表验证零类型 provider 仍走 metadata 发布、原生完成边界和一次性通知。
    // 不伪造自定义类型；回调中的 ProviderContext 必须能查询原有 System.Object。
    static bool CheckZeroTypeProviderNotification() {
        std::array<ci::Type, 2> types{};
        std::array<RE::RETypeImpl, 2> impls{};
        std::array<ci::MethodDef, 1> methods{};
        std::array<ci::MethodImpl, 1> methodImpls{};
        std::array<ci::FieldDef, 1> fields{};
        std::array<ci::FieldImpl, 1> fieldImpls{};
        std::array<ci::Parameter, 1> parameters{};
        std::array<uint8_t, 4> bytes{};
        char names[] = "\0Object\0System";
        types[1].index = types[1].implIndex = 1;
        types[1].objectType = 1;
        impls[1].nameOffset = 1;
        impls[1].namespaceOffset = 8;
        RE::TDB original{.version = 81,
                         .numTypes = uint32_t(types.size()),
                         .numMethods = uint32_t(methods.size()),
                         .numFields = uint32_t(fields.size()),
                         .numTypeImpl = uint32_t(impls.size()),
                         .numFieldImpl = uint32_t(fieldImpls.size()),
                         .numMethodImpl = uint32_t(methodImpls.size()),
                         .numParams = uint32_t(parameters.size()),
                         .numStringPool = uint32_t(sizeof(names)),
                         .numBytePool = uint32_t(bytes.size()),
                         .types = types.data(),
                         .typeImpls = impls.data(),
                         .methods = methods.data(),
                         .methodImpls = methodImpls.data(),
                         .fields = fields.data(),
                         .fieldImpls = fieldImpls.data(),
                         .params = parameters.data(),
                         .stringPool = names,
                         .bytePool = bytes.data()};
        ci::g_database = ci::BuildTypeDatabase(original, {});
        auto& database = *ci::g_database;
        ci::CreateMethodTrampolines(database);
        ci::PrepareInvokeAdapters(database);
        auto published = original;
        database.Publish(published);
        static size_t calls;
        static const ci::Type* observedObject;
        static bool observedReady;
        calls = 0;
        observedObject = nullptr;
        observedReady = false;
        std::vector<ci::ClassProviderNotification> callbacks{{[](const ci::ProviderContext* context) noexcept {
            ++calls;
            observedReady = ci::g_ready.load();
            observedObject = QueryType(context, "System.Object");
        }}};
        const bool passed = database.customTypes.empty() && database.customMethods.empty() &&
                            published.numTypes == original.numTypes && published.types == database.types.data() &&
                            !ci::CompleteRuntimeInitialization(callbacks) && !calls && callbacks.size() == 1 &&
                            ci::FinalizeRuntimeTypes() && !calls && !ci::g_ready &&
                            ci::CompleteRuntimeInitialization(callbacks) && calls == 1 && observedReady &&
                            observedObject == &database.types[1] && callbacks.empty() &&
                            ci::CompleteRuntimeInitialization(callbacks) && calls == 1;
        ci::g_ready = false;
        ci::g_database.reset();
        return passed;
    }
} // namespace ClassInjectorFieldLayoutTests

int main() {
    using namespace ClassInjectorFieldLayoutTests;
    if (!CheckZeroTypeProviderNotification()) {
        std::cerr << "zero-type provider publication or notification lifecycle failed\n";
        return 1;
    }
    std::cout << "PASS: zero-type provider metadata publication, native lifecycle gating and one-shot ProviderContext "
                 "notification\n";
    ci::g_database = std::make_unique<ci::TypeDatabase>();
    auto& database = *ci::g_database;
    database.types.resize(7);
    database.typeImpls.resize(7);
    database.fields.resize(3);
    database.fieldImpls.resize(3);
    const char names[] = "Counter\0Bonus\0Reference";
    database.strings.assign(names, names + sizeof(names));
    for (uint32_t i = 0; i < database.types.size(); ++i) {
        database.types[i].index = i;
        database.types[i].implIndex = i;
    }
    const auto typeNameOffset = database.strings.size();
    const char typeNames[] = "Enum\0System\0Object";
    database.strings.insert(database.strings.end(), typeNames, typeNames + sizeof(typeNames));
    database.typeImpls[4].nameOffset = uint32_t(typeNameOffset);
    database.typeImpls[4].namespaceOffset = uint32_t(typeNameOffset + sizeof("Enum"));
    database.typeImpls[6].nameOffset = uint32_t(typeNameOffset + sizeof("Enum") + sizeof("System"));
    database.typeImpls[6].namespaceOffset = database.typeImpls[4].namespaceOffset;
    database.typeNames.emplace("System.Enum", 4);
    database.types[2].parentTypeIndex = 6;
    database.types[4].parentTypeIndex = 6;
    database.types[5].parentTypeIndex = 4;
    database.types[1].objectType = 5;
    database.typeImpls[1].fieldSize = 4;
    database.typeImpls[2].numMemberFields = 1;
    database.types[3].parentTypeIndex = 2;
    database.types[3].memberFieldIndex = 1;
    database.typeImpls[3].numMemberFields = 2;
    database.fields[0].declaringType = 2;
    database.fields[0].type = 1;
    database.fields[1].declaringType = 3;
    database.fields[1].type = 1;
    database.fields[1].impl = 1;
    database.fields[2].declaringType = 3;
    database.fields[2].type = 3;
    database.fields[2].impl = 2;
    database.fieldImpls[0].name = 0;
    database.fieldImpls[0].attributes = 0x10; // Not the engine's static flag.
    database.fieldImpls[1].name = 8;
    database.fieldImpls[1].offset = 8;
    database.fieldImpls[2].name = 14;
    database.fieldImpls[2].offset = 16;
    // Declaration lookup is usable before ObjectInfo exists, without a layout adapter or cache.
    if (database.GetFieldDef(&database.types[3], "Counter") != &database.fields[0] ||
        database.GetFieldDef(&database.types[2], "Counter") != &database.fields[0] ||
        database.GetFieldDef(&database.types[3], "Bonus") != &database.fields[1] ||
        database.GetFieldDef(&database.types[2], "Bonus")) {
        std::cerr << "field declaration lookup must use final tables without runtime shapes or caches\n";
        return 1;
    }
    database.fieldImpls[0].flags |= 0x10;
    if (database.GetFieldDef(&database.types[3], "Counter")) {
        std::cerr << "field declaration lookup must exclude static fields\n";
        return 1;
    }
    database.fieldImpls[0].flags &= ~0x10;
    ObjectInfo defaultInfo{.FieldBase = 16, .Type = &database.types[3]};
    database.types[3].objectInfo = reinterpret_cast<void**>(&defaultInfo.Type);
    // The abstract declaring parent deliberately has no ObjectInfo.
    const auto* child = &database.types[3];
    const auto* publishedAddress = &ci::GetContext();
    auto& context = const_cast<ci::ProviderContext&>(*publishedAddress);
    auto& publishedSDK = const_cast<ci::SDKAPI&>(*context.sdk);
    const RestoreContext restore{publishedSDK, publishedSDK};
    auto testTDB = *publishedSDK.tdb;
    auto testType = *publishedSDK.type_definition;
    auto testMethod = *publishedSDK.method;
    publishedSDK.tdb = &testTDB;
    publishedSDK.type_definition = &testType;
    publishedSDK.method = &testMethod;
    const auto wasReady = ci::g_ready.load();
    // 在独立 EXE 中调用真实准备流程；没有游戏 VM，未找到原生入口是预期场景。
    ci::PrepareContext();
    if (&ci::GetContext() != publishedAddress || ci::g_ready.load() != wasReady || !context.sdk->tdb->find_type ||
        !context.sdk->method->get_function || !context.sdk->type_definition->find_field ||
        !context.sdk->managed_object->get_type_definition || !context.sdk->field->get_data_raw ||
        !context.sdk->type_definition->is_derived_from || !context.sdk->type_definition->is_enum ||
        context.sdk->managed_object->create_instance || context.sdk->managed_object->assign_reference) {
        std::cerr << "context publication changed its address, readiness, or fixed "
                     "service slots\n";
        return 1;
    }
    if (!IsDerived(context, child, child) || !IsDerived(context, child, &database.types[2]) ||
        !IsDerived(context, child, &database.types[6]) || IsDerived(context, &database.types[2], child) ||
        IsDerived(context, child, &database.types[4]) || IsDerived(context, nullptr, child) ||
        IsDerived(context, child, nullptr) || IsEnum(context, nullptr) || IsEnum(context, child) ||
        IsEnum(context, &database.types[4]) || !IsEnum(context, &database.types[5])) {
        std::cerr << "type queries changed self/parent-chain/enum semantics\n";
        return 1;
    }
    database.typeNames.erase("System.Enum");
    if (IsEnum(context, &database.types[5])) {
        std::cerr << "enum query accepted a missing System.Enum definition\n";
        return 1;
    }
    database.typeNames.emplace("System.Enum", 4);
    const ci::API::TypeDefinition childType{context.sdk, Handle(child)};
    const ci::API::TypeDefinition parentType{context.sdk, Handle(&database.types[2])};
    if (QueryType(context, nullptr) || QueryMethod(context, nullptr, nullptr) ||
        context.sdk->type_definition->find_field(nullptr, "Counter") || childType.find_field(nullptr) ||
        context.sdk->type_definition->find_field(nullptr, nullptr)) {
        std::cerr << "null metadata input contract failed\n";
        return 1;
    }
    // 通用声明查询包含 static；实例寻址不能将其当作对象字段。
    database.fieldImpls[0].flags |= 0x10;
    const auto staticCounter = childType.find_field("Counter");
    if (!staticCounter || !staticCounter.is_static() || staticCounter.get_data_raw(nullptr)) {
        std::cerr << "static field metadata or instance access contract failed\n";
        return 1;
    }
    database.fieldImpls[0].flags &= ~0x10;
    const auto counter = childType.find_field("Counter");
    const auto bonus = childType.find_field("Bonus");
    const auto reference = childType.find_field("Reference");
    if (!counter || !bonus || !reference || counter.handle == bonus.handle ||
        counter.get_type().handle != Handle(&database.types[1]) || counter.get_offset_from_fieldptr() != 0 ||
        counter.get_declaring_type().handle != parentType.handle || counter.get_offset_from_base() != 0 ||
        counter.is_static() || counter.get_flags() != 0 || counter.get_type().get_valuetype_size() != 4 ||
        bonus.get_declaring_type().handle != childType.handle || bonus.get_offset_from_fieldptr() != 8 ||
        bonus.get_offset_from_base() != 24 || bonus.get_type().get_valuetype_size() != 4 ||
        reference.get_declaring_type().handle != childType.handle || reference.get_offset_from_fieldptr() != 16 ||
        reference.get_offset_from_base() != 32 || reference.get_type().get_valuetype_size() ||
        childType.find_field("Missing")) {
        std::cerr << "declaring type, inherited field or offset getter contract failed\n";
        return 1;
    }
    // 直接比较真实声明句柄与枚举结果；不重建已删除的布局缓存来验证自身。
    const auto declared = childType.get_fields();
    const auto inherited = parentType.get_fields();
    const auto copiedCounter = counter;
    if (declared.size() != 2 || inherited.size() != 1 || declared[0].handle != bonus.handle ||
        declared[1].handle != reference.handle || inherited[0].handle != counter.handle ||
        childType.find_field("Counter").handle != copiedCounter.handle ||
        childType.find_field("Bonus").handle != bonus.handle) {
        std::cerr << "declared field enumeration or copied handle identity changed\n";
        return 1;
    }

    ObjectInfo actualInfo{.FieldBase = 16, .Type = &database.types[3]};
    Object object{.Info = &actualInfo.Type, .ReferenceCount = 1};
    static_assert(std::is_same_v<decltype(counter.get_data<int32_t>(&object)), int32_t&>);
    static_assert(std::is_same_v<decltype(reference.get_data<Object*>(&object)), Object*&>);
    const ci::API::ManagedObject instance{context.sdk, &object};
    static_assert(std::is_same_v<decltype(instance.get_field<Object*>("Reference")), Object**>);
    // 核对当前函数表的布局和已发布服务，不保留历史接口适配测试。
    if (sizeof(context) != sizeof(void*) || !context.sdk->field->get_data_raw ||
        !context.sdk->type_definition->get_valuetype_size || ValueSize(context, &database.types[1]) != 4 ||
        ValueSize(context, child) != 0 || ValueSize(context, nullptr) != 0) {
        std::cerr << "typed field access changed the ProviderContext binary layout\n";
        return 1;
    }
    // 开放泛型或没有 payload 布局的值类型不能被当成可写入的闭合值。
    auto openValue = database.types[1];
    openValue.typeFlags |= 0x08000000;
    auto noLayoutValue = database.types[1];
    noLayoutValue.implIndex = 0; // 合成表第0项的 fieldSize 保持0。
    if (ValueSize(context, &openValue) != 0 || ValueSize(context, &noLayoutValue) != 0) {
        std::cerr << "value-size query accepted open or missing layout\n";
        return 1;
    }
    for (const intptr_t base : {16, 40}) {
        actualInfo.FieldBase = base;
        auto* bytes = reinterpret_cast<uint8_t*>(&object);
        if (InstanceType(context, &object) != child || counter.get_data_raw(&object) != bytes + base ||
            bonus.get_data_raw(&object) != bytes + base + 8 ||
            &counter.get_data<int32_t>(&object) != reinterpret_cast<int32_t*>(bytes + base) ||
            instance.get_field<int32_t>("Counter") != reinterpret_cast<int32_t*>(bytes + base) ||
            &reference.get_data<Object*>(&object) != reinterpret_cast<Object**>(bytes + base + 16) ||
            instance.get_field<Object*>("Reference") != reinterpret_cast<Object**>(bytes + base + 16) ||
            bonus.get_offset_from_base() != 24) {
            std::cerr << "instance field address reused the default type layout\n";
            return 1;
        }
        const int32_t marker = static_cast<int32_t>(base + 123);
        bonus.get_data<int32_t>(&object) = marker;
        int32_t actual{};
        std::memcpy(&actual, bytes + base + 8, sizeof(actual));
        if (actual != marker) {
            std::cerr << "instance field address did not select the writable "
                         "field slot\n";
            return 1;
        }
    }
    std::array<uint8_t, 32> payload{};
    if (counter.get_data_raw(payload.data(), true) != payload.data() ||
        bonus.get_data_raw(payload.data(), true) != payload.data() + 8 ||
        reference.get_data_raw(payload.data(), true) != payload.data() + 16) {
        std::cerr << "unboxed field addressing added an object header\n";
        return 1;
    }
    const ci::API::ManagedObject nullObject{context.sdk, nullptr};
    if (InstanceType(context, nullptr) || bonus.get_data_raw(nullptr) ||
        context.sdk->field->get_data_raw(nullptr, &object, false) || nullObject.get_field<int32_t>("Counter") ||
        instance.get_field<int32_t>("Missing") || instance.get_field<int32_t>(nullptr) ||
        context.sdk->managed_object->create_instance) {
        std::cerr << "null object, field, or missing CLR allocator was accepted\n";
        return 1;
    }

    // 独立测试 EXE 没有 RSZ/CLR 分配入口。不得退回 TypeInfo slot 1：
    // 它返回的是 native 对象，无法作为托管行交给 the managed object/field APIs。
    if (context.sdk->managed_object->create_instance) {
        std::cerr << "missing CLR allocator published a native factory\n";
        return 1;
    }
    factoryObject.ReferenceCount = 7;

    // 原生能力缺失时只发布基础元数据查询，不用非空包装掩盖不可用服务。
    void* slot = nullptr;
    if (!childType.find_field("Bonus") || context.sdk->managed_object->assign_reference) {
        std::cerr << "missing lifetime capabilities broke metadata queries or "
                     "allowed reference assignment\n";
        return 1;
    }
    // 合成 setter 同时提供 CALL 与尾 JMP，真实绑定器解码到既有引用操作。
    const auto retainDisplacement =
        reinterpret_cast<intptr_t>(&RetainFixtureObject) - reinterpret_cast<intptr_t>(setBodyCode.data() + 0x15);
    const auto releaseDisplacement =
        reinterpret_cast<intptr_t>(&ReleaseFixtureObject) - reinterpret_cast<intptr_t>(setBodyCode.data() + 0x2D);
    if (retainDisplacement < std::numeric_limits<int32_t>::min() ||
        retainDisplacement > std::numeric_limits<int32_t>::max() ||
        releaseDisplacement < std::numeric_limits<int32_t>::min() ||
        releaseDisplacement > std::numeric_limits<int32_t>::max()) {
        std::cerr << "synthetic reference CALL/JMP is out of rel32 range\n";
        return 1;
    }
    setBodyCode[0x10] = 0xE8;
    const auto retainRelative = static_cast<int32_t>(retainDisplacement);
    std::memcpy(setBodyCode.data() + 0x11, &retainRelative, sizeof(retainRelative));
    setBodyCode[0x15] = 0x48;
    setBodyCode[0x16] = 0x8B;
    setBodyCode[0x28] = 0xE9;
    const auto releaseRelative = static_cast<int32_t>(releaseDisplacement);
    std::memcpy(setBodyCode.data() + 0x29, &releaseRelative, sizeof(releaseRelative));
    setBodyCode[0x2D] = 0x48;
    setBodyCode[0x2E] = 0x89;
    stringAllocatorEnabled = false; // 本段只准备引用入口，字符串分配器在后续独立验收。
    const auto getType = testTDB.find_type;
    const auto getMethod = testMethod.get_function;
    const auto findMethod = testType.find_method;
    testTDB.find_type = &FixtureFindType;
    testType.find_method = &FixtureFindMethod;
    testMethod.get_function = &FixtureGetFunction;
    // 分别缺 CALL、缺尾 JMP，以及两者错误地指向同一入口时均不发布引用能力。
    for (const auto missingOpcode : {size_t(0x10), size_t(0x28)}) {
        const auto opcode = setBodyCode[missingOpcode];
        setBodyCode[missingOpcode] = 0;
        ci::PrepareContext();
        if (context.sdk->managed_object->assign_reference || context.sdk->managed_object->add_ref ||
            context.sdk->managed_object->release) {
            std::cerr << "partial lifetime entries must not publish reference capabilities\n";
            return 1;
        }
        setBodyCode[missingOpcode] = opcode;
    }
    const auto duplicateRelative = static_cast<int32_t>(reinterpret_cast<intptr_t>(&RetainFixtureObject) -
                                                        reinterpret_cast<intptr_t>(setBodyCode.data() + 0x2D));
    std::memcpy(setBodyCode.data() + 0x29, &duplicateRelative, sizeof(duplicateRelative));
    ci::PrepareContext();
    if (context.sdk->managed_object->assign_reference || context.sdk->managed_object->add_ref ||
        context.sdk->managed_object->release) {
        std::cerr << "identical retain/release entries must not publish reference capabilities\n";
        return 1;
    }
    std::memcpy(setBodyCode.data() + 0x29, &releaseRelative, sizeof(releaseRelative));
    ci::PrepareContext();
    if (!context.sdk->managed_object->assign_reference ||
        context.sdk->managed_object->add_ref != &RetainFixtureObject ||
        context.sdk->managed_object->release != &ReleaseFixtureObject) {
        std::cerr << "complete lifetime entries must publish reference assignment\n";
        return 1;
    }
    testTDB.find_type = getType;
    testMethod.get_function = getMethod;
    testType.find_method = findMethod;
    stringAllocatorEnabled = true;
    Object previous{.ReferenceCount = 3};
    Object replacement{.ReferenceCount = 5};
    auto** typedReferenceSlot = instance.get_field<Object*>("Reference");
    if (*typedReferenceSlot || !referenceEvents.empty() || object.ReferenceCount != 1 ||
        !context.sdk->managed_object->assign_reference(&object, reinterpret_cast<void**>(typedReferenceSlot),
                                                       &replacement) ||
        *typedReferenceSlot != &replacement || replacement.ReferenceCount != 6 ||
        !context.sdk->managed_object->assign_reference(&object, reinterpret_cast<void**>(typedReferenceSlot),
                                                       nullptr) ||
        *typedReferenceSlot || replacement.ReferenceCount != 5 ||
        referenceEvents != std::vector<std::pair<char, void*>>{{'+', &replacement}, {'-', &replacement}}) {
        std::cerr << "typed reference slot changed pointer level or bypassed "
                     "reference assignment\n";
        return 1;
    }
    referenceEvents.clear();
    slot = &previous;
    if (!context.sdk->managed_object->assign_reference(&object, &slot, &replacement) || slot != &replacement ||
        previous.ReferenceCount != 2 || replacement.ReferenceCount != 6 ||
        referenceEvents != std::vector<std::pair<char, void*>>{{'+', &replacement}, {'-', &previous}}) {
        std::cerr << "reference assignment did not retain new before releasing old\n";
        return 1;
    }
    referenceEvents.clear();
    if (!context.sdk->managed_object->assign_reference(&object, &slot, &replacement) || !referenceEvents.empty() ||
        previous.ReferenceCount != 2 || replacement.ReferenceCount != 6) {
        std::cerr << "same-reference assignment changed ownership\n";
        return 1;
    }
    if (!context.sdk->managed_object->assign_reference(&object, &slot, nullptr) || slot ||
        replacement.ReferenceCount != 5 ||
        referenceEvents != std::vector<std::pair<char, void*>>{{'-', &replacement}}) {
        std::cerr << "null assignment did not release the previous object\n";
        return 1;
    }
    referenceEvents.clear();
    object.ReferenceCount = -1;
    slot = &previous;
    if (!context.sdk->managed_object->assign_reference(&object, &slot, &replacement) || slot != &replacement ||
        !referenceEvents.empty() || previous.ReferenceCount != 2 || replacement.ReferenceCount != 5 ||
        context.sdk->managed_object->assign_reference(nullptr, &slot, &replacement) ||
        context.sdk->managed_object->assign_reference(&object, nullptr, &replacement)) {
        std::cerr << "temporary-owner or null-input reference assignment contract "
                     "failed\n";
        return 1;
    }

    // 真实绑定器从合成 setupControl 的相对 CALL 定位原生入口。
    // 真实查询缓存仍返回 MethodDef，验证没有把 GetMethod 的函数地址当作声明。
    if (context.sdk->managed_object->create_action || context.sdk->managed_object->create_string) {
        std::cerr << "missing Action/String factories should leave empty "
                     "service slots\n";
        return 1;
    }
    const auto displacement =
        reinterpret_cast<intptr_t>(&CreateActionFixture) - reinterpret_cast<intptr_t>(setupControlCode.data() + 8);
    if (displacement < std::numeric_limits<int32_t>::min() || displacement > std::numeric_limits<int32_t>::max()) {
        std::cerr << "synthetic Action CALL is out of rel32 range\n";
        return 1;
    }
    setupControlCode[0] = 0x49;
    setupControlCode[1] = 0x89;
    setupControlCode[2] = 0xF8;
    setupControlCode[3] = 0xE8;
    const auto relative = static_cast<int32_t>(displacement);
    std::memcpy(setupControlCode.data() + 4, &relative, sizeof(relative));
    database.methods.resize(3);
    database.methods[0].encodedOffset = 17;
    database.methods[1].encodedOffset = 33;
    database.methodQueries[child]["Callback()"] = &database.methods[0];
    database.methodQueries[child]["StaticCallback()"] = &database.methods[1];
    database.methodQueries[child]["NoEntry()"] = &database.methods[2];
    database.methodQueries[child]["Missing()"] = nullptr;
    database.encodingBase = reinterpret_cast<uintptr_t>(setupControlCode.data());
    testTDB.find_type = &FixtureFindType;
    testType.find_method = &FixtureFindMethod;
    testMethod.get_function = &FixtureGetFunction;
    ci::PrepareContext();
    void* const vmContext = &actualInfo;
    if (!context.sdk->managed_object->create_action || context.sdk->managed_object->add_ref != &RetainFixtureObject ||
        context.sdk->managed_object->release != &ReleaseFixtureObject ||
        CreateAction(context, vmContext, child, &object, child, "Callback()", false) != &factoryObject ||
        actionCalls != 1 || actionArguments.VmContext != vmContext || actionArguments.ActionType != child ||
        actionArguments.Target != &object || actionArguments.Method != &database.methods[0] ||
        actionArguments.HeapAllocate || factoryObject.ReferenceCount != 7 ||
        QueryMethod(database, child, "Callback()") != setupControlCode.data() + 17) {
        std::cerr << "Action creation did not pass the declaration and "
                     "original native arguments\n";
        return 1;
    }
    if (CreateAction(context, vmContext, child, nullptr, child, "StaticCallback()", true) != &factoryObject ||
        actionCalls != 2 || actionArguments.Target || !actionArguments.HeapAllocate ||
        actionArguments.Method != &database.methods[1] ||
        CreateAction(context, nullptr, child, &object, child, "Callback()", false) ||
        CreateAction(context, vmContext, nullptr, &object, child, "Callback()", false) ||
        CreateAction(context, vmContext, child, &object, nullptr, "Callback()", false) ||
        CreateAction(context, vmContext, child, &object, child, nullptr, false) ||
        CreateAction(context, vmContext, child, &object, child, "Missing()", false) ||
        CreateAction(context, vmContext, child, &object, child, "NoEntry()", false) || actionCalls != 2) {
        std::cerr << "Action static target, heap flag, or invalid metadata "
                     "handling changed\n";
        return 1;
    }
    // 合成分配器只提供缓冲区；转换、长度、终止符写入都由生产 CreateString
    // 完成。
    static_assert(std::is_same_v<decltype(ci::ManagedObject::create_string), void* (*)(void*, const char*)>);
    if (!context.sdk->managed_object->create_string) {
        std::cerr << "resolved string factory did not publish CreateString\n";
        return 1;
    }
    const std::array<std::pair<std::string, std::wstring>, 4> strings{
        {{"Em0057_00", L"Em0057_00"},
         {"雷狼", L"雷狼"},
         {"🐺", L"🐺"},
         {std::string("a\0b", 3), std::wstring(L"a\0b", 3)}}};
    for (const auto& [utf8, expected] : strings) {
        if (RE::widen(utf8, MB_ERR_INVALID_CHARS) != expected || RE::widen(utf8) != expected) {
            std::cerr << "common widen changed UTF-8 content or explicit length\n";
            return 1;
        }
        // common 保留显式长度语义；CreateString 按 C 字符串读取至首个 NUL。
        const std::wstring_view expectedString(expected.c_str());
        auto* result =
            static_cast<RE::SystemString*>(context.sdk->managed_object->create_string(vmContext, utf8.c_str()));
        if (result != &stringObject || stringVmContext != vmContext || stringUnits != expectedString.size() ||
            result->size != expectedString.size() || result->referenceCount != 7 ||
            std::wstring_view(result->data, result->size) != expectedString || result->data[result->size] != L'\0' ||
            result->data[result->size + 1] != wchar_t(0x5A5A)) {
            std::cerr << "CreateString UTF-8/content/length/terminator/ownership "
                         "contract failed\n";
            return 1;
        }
    }
    if (context.sdk->managed_object->create_string(vmContext, "") != &stringObject || stringObject.size ||
        stringObject.data[0]) {
        std::cerr << "CreateString did not create an empty string\n";
        return 1;
    }
    // 路径使用严格模式；原有单参数调用仍保留 Win32 的非法序列替换行为。
    const std::array<char, 4> unterminated{'a', 'b', 'c', 'd'};
    if (!RE::widen({}, MB_ERR_INVALID_CHARS).empty() || !RE::widen("\xC0\xAF", MB_ERR_INVALID_CHARS).empty() ||
        !RE::widen("\xE2\x82", MB_ERR_INVALID_CHARS).empty() || RE::widen("\xC0\xAF").empty() ||
        RE::widen(std::string_view(unterminated.data(), 2), MB_ERR_INVALID_CHARS) != L"ab") {
        std::cerr << "common widen strict/default UTF-8 or bounded input contract failed\n";
        return 1;
    }
    std::cout << "PASS: common widen strict/default UTF-8, empty input, Chinese, surrogate pairs, embedded NUL and "
                 "bounded views\n";
    const auto previousStringCalls = stringCalls;
    if (context.sdk->managed_object->create_string(nullptr, "a") ||
        context.sdk->managed_object->create_string(vmContext, nullptr) ||
        context.sdk->managed_object->create_string(vmContext, "\xC0\xAF") ||
        context.sdk->managed_object->create_string(vmContext, "\xE2\x82") || stringCalls != previousStringCalls) {
        std::cerr << "invalid string input reached the allocator\n";
        return 1;
    }
    stringAllocationFails = true;
    if (context.sdk->managed_object->create_string(vmContext, "a")) {
        std::cerr << "failed native string allocation was accepted\n";
        return 1;
    }
    stringAllocationFails = false;
    stringLengthInvalid = true;
    if (context.sdk->managed_object->create_string(vmContext, "a") || stringObject.data[0] != wchar_t(0x5A5A)) {
        std::cerr << "unexpected native string length was written\n";
        return 1;
    }
    stringLengthInvalid = false;
    stringAllocatorEnabled = false;
    setupControlCode.fill(0);
    setBodyCode[0x28] = 0; // 只留下 retain 时仍须清空整对引用服务。
    ci::PrepareContext();
    if (context.sdk->managed_object->create_action || context.sdk->managed_object->create_string ||
        context.sdk->managed_object->add_ref || context.sdk->managed_object->release ||
        context.sdk->managed_object->assign_reference) {
        std::cerr << "missing Action/String factory or incomplete reference "
                     "pair left callable services\n";
        return 1;
    }
    std::cout
        << "PASS: ProviderContext prefix/lifetime, self/parent-chain/enum queries, abstract-parent fields, actual "
           "instance layouts, missing CLR allocator, "
           "declared field handles/getters, typed field slots/current ABI, UTF-8 "
           "strings, Action declaration/ABI, missing "
           "capabilities, retain/CAS/release ownership (synthetic "
           "objects; no live VM)\n";
    return 0;
}
