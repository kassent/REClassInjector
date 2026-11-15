#pragma once
#include "TypeDatabaseRecords.h"
#include <Windows.h>
#include <cstddef>
#include <vector>

namespace ci {
    struct TypeDatabase;
    struct MethodSpec;
    // 引擎和 RF 共用的反射调用帧；参数槽不含 context / self。
    struct InvokeFrame {
        uint8_t reserved00[0x10];
        const MethodDef* method;
        uint8_t reserved18[0x18];
        const uintptr_t* arguments;
        void* result;
        void* self;
    };
    static_assert(offsetof(InvokeFrame, method) == 0x10 && offsetof(InvokeFrame, arguments) == 0x30 && offsetof(InvokeFrame, self) == 0x40);
    using FnInvoke = void (*)(InvokeFrame*, void*);
    enum class InvokeValueKind { Unsupported, Void, Pointer, Boolean, Int8, UInt8, Int16, UInt16, Int32, UInt32, Int64, UInt64, Single, Double };
    struct InvokeSignature {
        InvokeValueKind result{InvokeValueKind::Unsupported}; // Unsupported 表示沿用原生 recipe。
        std::vector<InvokeValueKind> arguments;
        bool operator==(const InvokeSignature&) const = default;
    };

    // 动态代码与 unwind 表必须一起保活；发布前销毁也会撤销已注册的 unwind。
    struct InvokeAdapterCode {
        void* memory{};
        RUNTIME_FUNCTION function{};
        bool registered{};
        ~InvokeAdapterCode();
    };

    void PrepareInvokeAdapters(TypeDatabase& database);
    InvokeSignature ClassifyInvokeSignature(const TypeDatabase& database, const MethodSpec& method);
    bool InstallInvokeDispatch(TypeDatabase& database, void* initializeRuntimeEntries);
} // namespace ci
