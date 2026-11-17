#include "InvokeAdapters.h"
#include "CustomTypeConfig.h"
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <HookUtils.h>
#include <LogUtils.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ci {
extern "C" void __chkstk();

static InvokeValueKind ClassifyInvokeValue(const TypeDatabase& database, uint32_t index) {
    if (!index) return InvokeValueKind::Void;
    const auto& type = database.types[index];
    if (type.objectType >= 1 && type.objectType <= 4) return InvokeValueKind::Pointer;
    const auto name = database.GetFullName(index);
    // 枚举按其真实底层整数宽度；普通 struct 不因大小碰巧为 4/8B 就归入整数返回。
    if (type.parentTypeIndex && database.GetFullName(uint32_t(type.parentTypeIndex)) == "System.Enum") {
        switch (type.underlyingTypeIndex) {
        case 4: return InvokeValueKind::Int8;
        case 5: return InvokeValueKind::UInt8;
        case 6: return InvokeValueKind::Int16;
        case 7: return InvokeValueKind::UInt16;
        case 8: return InvokeValueKind::Int32;
        case 9: return InvokeValueKind::UInt32;
        case 10: return InvokeValueKind::Int64;
        case 11: return InvokeValueKind::UInt64;
        default: return InvokeValueKind::Unsupported;
        }
    }
    if (name == "System.Void") return InvokeValueKind::Void;
    if (name == "System.Boolean") return InvokeValueKind::Boolean;
    if (name == "System.SByte") return InvokeValueKind::Int8;
    if (name == "System.Byte") return InvokeValueKind::UInt8;
    if (name == "System.Int16") return InvokeValueKind::Int16;
    if (name == "System.UInt16" || name == "System.Char") return InvokeValueKind::UInt16;
    if (name == "System.Int32") return InvokeValueKind::Int32;
    if (name == "System.UInt32") return InvokeValueKind::UInt32;
    if (name == "System.Int64" || name == "System.IntPtr") return InvokeValueKind::Int64;
    if (name == "System.UInt64" || name == "System.UIntPtr") return InvokeValueKind::UInt64;
    if (name == "System.Single") return InvokeValueKind::Single;
    if (name == "System.Double") return InvokeValueKind::Double;
    return InvokeValueKind::Unsupported;
}

InvokeSignature ClassifyInvokeSignature(const TypeDatabase& database, const MethodSpec& method) {
    if (method.kind == MethodKind::Abstract) return {};
    InvokeSignature signature{.result = ClassifyInvokeValue(database, database.ResolveType(method.returnType))};
    if (signature.result == InvokeValueKind::Unsupported) return {};
    for (const auto& argument : method.parameters) {
        const auto index = database.ResolveType(argument.type);
        auto kind = argument.modifier == ParameterModifier::Value ? ClassifyInvokeValue(database, index) : InvokeValueKind::Pointer;
        // 引擎非 primitive 值参数沿用 payload 指针 ABI；ref/out 同样传地址，但元数据语义不混淆。
        if (kind == InvokeValueKind::Unsupported && database.types[index].objectType == 5) kind = InvokeValueKind::Pointer;
        if (kind == InvokeValueKind::Unsupported || kind == InvokeValueKind::Void) return {};
        signature.arguments.push_back(kind);
    }
    return signature;
}

InvokeAdapterCode::~InvokeAdapterCode() {
    if (registered) RtlDeleteFunctionTable(&function);
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
}

