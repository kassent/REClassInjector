#pragma once
#include "CustomTypeConfig.h"

namespace RE {
    struct RETypeDefinition;
    struct RETypeImpl;
}

namespace ci {
    struct TypeDatabase;
    struct RuntimeType;
    // 从最终方法/参数记录还原签名与种类，不重建 provider binding；供模板展开与接口匹配共用。
    MethodSpec ReadMethodDeclaration(const TypeDatabase& database, uint32_t methodIndex);
    // 追加方法与参数，选择 invoke recipe，建立 virtual/override 槽；消费声明到入口生成队列。
    void BuildClassMethods(TypeDatabase& database, CustomTypeConfig& declaration, RuntimeType& type,
                           RE::RETypeDefinition& definition);
    // 在已有方法槽上匹配接口实现并写入连续窗口；不写 ObjectInfo 中的函数指针。
    void BuildInterfaceSlots(TypeDatabase& database, const CustomTypeConfig& declaration, RuntimeType& type,
                             RE::RETypeDefinition& definition, const RE::RETypeImpl& implementation);
} // namespace ci
