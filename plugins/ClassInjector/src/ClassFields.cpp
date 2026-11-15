#include "ClassFields.h"
#include "TypeDatabase.h"
#include "CustomTypeConfig.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace ci {
    // TypeImpl +20h 是托管引用图，+28h 是循环遍历图；每个 bit 对应字段块的一个 8 字节槽。
    // 字段块 <=200h 时单个 uint64 直接内嵌，更大时同一位置保存外部位图指针。
    static std::vector<uint64_t> ReadBitmap(const RE::RETypeImpl& typeImpl, size_t bitmapOffset, size_t wordCount) {
        std::vector<uint64_t> bitmap(wordCount);
        auto bitmapData = reinterpret_cast<const uint8_t*>(&typeImpl) + bitmapOffset;
        size_t storedWordCount = (size_t(typeImpl.fieldSize) + 511) / 512;
        if (typeImpl.fieldSize > 0x200) {
            uintptr_t bitmapAddress;
            std::memcpy(&bitmapAddress, bitmapData, 8);
            if (!bitmapAddress) {
                return bitmap;
            }
            bitmapData = reinterpret_cast<const uint8_t*>(bitmapAddress);
        }
        if (storedWordCount) {
            std::memcpy(bitmap.data(), bitmapData, std::min(storedWordCount, wordCount) * 8);
        }
        return bitmap;
    }

    // 按字段块大小选择内嵌/外置编码；外置存储由 TypeDatabase 持有，空图保留零指针。
    static void WriteBitmap(TypeDatabase& database, RE::RETypeImpl& typeImpl, size_t bitmapOffset, const std::vector<uint64_t>& bitmap) {
        uint64_t encodedBitmap = 0;
        if (typeImpl.fieldSize <= 0x200) {
            if (!bitmap.empty()) {
                encodedBitmap = bitmap[0];
            }
        } else if (std::ranges::any_of(bitmap, [](auto word) { return word != 0; })) {
            auto bitmapStorage = std::make_unique<uint64_t[]>(bitmap.size());
            std::copy(bitmap.begin(), bitmap.end(), bitmapStorage.get());
            encodedBitmap = uintptr_t(bitmapStorage.get());
            database.bitmaps.push_back(std::move(bitmapStorage));
        }
        std::memcpy(reinterpret_cast<uint8_t*>(&typeImpl) + bitmapOffset, &encodedBitmap, 8);
    }

    // 在源类型字段块后追加字段，并扩展两套引用图；新类使用父布局，原类型扩展使用本类旧布局。
    // 所有 offset 都相对 managed 字段块；字段数据最终由原 builder/allocator 分配。
    void BuildClassFields(TypeDatabase& database, const CustomTypeConfig& config, const RE::RETypeDefinition& typeDef,
                          RE::RETypeImpl& typeImpl, const RE::RETypeDefinition& sourceTypeDef) {
        const auto& sourceTypeImpl = database.typeImpls[sourceTypeDef.implIndex];
        const auto typeIndex = uint32_t(typeDef.index);
        const auto firstAddedFieldIndex = database.fields.size();
        uint32_t fieldBlockSize = uint32_t(sourceTypeImpl.fieldSize);
        for (auto& field : config.fields) {
            try {
                if (config.extend && field.serialize) {
                    throw std::runtime_error(config.name + ": serialized extension changes the original RSZ schema");
                }
                if (database.GetFieldDef(&sourceTypeDef, field.name)) {
                    throw std::runtime_error(config.name + ": inherited field name already exists: " + field.name);
                }
                auto fieldTypeIndex = database.ResolveType(field.type);
                auto fieldSize = database.GetStorageSize(fieldTypeIndex);
                if (!fieldSize) {
                    throw std::runtime_error("unsupported field type: " + field.type);
                }
                // 引用保持 8B；较大的值按 16B 保守对齐，覆盖引擎 vec3 等 SIMD 的布局要求。
                // 普通 Guid 只需 8B，但额外 padding 不改变其内嵌数据；引用图在下面合并。
                auto fieldAlignment = std::min(std::bit_floor(fieldSize), 16u);
                fieldBlockSize = (fieldBlockSize + fieldAlignment - 1) & ~(fieldAlignment - 1);
                database.fields.push_back(
                    {.declaringType = typeIndex, .impl = database.fieldImpls.size(), .type = fieldTypeIndex});
                // 0x4000 是引擎字段的 RSZ 标记；原 builder 据此生成 sequence 和 TypeInfo CRC。
                database.fieldImpls.push_back(
                    {.attributes = 0,
                     .flags = uint16_t(6 | (field.serialize ? 0x4000 : 0)),
                     .offset = fieldBlockSize,
                     .name = database.AppendString(field.name)});
                fieldBlockSize += fieldSize;
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(std::string("field ") + field.name + ": " + error.what());
            }
        }
        typeImpl.fieldSize = int32_t((fieldBlockSize + 7) & ~7u);
        auto bitmapWordCount = (size_t(typeImpl.fieldSize) + 511) / 512;
        auto referenceBitmap = ReadBitmap(sourceTypeImpl, 0x20, bitmapWordCount);
        auto cycleBitmap = ReadBitmap(sourceTypeImpl, 0x28, bitmapWordCount);
        for (size_t i = firstAddedFieldIndex; i < database.fields.size(); ++i) {
            const auto& field = database.fields[i];
            const auto fieldOffset = uint32_t(database.fieldImpls[field.impl].offset);
            auto SetBit = [&](auto& map, size_t slot) { map[slot / 64] |= uint64_t(1) << (slot % 64); };
            if (database.IsReferenceType(uint32_t(field.type))) {
                SetBit(referenceBitmap, fieldOffset / 8);
                // 循环遍历还会检查目标自身的图；保守标记保留多态/自引用，不另做整张类型图分析。
                if (database.types[field.type].objectType != std::to_underlying(via::clr::VMObjType::String)) {
                    SetBit(cycleBitmap, fieldOffset / 8);
                }
            } else {
                auto& valueTypeImpl = database.typeImpls[database.types[field.type].implIndex];
                for (size_t bitmapOffset : {0x20, 0x28}) {
                    auto embeddedBitmap = ReadBitmap(valueTypeImpl, bitmapOffset, (size_t(valueTypeImpl.fieldSize) + 511) / 512);
                    for (size_t bit = 0; bit < size_t(valueTypeImpl.fieldSize) / 8; ++bit) {
                        if (embeddedBitmap[bit / 64] & (uint64_t(1) << (bit % 64))) {
                            if (fieldOffset % 8) {
                                throw std::runtime_error("reference-containing value field must be qword aligned");
                            }
                            SetBit(bitmapOffset == 0x20 ? referenceBitmap : cycleBitmap, fieldOffset / 8 + bit);
                        }
                    }
                }
            }
        }
        WriteBitmap(database, typeImpl, 0x20, referenceBitmap);
        WriteBitmap(database, typeImpl, 0x28, cycleBitmap);
    }

    // 只修改原版 sealed 或无既有子类的纯托管 class 尾部；旧字段偏移及原方法入口保持有效。
    // 成员字段按连续区间枚举，因此复制旧成员到新尾部块；原 field ID 不删除、不插入重排。
    void ExtendClassFields(TypeDatabase& database, const CustomTypeConfig& config) {
        auto typeIndex = database.ResolveType(config.name);
        auto& typeDef = database.types[typeIndex];
        if (typeIndex >= database.originalCount || typeDef.objectType != std::to_underlying(via::clr::VMObjType::Object) || (typeDef.typeFlags & 0x180000A0)) {
            throw std::runtime_error("extend requires an original concrete managed class: " + config.name);
        }
        if (!(typeDef.typeFlags & 0x100)) {
            // 任一既有后代都必有直接子类；只查原 TDB，避免改变已有 AOT 字段布局。
            // 本轮新增派生在全部扩展完成后构建，会使用新的父布局，不属于此处的冲突。
            auto originalTypes = std::span(database.types.data(), database.originalCount);
            auto derivedType = std::ranges::find_if(originalTypes, [&](const auto& type) { return type.parentTypeIndex == typeIndex; });
            if (derivedType != originalTypes.end()) {
                throw std::runtime_error("extend would change an existing derived layout: " + config.name + " -> " + database.GetFullName(uint32_t(derivedType->index)));
            }
        }
        if (typeDef.genericsOffset &&
            (*reinterpret_cast<const uint32_t*>(database.bytes.data() + typeDef.genericsOffset) & 0x7FFFF) ==
                typeIndex) {
            throw std::runtime_error("extend requires a concrete class: " + config.name);
        }
        if (!config.methods.empty() || !config.interfaces.empty() || !config.genericTemplate.empty() || config.abstract) {
            throw std::runtime_error("extend only accepts fields: " + config.name);
        }
        // 保存本次扩展前的布局；同一类型可能已经被前一个 Provider 扩展过。
        auto previousTypeImpl = database.typeImpls[typeDef.implIndex];
        const auto previousTypeDef = typeDef;
        auto memberFieldsBegin = database.fields.begin() + typeDef.memberFieldIndex;
        std::vector<FieldDef> memberFields(memberFieldsBegin, memberFieldsBegin + previousTypeImpl.numMemberFields);
        for (auto& field : memberFields) {
            auto& fieldImpl = database.fieldImpls[field.impl];
            if ((fieldImpl.flags & 0x10) && std::ranges::any_of(config.fields, [&](auto& added) {
                    return added.name == database.strings.data() + fieldImpl.name;
                })) {
                throw std::runtime_error(config.name + ": field name already exists");
            }
        }
        typeDef.memberFieldIndex = database.fields.size();
        database.fields.insert(database.fields.end(), memberFields.begin(), memberFields.end());
        auto extendedTypeImpl = previousTypeImpl;
        BuildClassFields(database, config, typeDef, extendedTypeImpl, previousTypeDef);
        extendedTypeImpl.numMemberFields += config.fields.size();
        // 某些原类型共享 impl；扩展使用独立副本，避免连带修改其他类型的大小或位图。
        typeDef.implIndex = database.typeImpls.size();
        database.typeImpls.push_back(extendedTypeImpl);
        // 与新增 class 一样走普通堆；完整清零新增引用槽，并避开 compact arena 的容量限制。
        typeDef.typeFlags &= ~0x01000000u;
        if (std::ranges::none_of(database.extensions, [&](auto& entry) { return entry.index == typeIndex; })) {
            database.extensions.push_back({typeIndex, uint32_t(previousTypeImpl.fieldSize)});
        }
    }

    // 原大字段块位图可能指向原 bytePool；复制表不自动重定位这些嵌在 TypeImpl 中的裸指针。
    // 等新池追加完成、地址稳定后再修正，只改指向原池范围的指针，保留其他原外部存储。
    void RebaseReferenceBitmaps(TypeDatabase& database) {
        const auto& original = database.header;
        for (uint32_t i = 1; i < original.numTypeImpl; ++i) {
            if (database.typeImpls[i].fieldSize > 0x200) {
                for (size_t offset : {0x20, 0x28}) {
                    uintptr_t pointer;
                    auto* destination = reinterpret_cast<uint8_t*>(&database.typeImpls[i]) + offset;
                    std::memcpy(&pointer, destination, 8);
                    if (pointer >= uintptr_t(original.bytePool) &&
                        pointer < uintptr_t(original.bytePool) + original.numBytePool) {
                        pointer = uintptr_t(database.bytes.data()) + pointer - uintptr_t(original.bytePool);
                        std::memcpy(destination, &pointer, 8);
                    }
                }
            }
        }
    }
} // namespace ci
