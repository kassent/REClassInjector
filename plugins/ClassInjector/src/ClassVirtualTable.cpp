#include "ClassVirtualTable.h"
#include "TypeDatabase.h"

namespace ci {
    // Nearest ancestor with ObjectInfo supplies inherited slots; metadata selects overrides.
    void UpdateVirtualTable(TypeDatabase& database, const RuntimeType& type) {
        auto& definition = database.types[type.index];
        // 抽象类可能没有 ObjectInfo，但其 metadata slot 仍被具体派生类继承。
        if (!definition.objectInfo) {
            return;
        }
        // ObjectInfo[-2] 指向托管虚表；它与 ObjectInfo 内部的 native slot 区域不同。
        auto** slots = static_cast<void**>(definition.objectInfo[-2]);
        auto parent = type.parent;
        while (parent && !database.types[parent].objectInfo) {
            parent = uint32_t(database.types[parent].parentTypeIndex);
        }
        const uint32_t* parentMethods = nullptr;
        size_t parentCount = 0;
        if (parent) {
            auto& ancestor = database.types[parent];
            auto** parentSlots = static_cast<void**>(ancestor.objectInfo[-2]);
            parentCount = database.typeImpls[ancestor.implIndex].numManagedVtable;
            parentMethods = reinterpret_cast<const uint32_t*>(database.bytes.data() + ancestor.unknown2C);
            for (size_t i = 0; i < parentCount; ++i) {
                slots[i] = parentSlots[i];
            }
        }
        for (size_t i = 0; i < type.virtualMethods.size(); ++i) {
            auto index = type.virtualMethods[i];
            if (auto* method = database.FindMethod(index)) {
                slots[i] = method->entry;
            } else if (index && (i >= parentCount || parentMethods[i] != index)) {
                // 未被祖先覆盖的抽象父类 override 解码其 AOT 入口；接口别名复用相同 method 的现有槽。
                auto& implementation = database.methodImpls[database.methods[index].impl];
                if (parent && (implementation.flags & 0x40) && implementation.virtualIndex >= 0 &&
                    size_t(implementation.virtualIndex) < parentCount &&
                    parentMethods[implementation.virtualIndex] == index) {
                    auto** parentSlots = static_cast<void**>(database.types[parent].objectInfo[-2]);
                    slots[i] = parentSlots[implementation.virtualIndex];
                    continue;
                }
                auto offset = database.methods[index].encodedOffset;
                slots[i] = offset ? reinterpret_cast<void*>(database.encodingBase + offset) : nullptr;
            }
        }
    }
} // namespace ci