static std::unique_ptr<InvokeAdapterCode> GenerateInvokeAdapter(const TypeDatabase& database, const InvokeSignature& signature) {
    auto storage = std::make_unique<InvokeAdapterCode>();
    std::vector<uint8_t> code;
    auto Emit = [&](std::initializer_list<uint8_t> bytes) { code.insert(code.end(), bytes); };
    auto EmitImmediate = [&](auto value) { const auto* bytes = reinterpret_cast<const uint8_t*>(&value); code.insert(code.end(), bytes, bytes + sizeof(value)); };
    // context/self 占前两槽。固定大小栈帧按签名确定，调用前对齐 16B，始终包含 32B shadow space。
    const auto stackArgumentCount = signature.arguments.size() > 2 ? signature.arguments.size() - 2 : 0;
    const auto stackSize = uint32_t((32 + stackArgumentCount * 8 + 15) & ~size_t(15));
    Emit({0x53}); // push rbx，保留 frame 跨目标调用
    if (stackSize < 0x1000) {
        Emit({0x48, 0x81, 0xEC}); EmitImmediate(stackSize); // sub rsp,imm32
    } else {
        // MSVC __chkstk 保留参数寄存器；大参数表逐页探栈，避免跨过 Windows guard page。
        Emit({0xB8}); EmitImmediate(stackSize);
        Emit({0x49, 0xBB}); EmitImmediate(uintptr_t(&__chkstk));
        Emit({0x41, 0xFF, 0xD3, 0x48, 0x29, 0xC4}); // call r11; sub rsp,rax
    }
    const auto prologueSize = uint8_t(code.size());
    Emit({0x48, 0x89, 0xCB});                         // mov rbx,rcx
    Emit({0x48, 0x8B, 0x43, 0x10, 0x48, 0x63, 0x40, 0x08}); // 当前 MethodDef::encodedOffset
    Emit({0x49, 0xBA}); EmitImmediate(database.encodingBase);
    Emit({0x4C, 0x01, 0xD0, 0x4C, 0x8B, 0x5B, 0x30}); // rax=目标，r11=输入槽
    Emit({0x48, 0x89, 0xD1, 0x48, 0x8B, 0x53, 0x40}); // rcx=context，rdx=self
    for (size_t index = 0; index < signature.arguments.size(); ++index) {
        const auto kind = signature.arguments[index];
        const auto argumentOffset = uint32_t(index * 8);
        const auto stackArgumentOffset = uint32_t(0x20 + (index >= 2 ? index - 2 : 0) * 8);
        if (kind == InvokeValueKind::Single || kind == InvokeValueKind::Double) {
            Emit({0xF2, 0x41, 0x0F, 0x10, 0x83}); EmitImmediate(argumentOffset); // movsd xmm0,[r11+disp32]
            if (kind == InvokeValueKind::Single) Emit({0xF2, 0x0F, 0x5A, 0xC0}); // 通用 double -> Single
            if (index < 2) Emit({0x0F, 0x28, uint8_t(index == 0 ? 0xD0 : 0xD8)}); // xmm2/3 <- xmm0
            else { Emit({uint8_t(kind == InvokeValueKind::Single ? 0xF3 : 0xF2), 0x0F, 0x11, 0x84, 0x24}); EmitImmediate(stackArgumentOffset); }
            continue;
        }
        switch (kind) {
        case InvokeValueKind::Boolean:
            Emit({0x41, 0x83, 0xBB}); EmitImmediate(argumentOffset); Emit({0, 0x41, 0x0F, 0x95, 0xC2, 0x45, 0x0F, 0xB6, 0xD2}); break;
        case InvokeValueKind::Int8: Emit({0x45, 0x0F, 0xBE, 0x93}); EmitImmediate(argumentOffset); break;
        case InvokeValueKind::UInt8: Emit({0x45, 0x0F, 0xB6, 0x93}); EmitImmediate(argumentOffset); break;
        case InvokeValueKind::Int16: Emit({0x45, 0x0F, 0xBF, 0x93}); EmitImmediate(argumentOffset); break;
        case InvokeValueKind::UInt16: Emit({0x45, 0x0F, 0xB7, 0x93}); EmitImmediate(argumentOffset); break;
        case InvokeValueKind::Int32:
        case InvokeValueKind::UInt32: Emit({0x45, 0x8B, 0x93}); EmitImmediate(argumentOffset); break;
        default: Emit({0x4D, 0x8B, 0x93}); EmitImmediate(argumentOffset); break; // pointer / Int64 / UInt64
        }
        if (index < 2) Emit({0x4D, 0x89, uint8_t(index == 0 ? 0xD0 : 0xD1)}); // r8/9 <- r10
        else { Emit({0x4C, 0x89, 0x94, 0x24}); EmitImmediate(stackArgumentOffset); }
    }
    Emit({0xFF, 0xD0}); // call rax
    switch (signature.result) {
    case InvokeValueKind::Void: break;
    case InvokeValueKind::Single:
        Emit({0xF3, 0x0F, 0x5A, 0xC0}); // Single -> 通用 double 返回
        [[fallthrough]];
    case InvokeValueKind::Double: Emit({0xF2, 0x0F, 0x11, 0x43, 0x38}); break;
    case InvokeValueKind::Int8: Emit({0x0F, 0xBE, 0xC0, 0x89, 0x43, 0x38}); break;
    case InvokeValueKind::Boolean:
    case InvokeValueKind::UInt8: Emit({0x0F, 0xB6, 0xC0, 0x89, 0x43, 0x38}); break;
    case InvokeValueKind::Int16: Emit({0x0F, 0xBF, 0xC0, 0x89, 0x43, 0x38}); break;
    case InvokeValueKind::UInt16: Emit({0x0F, 0xB7, 0xC0, 0x89, 0x43, 0x38}); break;
    case InvokeValueKind::Int32:
    case InvokeValueKind::UInt32: Emit({0x89, 0x43, 0x38}); break;
    default: Emit({0x48, 0x89, 0x43, 0x38}); break;
    }
    Emit({0x48, 0x81, 0xC4}); EmitImmediate(stackSize); Emit({0x5B, 0xC3});
    // UNWIND_CODE 按逆序描述固定栈帧；ALLOC_LARGE 的额外槽不算额外操作。
    std::vector<uint8_t> unwind{1, prologueSize, 0, 0, prologueSize};
    if (stackSize <= 128) { unwind.push_back(uint8_t(((stackSize / 8 - 1) << 4) | 2)); unwind[2] = 2; }
    else if (stackSize / 8 <= UINT16_MAX) { unwind.push_back(1); unwind.push_back(uint8_t(stackSize / 8)); unwind.push_back(uint8_t(stackSize / 8 >> 8)); unwind[2] = 3; }
    else { unwind.push_back(0x11); for (size_t byte = 0; byte < 4; ++byte) unwind.push_back(uint8_t(stackSize >> (byte * 8))); unwind[2] = 4; }
    unwind.insert(unwind.end(), {1, 0x30}); // push rbx
    while (unwind.size() % 4) unwind.push_back(0);
    const auto unwindOffset = uint32_t((code.size() + 3) & ~size_t(3));
    const auto allocationSize = size_t(unwindOffset) + unwind.size();
    storage->memory = VirtualAlloc(nullptr, allocationSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!storage->memory) throw std::runtime_error("invoker code allocation failed");
    std::memcpy(storage->memory, code.data(), code.size());
    std::memcpy(static_cast<uint8_t*>(storage->memory) + unwindOffset, unwind.data(), unwind.size());
    DWORD oldProtect{};
    if (!VirtualProtect(storage->memory, allocationSize, PAGE_EXECUTE_READ, &oldProtect)) throw std::runtime_error("invoker code protection failed");
    FlushInstructionCache(GetCurrentProcess(), storage->memory, code.size());
    storage->function = {.BeginAddress = 0, .EndAddress = uint32_t(code.size()), .UnwindData = unwindOffset};
    if (!RtlAddFunctionTable(&storage->function, 1, DWORD64(storage->memory))) throw std::runtime_error("invoker unwind registration failed");
    storage->registered = true;
    return storage;
}

