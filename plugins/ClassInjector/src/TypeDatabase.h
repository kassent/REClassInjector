#pragma once
#include "InvokeAdapters.h"
#include "TypeDatabaseRecords.h"
#include <GameTypes.h>
#include <ci/Provider.h>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class BranchTrampoline;

namespace ci {
    using Type = RE::RETypeDefinition;
    // customMethods 按追加 method ID 连续排列。entry 是引擎可调用的入口，不是 provider 的直接导出。
    struct RuntimeMethod {
        uint32_t index{};
        uint32_t declaringType{};
        void* entry{};
        FnInvoke invokeAdapter{};
        LONG reportedError{};
    };
    // virtualMethods 按 slot 保存 method ID，接口窗口也位于这张表中。
    struct RuntimeType {
        std::string name;
        uint32_t index;
        uint32_t parent;
        std::vector<uint32_t> virtualMethods;
        uint32_t genericDefinition{}; // 闭合实例关联的 definition ID（原版或本次新增）；非泛型为 0。
    };

    struct RuntimeExtension {
        uint32_t index;
        uint32_t originalFieldSize;
    };

    // 原表的可扩展副本：不重排旧记录，新类型/方法只追加，原 ID 因此保持不变。
    // Publish 后引擎直接持有 vector.data()，不再改变容量；metadata/provider/entry 保留到进程结束。
    struct TypeDatabase {
        TypeDatabase();
        ~TypeDatabase();
        RE::TDB header{};
        uint32_t originalCount{};         // 新旧 type ID 的分界，不随 header.numTypes 扩展而改变。
        uint32_t originalMethodCount{};   // customMethods[ID - originalMethodCount] 的固定基准。
        uint32_t originalParameterMask{}; // 原参数 ID 的编码掩码，发布后不随 numParams 改变。
        uintptr_t encodingBase{};
        RE::VM* vm{};
        std::vector<RE::RETypeDefinition> types;
        std::vector<RE::RETypeImpl> typeImpls;
        std::vector<MethodDef> methods;
        std::vector<MethodImpl> methodImpls;
        std::vector<FieldDef> fields;
        std::vector<FieldImpl> fieldImpls;
        std::vector<Parameter> parameters;
        std::vector<char> strings;
        std::vector<uint8_t> bytes;
        std::vector<int32_t> initializers; // initData 偏移表；仅追加常量时复制，发布后地址冻结。
        std::vector<RuntimeType> customTypes;
        std::vector<RuntimeExtension> extensions; // 原类型尾部扩展的验证数据，builder 形状核验通过后释放。
        std::vector<RuntimeMethod> customMethods;
        // Build-only declarations, consumed by CreateMethodTrampolines before publication.
        std::vector<MethodSpec> methodDeclarations;
        // Build-only signatures in customMethods order, consumed when reflection adapters are generated.
        std::vector<InvokeSignature> invokeSignatures;
        // Own near entries alongside the records whose addresses they embed.
        std::vector<std::unique_ptr<BranchTrampoline>> methodTrampolines;
        uint16_t invokeDispatchId{}; // 仅生成方法使用；编号原有消费者仍执行原 wrapper。
        std::vector<std::unique_ptr<InvokeAdapterCode>> invokeAdapterCodes;
        std::vector<std::unique_ptr<uint64_t[]>> bitmaps; // 大字段块的外部引用/循环回收位图存储。
        std::unordered_map<std::string, uint32_t> typeNames;
        // 与 RF 一样缓存匹配的声明（包括未找到），不缓存会被 hook 改写的入口地址。
        mutable std::shared_mutex methodQueryMutex;
        mutable std::unordered_map<const Type*, std::unordered_map<std::string, const MethodDef*>> methodQueries;
        // Published names own their buffers; rehash preserves pointers to elements.
        mutable std::shared_mutex nameQueryMutex;
        mutable std::unordered_map<uint32_t, std::string> fullNameQueries;
        // builder 已创建运行时形状；原生 member entry 仍由外层初始化随后安装。
        bool runtimeTypesReady{};

        // 更新 TypeInfo 回指和 TDB 表指针；此后进入引擎持有这些地址的阶段。
        void Publish(RE::TDB& target);
        bool IsRuntimeTypeReady(const RuntimeType& type) const;
        // Find* 只查询自定义记录；ResolveType / GetFullName 同时覆盖原类型与新增类型。
        RuntimeType* FindType(std::string_view name);
        RuntimeMethod* FindMethod(uint32_t index);
        uint32_t ResolveType(std::string_view name) const;
        std::string GetFullName(uint32_t index) const;
        const char* GetFullNameData(uint32_t index) const;
        // 存储分类：对象/数组/字符串/委托均按托管引用占槽，不根据名称猜测。
        bool IsReferenceType(uint32_t index) const;
        // 已有枚举身份、I4 编码和 4 字节 payload 同时成立，供枚举编辑与 ABI 策略复用。
        bool IsInt32Enum(uint32_t index) const;
        // 内嵌槽大小：引用为指针宽，Void 为 0，值类型为 payload；不同于公开 GetValueTypeSize。
        uint32_t GetStorageSize(uint32_t index) const;
        // Build-time pool appends; borrowed pool views may be invalidated. Never append after Publish.
        uint32_t AppendString(std::string_view value);
        uint32_t AppendBytes(const void* data, size_t size);
        // 借用 byte pool 中的列表头；追加池数据后不得继续使用旧指针。
        const ParamList* GetMethodParameters(const MethodDef& method) const;
        // 只读早期查询：覆盖原版与新增记录；方法返回声明入口，不进行对象虚分派。
        const Type* GetType(std::string_view name) const;
        // 查询同类型或父链继承，不将接口可赋值关系混入派生判断。
        bool IsDerivedFrom(const Type* type, const Type* baseType) const;
        // 判断类型是否直接继承 System.Enum，不包含 System.Enum 本身。
        bool IsEnum(const Type* type) const;
        // 只查询闭合值类型的真实 payload 大小，不包含对象头或消费插件的类型政策。
        uint32_t GetValueTypeSize(const Type* type) const;
        void* GetMethod(const Type* type, std::string_view signature) const;
        // 返回 MethodDef 记录，不是 MethodImpl，也不是可调用入口地址。
        const MethodDef* GetMethodDef(const Type* type, std::string_view signature) const;
        // 沿父链查询实例字段记录，不访问 ObjectInfo；构建期和运行期共用。
        const FieldDef* GetFieldDef(const Type* type, std::string_view name) const;
        const FieldDef* FindFieldDeclaration(const Type* type, std::string_view name) const;
        const void* GetFieldInitializer(const FieldDef& field) const;
        // 类型与接口关系：同类型、父类链或已构建的接口列表，不实现泛型变型。
        bool IsAssignable(uint32_t source, uint32_t target) const;
        // Borrowed byte-pool view; invalidated by appending to the pool during construction.
        std::span<const uint32_t> GetInterfaces(const RE::RETypeDefinition& type) const;
    };

} // namespace ci
