#include "MethodTrampolines.h"
#include "CustomTypeConfig.h"
#include "TypeDatabase.h"
#include "TypeInjection.h"
#include <BranchTrampoline.h>
#include <HookUtils.h>
#include <LogUtils.h>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ci {
    using FnConstructor = void (*)(void*, void*);

    // Builtin 没有业务参数/实现，仅保留“省略构造等于调用父默认构造”的声明语义。
    static void CallBaseDefaultConstructor(void* context, void* self, RuntimeMethod* method) noexcept {
        try {
            auto& database = *g_database;
            auto parent = database.types[method->declaringType].parentTypeIndex;
            auto ctor = database.types[parent].defaultCtorIndex;
            if (ctor) {
                auto offset = database.methods[ctor].encodedOffset;
                if (!offset) {
                    throw std::runtime_error("base constructor entry is not installed");
                }
                reinterpret_cast<FnConstructor>(database.encodingBase + offset)(context, self);
            }
        } catch (const std::exception& error) {
            // 仅包装层内部错误在这里记录；provider 原生方法本身约定 noexcept。
            if (!InterlockedExchange(&method->reportedError, 1)) {
                _MESSAGE("Default constructor failed: %s: %s", g_database->GetFullName(method->declaringType).c_str(), error.what());
            }
        }
    }

    // 普通入口仅 jmp [rip+disp32]：隐藏返回缓冲、ref/out 指针、栈参数和返回地址均保持原布局。
    // 仅无参 Builtin 使用 R8 附带方法记录；显式构造的 R8/XMM2 等业务参数不被改写。
    // 使用公共 Write6Branch 写间接跳转，入口仍须位于 encodingBase 的 signed32 范围内。
    static void* CreateMethodTrampoline(TypeDatabase& database, RuntimeMethod& method, const MethodSpec& declaration, BranchTrampoline& page) {
        bool builtin = declaration.kind == MethodKind::Constructor && declaration.binding.backend == Backend::Builtin;
        auto size = builtin ? 16u : 6u;
        auto* entry = static_cast<uint8_t*>(page.Allocate(size));
        auto* jump = entry;
        auto target = uintptr_t(declaration.binding.address);
        if (builtin) {
            uint8_t code[10]{0x49, 0xB8}; // mov r8, imm64
            auto address = uintptr_t(&method);
            std::memcpy(code + 2, &address, sizeof(address));
            HookUtils::SafeWriteBuf(uintptr_t(entry), code, sizeof(code));
            jump += sizeof(code);
            target = uintptr_t(&CallBaseDefaultConstructor);
        }
        if (!page.Write6Branch(uintptr_t(jump), target)) {
            throw std::runtime_error("native entry branch write failed");
        }
        FlushInstructionCache(GetCurrentProcess(), entry, size);
        auto difference = int64_t(uintptr_t(entry)) - int64_t(database.encodingBase);
        if (difference < INT32_MIN || difference > INT32_MAX || difference == 0) {
            throw std::runtime_error("method entry is outside signed32 encoding range");
        }
        database.methods[method.index].encodedOffset = int32_t(difference);
        return entry;
    }

    // metadata 仅包含整份通过核对的插件；显式构造/普通方法直接尾跳登记地址。
    void CreateMethodTrampolines(TypeDatabase& database) {
        auto declarations = std::exchange(database.methodDeclarations, {});
        BranchTrampoline* entries = nullptr;
        size_t usedBytes = 0;
        size_t capacityBytes = 0;
        size_t entryCount = 0;
        size_t builtinCount = 0;
        for (size_t index = 0; index < declarations.size(); ++index) {
            const auto& declaration = declarations[index];
            auto& method = database.customMethods[index];
            try {
                if (declaration.kind == MethodKind::Abstract) {
                    continue;
                }
                if (declaration.binding.backend == Backend::Cpp && !declaration.binding.address) {
                    throw std::runtime_error("binding was not resolved: " + declaration.binding.function);
                }
                if (!entries || entries->Remain() < 32) {
                    auto page = std::make_unique<BranchTrampoline>();
                    // 最大入口 16 字节加 8 字节间接目标；公共 Create 要求单次 <1000，按 960 字节复用。
                    if (!page->Create(30 * 32)) {
                        throw std::runtime_error("near entry allocation failed");
                    }
                    entries = page.get();
                    capacityBytes += entries->Remain();
                    database.methodTrampolines.push_back(std::move(page));
                }
                const auto remainingBytes = entries->Remain();
                method.entry = CreateMethodTrampoline(database, method, declaration, *entries);
                usedBytes += remainingBytes - entries->Remain();
                ++entryCount;
                if (declaration.kind == MethodKind::Constructor && declaration.binding.backend == Backend::Builtin) {
                    ++builtinCount;
                }
            } catch (const std::runtime_error& error) {
                const auto owner = database.GetFullName(method.declaringType) + "." + declaration.name;
                const auto location = declaration.providerSource
                    ? declaration.providerSource->configPath + " [" + declaration.providerSource->dllPath + "]: " + owner : owner;
                throw std::runtime_error(location + ": " + error.what());
            }
        }
        _MESSAGE("Method trampolines: methods=%zu, entries=%zu, builtin constructors=%zu, blocks=%zu, used=%zu bytes, capacity=%zu bytes, remaining=%zu bytes",
                 declarations.size(), entryCount, builtinCount, database.methodTrampolines.size(),
                 usedBytes, capacityBytes, capacityBytes - usedBytes);
    }
} // namespace ci
