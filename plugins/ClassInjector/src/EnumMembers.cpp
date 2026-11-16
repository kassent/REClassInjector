#include "EnumMembers.h"
#include "TypeDatabase.h"
#include "CustomTypeBuildSpec.h"
#include <LogUtils.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace ci {
    // 来源已归一为名称；同名操作仍需验证源存在，但不消耗新的记录容量。
    size_t CountEnumMemberRenames(const EnumExtensionSpec& changes) {
        return std::ranges::count_if(changes.renames, [](const auto& rename) {
            return std::get<std::string>(rename.source) != rename.to;
        });
    }

    // initData 项是有符号池偏移而非枚举值；只在按值选源时读取，纯按名路径不依赖该表。
    static int32_t ReadEnumConstant(const TypeDatabase& database, const FieldDef& field) {
        const void* data = database.GetFieldInitializer(field);
        if (!data)
            throw std::runtime_error("enum initializer is missing or out of bounds");
        int32_t value;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    // 只为包含按值选源的枚举模拟声明顺序；先归一成现有按名操作，再精确计算 no-op/链式改名的容量。
    // 原字段借用到此阶段结束，追加成员直接保留其值；不复制常量表、不创建托管对象。
    void ResolveEnumRenameSources(const TypeDatabase& database, std::vector<CustomTypeBuildSpec>& specs) {
        struct Member {
            std::string name;
            const FieldDef* field{};
            std::optional<int32_t> value;
        };
        std::unordered_map<std::string, std::vector<Member>> enums;
        for (const auto& spec : specs) {
            try {
                if (!spec.enumExtension || !std::ranges::any_of(spec.enumExtension->renames, [](const auto& rename) { return std::holds_alternative<int32_t>(rename.source); }) || enums.contains(spec.name)) continue;
                const auto index = database.ResolveType(spec.name);
                if (index >= database.originalCount || !database.IsInt32Enum(index)) {
                    throw std::runtime_error("enum extension requires an existing Int32 enum: " + spec.name);
                }
                const auto& definition = database.types[index];
                const auto count = database.typeImpls[definition.implIndex].numMemberFields;
                auto& members = enums[spec.name];
                for (uint32_t i = 0; i < count; ++i) {
                    const auto& field = database.fields[definition.memberFieldIndex + i];
                    members.push_back({.name = database.strings.data() + database.fieldImpls[field.impl].name, .field = &field});
                }
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
        for (auto& spec : specs) {
            try {
                auto found = enums.find(spec.name);
                if (!spec.enumExtension || found == enums.end()) continue;
                auto& members = found->second;
                for (auto& rename : spec.enumExtension->renames) {
                    auto member = std::ranges::find_if(members, [&](auto& candidate) {
                        if (const auto* name = std::get_if<std::string>(&rename.source)) return candidate.name == *name;
                        // 0x10 static + 0x40 literal；value__ 不是常量，不能把其默认数据误当枚举0。
                        if (candidate.field && (database.fieldImpls[candidate.field->impl].flags & 0x50) != 0x50) return false;
                        if (!candidate.value) candidate.value = ReadEnumConstant(database, *candidate.field);
                        return *candidate.value == std::get<int32_t>(rename.source);
                    });
                    if (member == members.end()) {
                        const auto source = std::holds_alternative<int32_t>(rename.source) ? std::to_string(std::get<int32_t>(rename.source)) : std::get<std::string>(rename.source);
                        throw std::runtime_error("enum rename source missing: " + spec.name + "." + source);
                    }
                    rename.source = member->name;
                    member->name = rename.to;
                }
                for (const auto& append : spec.enumExtension->appends) members.push_back({.name = append.name, .value = append.value});
            } catch (const std::runtime_error& error) {
                throw std::runtime_error(spec.FormatDiagnosticContext() + ": " + error.what());
            }
        }
    }

    // 原枚举及数组身份不变，只复制连续成员块；共享 FieldImpl 的修改使用局部副本。
    void ApplyEnumMemberChanges(TypeDatabase& database, const CustomTypeConfig& spec) {
        const auto index = database.ResolveType(spec.name);
        auto& definition = database.types[index];
        if (index >= database.originalCount || !database.IsInt32Enum(index)) {
            throw std::runtime_error("enum extension requires an existing Int32 enum: " + spec.name);
        }
        auto implementation = database.typeImpls[definition.implIndex];
        const auto renameCount = CountEnumMemberRenames(*spec.enumExtension);
        const bool changesMembers = renameCount || !spec.enumExtension->appends.empty();
        // 无变化声明借用原成员块完成校验，只有真实改动才复制连续记录。
        std::span<const FieldDef> members(database.fields.data() + definition.memberFieldIndex, implementation.numMemberFields);
        std::vector<FieldDef> changedMembers;
        if (changesMembers) {
            changedMembers.reserve(members.size() + spec.enumExtension->appends.size());
            changedMembers.assign(members.begin(), members.end());
            members = changedMembers;
        }
        auto GetMemberName = [&](const FieldDef& field) { return std::string_view(database.strings.data() + database.fieldImpls[field.impl].name); };
        // TDB名称以NUL终止；拒绝不可原样表示的输入，不限制其它字符。
        auto IsValidMemberName = [](std::string_view name) { return !name.empty() && name != "value__" && name.find('\0') == std::string_view::npos; };
        for (const auto& rename : spec.enumExtension->renames) {
            const auto& from = std::get<std::string>(rename.source);
            if (!IsValidMemberName(from) || !IsValidMemberName(rename.to)) throw std::runtime_error("invalid enum rename");
            auto found = std::ranges::find_if(members, [&](const auto& member) { return GetMemberName(member) == from; });
            if (found == members.end()) throw std::runtime_error("enum rename source missing: " + spec.name + "." + from);
            if (from == rename.to) continue;
            if (std::ranges::any_of(members, [&](const auto& member) { return GetMemberName(member) == rename.to; })) throw std::runtime_error("duplicate enum member: " + rename.to);
            auto impl = database.fieldImpls[found->impl];
            impl.name = database.AppendString(rename.to);
            changedMembers[found - members.begin()].impl = database.fieldImpls.size();
            database.fieldImpls.push_back(impl);
        }
        for (const auto& member : spec.enumExtension->appends) {
            if (!IsValidMemberName(member.name) || std::ranges::any_of(members, [&](const auto& existing) { return GetMemberName(existing) == member.name; })) {
                throw std::runtime_error("duplicate/invalid enum member: " + member.name);
            }
            if (database.initializers.size() >= (1u << 16)) throw std::runtime_error("enum initializer index exceeds 16 bits");
            const auto initializer = uint32_t(database.initializers.size());
            if (database.bytes.empty()) database.bytes.resize(4);
            const auto offset = database.AppendBytes(&member.value, sizeof(member.value));
            database.initializers.push_back(int32_t(offset));
            FieldDef field{
                .declaringType = index,
                .impl = database.fieldImpls.size(),
                .type = index,
                .initHi = initializer >> 10};
            FieldImpl impl{
                .attributes = 0,
                .flags = 0x56,
                .offset = 0,
                .initLo = initializer & 0x3F,
                .name = database.AppendString(member.name),
                .initMid = (initializer >> 6) & 0xF};
            database.fieldImpls.push_back(impl);
            changedMembers.push_back(field);
            members = changedMembers;
        }
        if (!changesMembers) return;
        if (database.fields.size() + members.size() >= (1u << 20) ||
            database.fieldImpls.size() >= (1u << 19) || database.typeImpls.size() >= (1u << 18)) {
            throw std::runtime_error("enum metadata packed index limit exceeded");
        }
        definition.memberFieldIndex = database.fields.size();
        database.fields.insert(database.fields.end(), members.begin(), members.end());
        implementation.numMemberFields = members.size();
        definition.implIndex = database.typeImpls.size();
        database.typeImpls.push_back(implementation);
        _MESSAGE("Extended enum %s: renamed=%zu appended=%zu members=%zu", spec.name.c_str(), renameCount, spec.enumExtension->appends.size(), members.size());
    }

} // namespace ci