void PrepareInvokeAdapters(TypeDatabase& database) {
    auto signatures = std::exchange(database.invokeSignatures, {});
    std::vector<std::pair<InvokeSignature, FnInvoke>> adapters;
    size_t count{};
    for (size_t index = 0; index < signatures.size(); ++index) {
        auto& signature = signatures[index];
        if (signature.result == InvokeValueKind::Unsupported) continue;
        auto& method = database.customMethods[index];
        auto found = std::ranges::find_if(adapters, [&](const auto& value) { return value.first == signature; });
        if (found == adapters.end()) {
            auto storage = GenerateInvokeAdapter(database, signature);
            _MESSAGE("Generated invoker: entry=%p, bytes=%u, args=%zu, return=%u, unwind=1", storage->memory, storage->function.EndAddress, signature.arguments.size(), unsigned(signature.result));
            method.invokeAdapter = reinterpret_cast<FnInvoke>(storage->memory);
            database.invokeAdapterCodes.push_back(std::move(storage));
            adapters.emplace_back(std::move(signature), method.invokeAdapter);
        } else {
            method.invokeAdapter = found->second;
        }
        ++count;
    }
    if (count) _MESSAGE("Generated invokers ready: invokeId=%u, methods=%zu, unique shapes=%zu", database.invokeDispatchId, count, database.invokeAdapterCodes.size());
}

bool InvokeGeneratedMethod(InvokeFrame* frame, void* context) {
    if (!g_database) return false;
    auto& database = *g_database;
    const auto begin = uintptr_t(database.methods.data());
    const auto address = uintptr_t(frame->method);
    if (address < begin || address - begin >= database.methods.size() * sizeof(MethodDef) || (address - begin) % sizeof(MethodDef)) return false;
    auto* method = database.FindMethod(uint32_t((address - begin) / sizeof(MethodDef)));
    if (!method || !method->invokeAdapter) return false;
    method->invokeAdapter(frame, context);
    return true;
}

bool InstallInvokeDispatch(TypeDatabase& database, void* initializeRuntimeEntries) {
    const auto registration = HookUtils::LookupFunctionPattern(initializeRuntimeEntries, "48 8D 15 ? ? ? ? 48 89 F1 41 B8 ? ? ? ? E8 ? ? ? ?");
    if (!registration) {
        _MESSAGE("Invoke dispatch FAILED: native table registration not found");
        return false;
    }
    auto** table = reinterpret_cast<void**>(HookUtils::ReadOffsetData(registration, 3, 7));
    const auto count = RE::read_offset_data<uint32_t>(reinterpret_cast<void*>(registration), 12);
    if (database.invokeDispatchId >= count || !table[database.invokeDispatchId]) return false;
    auto invokeWrapper = reinterpret_cast<FnInvoke>(table[database.invokeDispatchId]);
    // 原型要求源 wrapper 的第五参数确为 Boolean；编号来自原 TDB，不硬编码版本地址。
    if (!HookUtils::LookupFunctionPattern(reinterpret_cast<void*>(invokeWrapper), "83 79 20 00 0F 95 44 24 30", 0x80)) {
        _MESSAGE("Invoke dispatch FAILED: five-argument Boolean wrapper contract differs, entry=%p", invokeWrapper);
        return false;
    }
    const auto installed = HookLambda(invokeWrapper, [](InvokeFrame* frame, void* context) {
        if (!InvokeGeneratedMethod(frame, context)) original(frame, context);
    });
    _MESSAGE("Invoke dispatch: installed=%d, table=%p, count=%u, id=%u, wrapper=%p; original table entries unchanged", installed, table, count, database.invokeDispatchId, invokeWrapper);
    return installed;
}
} // namespace ci
