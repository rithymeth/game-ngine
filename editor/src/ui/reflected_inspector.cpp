#include "reflected_inspector.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>

namespace aether::editor {

namespace {

using reflect::FieldInfo;
using reflect::TypeInfo;
using reflect::TypeKind;

struct DrawContext {
    InspectResult result;
    const FieldInfo* top_level_field = nullptr; // attributed as the changed field
};

void NoteEdit(DrawContext& ctx, bool changed) {
    if (changed && ctx.result.changed_field == nullptr) {
        ctx.result.changed_field = ctx.top_level_field;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        ctx.result.committed = true;
        if (ctx.result.changed_field == nullptr) {
            ctx.result.changed_field = ctx.top_level_field;
        }
    }
}

ImGuiDataType ScalarDataType(const TypeInfo& type) {
    const bool is_signed = type.kind == TypeKind::Int;
    switch (type.size) {
    case 1: return is_signed ? ImGuiDataType_S8 : ImGuiDataType_U8;
    case 2: return is_signed ? ImGuiDataType_S16 : ImGuiDataType_U16;
    case 4: return type.kind == TypeKind::Float ? ImGuiDataType_Float : (is_signed ? ImGuiDataType_S32 : ImGuiDataType_U32);
    default: return type.kind == TypeKind::Float ? ImGuiDataType_Double : (is_signed ? ImGuiDataType_S64 : ImGuiDataType_U64);
    }
}

// Writes a Meta range bound into scalar storage of `type`.
void StoreBound(const TypeInfo& type, f64 value, void* out) {
    switch (ScalarDataType(type)) {
    case ImGuiDataType_S8: *static_cast<i8*>(out) = static_cast<i8>(value); break;
    case ImGuiDataType_U8: *static_cast<u8*>(out) = static_cast<u8>(value); break;
    case ImGuiDataType_S16: *static_cast<i16*>(out) = static_cast<i16>(value); break;
    case ImGuiDataType_U16: *static_cast<u16*>(out) = static_cast<u16>(value); break;
    case ImGuiDataType_S32: *static_cast<i32*>(out) = static_cast<i32>(value); break;
    case ImGuiDataType_U32: *static_cast<u32*>(out) = static_cast<u32>(value); break;
    case ImGuiDataType_S64: *static_cast<i64*>(out) = static_cast<i64>(value); break;
    case ImGuiDataType_U64: *static_cast<u64*>(out) = static_cast<u64>(value); break;
    case ImGuiDataType_Float: *static_cast<f32*>(out) = static_cast<f32>(value); break;
    default: *static_cast<f64*>(out) = value; break;
    }
}

std::string NumberFormat(const TypeInfo& type, const reflect::Meta& meta) {
    std::string format;
    switch (type.kind) {
    case TypeKind::Float: format = "%.3f"; break;
    case TypeKind::UInt: format = type.size == 8 ? "%llu" : "%u"; break;
    default: format = type.size == 8 ? "%lld" : "%d"; break;
    }
    if (meta.units != nullptr) {
        format += " ";
        format += meta.units;
    }
    return format;
}

// Small all-float structs laid out contiguously (Vec3, Vec4, Quaternion)
// get one compact row of drags instead of a nested table.
bool IsCompactFloatRow(const TypeInfo& type) {
    if (type.kind != TypeKind::Struct || type.fields.empty() || type.fields.size() > 4) {
        return false;
    }
    for (usize i = 0; i < type.fields.size(); ++i) {
        const FieldInfo& field = type.fields[i];
        if (field.type->kind != TypeKind::Float || field.type->size != 4 || field.offset != i * sizeof(f32)) {
            return false;
        }
    }
    return true;
}

void DrawValue(const TypeInfo& type, void* ptr, const reflect::Meta& meta, DrawContext& ctx);

void DrawStructFields(const TypeInfo& type, void* object, DrawContext& ctx, bool top_level) {
    const char* current_category = nullptr;
    for (const FieldInfo& field : type.fields) {
        if (top_level && !field.HasFlag(reflect::Field_EditAnywhere) && !field.HasFlag(reflect::Field_ReadOnly)) {
            continue;
        }
        if (top_level) {
            ctx.top_level_field = &field;
        }

        const char* category = field.meta.category;
        if (category != nullptr && (current_category == nullptr || std::strcmp(category, current_category) != 0)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%s", category);
            current_category = category;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(field.name);
        if (field.meta.tooltip != nullptr && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", field.meta.tooltip);
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(field.name);
        const bool read_only = top_level && field.HasFlag(reflect::Field_ReadOnly);
        if (read_only) {
            ImGui::BeginDisabled();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        DrawValue(*field.type, field.Ptr(object), field.meta, ctx);
        if (read_only) {
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
}

void DrawValue(const TypeInfo& type, void* ptr, const reflect::Meta& meta, DrawContext& ctx) {
    switch (type.kind) {
    case TypeKind::Bool:
        NoteEdit(ctx, ImGui::Checkbox("##v", static_cast<bool*>(ptr)));
        return;
    case TypeKind::Int:
    case TypeKind::UInt:
    case TypeKind::Float: {
        const ImGuiDataType data_type = ScalarDataType(type);
        const std::string format = NumberFormat(type, meta);
        alignas(8) unsigned char min_storage[8] = {};
        alignas(8) unsigned char max_storage[8] = {};
        const void* min = nullptr;
        const void* max = nullptr;
        if (meta.HasRange()) {
            StoreBound(type, meta.range_min, min_storage);
            StoreBound(type, meta.range_max, max_storage);
            min = min_storage;
            max = max_storage;
        }
        const f32 speed = type.kind == TypeKind::Float ? 0.01f : 0.1f;
        NoteEdit(ctx, ImGui::DragScalar("##v", data_type, ptr, speed, min, max, format.c_str(),
                                        meta.HasRange() ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None));
        return;
    }
    case TypeKind::String:
        NoteEdit(ctx, ImGui::InputText("##v", static_cast<std::string*>(ptr)));
        return;
    case TypeKind::FixedString:
        NoteEdit(ctx, ImGui::InputText("##v", static_cast<char*>(ptr), type.size));
        return;
    case TypeKind::Enum: {
        const TypeInfo& underlying = *type.underlying;
        i64 value = 0;
        std::memcpy(&value, ptr, underlying.size); // little-endian: low bytes first
        if (underlying.kind == TypeKind::Int && underlying.size < 8 && (value >> (underlying.size * 8 - 1)) & 1) {
            value |= ~((i64{1} << (underlying.size * 8)) - 1); // sign-extend
        }
        const char* preview = type.EnumName(value);
        char unnamed[32];
        if (preview == nullptr) {
            std::snprintf(unnamed, sizeof(unnamed), "%lld", static_cast<long long>(value));
            preview = unnamed;
        }
        if (ImGui::BeginCombo("##v", preview)) {
            for (const reflect::EnumValue& entry : type.enum_values) {
                const bool selected = entry.value == value;
                if (ImGui::Selectable(entry.name, selected) && !selected) {
                    std::memcpy(ptr, &entry.value, underlying.size);
                    NoteEdit(ctx, true);
                    ctx.result.committed = true; // a pick is a complete edit
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return;
    }
    case TypeKind::Array: {
        const usize count = type.array_size(ptr);
        if (ImGui::TreeNodeEx("##v", ImGuiTreeNodeFlags_SpanAvailWidth, "%zu element%s", count, count == 1 ? "" : "s")) {
            usize remove_index = count; // none
            if (ImGui::BeginTable("##elements", 3, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("index", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("remove", ImGuiTableColumnFlags_WidthFixed);
                for (usize i = 0; i < count; ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("[%zu]", i);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    DrawValue(*type.element, type.array_element(ptr, i), {}, ctx);
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::SmallButton("x")) {
                        remove_index = i;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove element %zu", i);
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (remove_index < count) {
                // Shift the later elements down (copy-assign), then shrink.
                for (usize i = remove_index; i + 1 < count; ++i) {
                    type.element->copy_assign(type.array_element(ptr, i), type.array_element(ptr, i + 1));
                }
                type.array_resize(ptr, count - 1);
                NoteEdit(ctx, true);
                ctx.result.committed = true;
            }
            if (ImGui::SmallButton("+ Add")) {
                type.array_resize(ptr, type.array_size(ptr) + 1);
                NoteEdit(ctx, true);
                ctx.result.committed = true;
            }
            ImGui::TreePop();
        }
        return;
    }
    case TypeKind::Struct: {
        if (IsCompactFloatRow(type)) {
            const std::string format = NumberFormat(*type.fields[0].type, meta);
            NoteEdit(ctx, ImGui::DragScalarN("##v", ImGuiDataType_Float, ptr, static_cast<int>(type.fields.size()),
                                             0.01f, nullptr, nullptr, format.c_str()));
            return;
        }
        if (ImGui::TreeNodeEx("##v", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", type.name)) {
            if (ImGui::BeginTable("##nested", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
                DrawStructFields(type, ptr, ctx, /*top_level=*/false);
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }
        return;
    }
    }
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != haystack.end();
}

} // namespace

InspectResult InspectObject(const reflect::TypeInfo& type, void* object, const char* id) {
    DrawContext ctx;
    ImGui::PushID(id);
    if (type.kind == TypeKind::Struct) {
        if (ImGui::BeginTable("##properties", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
            DrawStructFields(type, object, ctx, /*top_level=*/true);
            ImGui::EndTable();
        }
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        DrawValue(type, object, {}, ctx);
    }
    ImGui::PopID();
    return ctx.result;
}

bool HasInspectableFields(const reflect::TypeInfo& type) {
    return std::any_of(type.fields.begin(), type.fields.end(), [](const FieldInfo& field) {
        return field.HasFlag(reflect::Field_EditAnywhere) || field.HasFlag(reflect::Field_ReadOnly);
    });
}

int AddComponentButton(std::span<const reflect::TypeInfo* const> candidates, const char* id) {
    int picked = -1;
    ImGui::PushID(id);
    if (ImGui::Button("+ Add Component", ImVec2(-FLT_MIN, 0))) {
        ImGui::OpenPopup("##add_component_popup");
    }
    if (ImGui::BeginPopup("##add_component_popup")) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) {
            filter.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(220.0f);
        ImGui::InputTextWithHint("##filter", "Search components", &filter);
        ImGui::Separator();
        int shown = 0;
        for (usize i = 0; i < candidates.size(); ++i) {
            if (!ContainsCaseInsensitive(candidates[i]->name, filter)) {
                continue;
            }
            ++shown;
            if (ImGui::Selectable(candidates[i]->name)) {
                picked = static_cast<int>(i);
            }
        }
        if (shown == 0) {
            ImGui::TextDisabled(candidates.empty() ? "Every component is already added" : "No matches");
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return picked;
}

} // namespace aether::editor
