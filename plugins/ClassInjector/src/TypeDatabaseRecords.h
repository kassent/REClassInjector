#pragma once
#include <cstdint>

namespace ci {
    // TDB81 原生记录布局：定义表存 ID/偏移，impl 表存名称/标志/slot，池地址由 TDB 统一提供。
    // 这些位宽和 packing 是引擎格式的一部分，必须保持与原记录一致。
#pragma pack(push, 4)
    struct MethodDef {
        uint32_t declaringType : 19;
        uint32_t paramsLo : 13;
        uint32_t impl : 19;
        uint32_t paramsHi : 13;
        int32_t encodedOffset; // 函数地址 = TypeDatabase::encodingBase + 此偏移；0 表示没有入口。
        uint32_t GetParameterOffset() const { return paramsLo | (paramsHi << 13); }
    };
    struct MethodImpl {
        uint16_t attributes;
        int16_t virtualIndex; // 托管虚表 slot；普通实例方法/构造为 -1。
        uint16_t flags;
        uint16_t implFlags;
        uint32_t name; // 低 28 位为字符串偏移；bit28 为 CLR 反射返回值装箱/读回门。
    };
    struct FieldDef {
        uint64_t declaringType : 19;
        uint64_t impl : 19;
        uint64_t type : 19;
        uint64_t initHi : 6;
        uint64_t reserved : 1;
    };
    struct FieldImpl {
        uint16_t attributes;
        uint16_t reserved : 1;
        uint16_t flags : 15;
        uint32_t offset : 26; // 相对 managed 字段块，不是相对对象起始地址。
        uint32_t initLo : 6;
        uint32_t name : 28;
        uint32_t initMid : 4;
    };
    struct Parameter {
        uint16_t attributes;
        uint16_t initData;
        uint32_t name : 30;
        uint32_t modifier : 2;
        uint32_t type : 19;
        uint32_t flags : 13;
    };
    // bytePool 中的参数列表头；其后紧接 count 个 Parameter ID，返回类型另占一条 Parameter。
    struct ParamList {
        uint16_t count;
        uint16_t invokeId; // 原生 recipe 或已安装 CI 分流的共享调用入口编号。
        uint32_t returnParameter;
    };
#pragma pack(pop)
    static_assert(sizeof(MethodDef) == 12 && sizeof(MethodImpl) == 12 && sizeof(FieldDef) == 8 &&
                  sizeof(FieldImpl) == 12 && sizeof(Parameter) == 12);

} // namespace ci
