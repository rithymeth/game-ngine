#include "reflected_inspector.h"

#include "content/content_drag.h"
#include "extensions.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace aether::editor {

namespace {

using reflect::FieldInfo;
using reflect::TypeInfo;
using reflect::TypeKind;

AssetListProvider& AssetProvider() {
    static AssetListProvider provider;
    return provider;
}

// An AssetRef<T>: a single AssetGuid field. Returns true if handled.
bool DrawAssetRef(const TypeInfo& type, void* ptr, struct DrawContext& ctx);

struct DrawContext {
    InspectResult result;
    bool read_only = false; // the field being drawn is shown but not editable
    const FieldInfo* top_level_field = nullptr; // attributed as the changed field
    const InspectOptions* options = nullptr;
    // Multi-object editing: the objects after the first. An edit to a field of the first object is copied to them,
    // and a field whose values differ is marked mixed.
    std::span<void*> others;
    bool mixed = false; // the top-level field being drawn differs across the objects
};

// One value, the way the property menu's Copy Value keeps it: pasted into any field of the same type, on any object.
struct ValueClipboard {
    const TypeInfo* type = nullptr;
    reflect::Any value;
};

ValueClipboard& Clipboard() {
    static ValueClipboard clipboard;
    return clipboard;
}

ValueClipboard& ObjectClipboard() {
    static ValueClipboard clipboard;
    return clipboard;
}

// The prefab-override marking: an accent bar at the row's left edge and the
// label in the accent colour.
constexpr ImVec4 kOverrideColor{0.35f, 0.65f, 1.0f, 1.0f};

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

// A property-menu action wrote a whole new value: report it like a finished widget edit.
void NoteAction(DrawContext& ctx, const FieldInfo& field) {
    ctx.result.changed_field = &field;
    ctx.result.committed = true;
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
    const bool multi = !ctx.others.empty();
    for (const FieldInfo& field : type.fields) {
        if (top_level && !field.HasFlag(reflect::Field_EditAnywhere) && !field.HasFlag(reflect::Field_ReadOnly)) {
            continue;
        }
        if (top_level && ctx.options != nullptr && !ctx.options->search.empty() &&
            !FieldMatchesSearch(type.name, field, ctx.options->search)) {
            continue;
        }
        if (top_level) {
            ctx.top_level_field = &field;
            ctx.mixed = false;
            if (multi) {
                for (void* other : ctx.others) {
                    if (!ValuesEqual(*field.type, field.Ptr(object), field.Ptr(other))) {
                        ctx.mixed = true;
                        break;
                    }
                }
            }
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
        const bool read_only = top_level && field.HasFlag(reflect::Field_ReadOnly);
        // Before the menu too: a paste or reset below must reach the rest of the selection like a widget edit.
        reflect::Any value_before;
        if (multi && top_level && !read_only) value_before = field.Get(object);
        const bool overridden = top_level && ctx.options != nullptr && ctx.options->is_overridden &&
                                ctx.options->is_overridden(field);
        if (overridden) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const f32 height = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(start.x - 4.0f, start.y), ImVec2(start.x - 1.0f, start.y + height),
                                                      ImGui::GetColorU32(kOverrideColor));
            ImGui::TextColored(kOverrideColor, "%s", field.name);
        } else {
            ImGui::TextUnformatted(field.name);
        }
        const bool label_hovered = ImGui::IsItemHovered();
        if (top_level) {
            ImGui::PushID(field.name);
            if (ImGui::BeginPopupContextItem("property_menu", ImGuiPopupFlags_MouseButtonRight)) {
                if (ImGui::MenuItem("Copy Value")) CopyFieldValue(field, field.Ptr(object));
                if (ImGui::MenuItem("Paste Value", nullptr, false, !read_only && FieldClipboardFits(field))) {
                    if (PasteFieldValue(field, field.Ptr(object))) NoteAction(ctx, field);
                }
                if (ctx.options != nullptr && ctx.options->default_value) {
                    if (ImGui::MenuItem("Reset to Default", nullptr, false, !read_only)) {
                        if (ResetFieldValue(field, field.Ptr(object), ctx.options->default_value(field))) NoteAction(ctx, field);
                    }
                }
                if (overridden) {
                    ImGui::Separator();
                    if (ImGui::MenuItem("Revert to Prefab")) {
                        ctx.result.revert_field = &field;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (ctx.mixed && top_level) {
            ImGui::SameLine();
            ImGui::TextDisabled("(multiple values)");
        }
        if (label_hovered) {
            if (field.meta.tooltip != nullptr) {
                ImGui::SetTooltip("%s%s", field.meta.tooltip, overridden ? "\n(overridden in this instance)" : "");
            } else if (overridden) {
                ImGui::SetTooltip("Overridden in this instance");
            }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(field.name);
        if (read_only) {
            ImGui::BeginDisabled();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ctx.read_only = read_only;
        DrawValue(*field.type, field.Ptr(object), field.meta, ctx);
        ctx.read_only = false;
        if (read_only) {
            ImGui::EndDisabled();
        }
        // An edit made here goes to the rest of the selection too — but only a real change, not the commit of a
        // field the user merely focused: that would unify values that were deliberately left mixed.
        if (value_before.HasValue() && !ValuesEqual(*field.type, value_before.Data(), field.Ptr(object))) {
            for (void* other : ctx.others) field.type->copy_assign(field.Ptr(other), field.Ptr(object));
        }
        ImGui::PopID();
    }
}

// An asset dragged from the Content Browser onto a path field that says what it takes (Meta::asset_extensions): the path goes in, as one complete edit. A field
// that is read only, one that takes other kinds, and anything dragged that is not an asset path leave it alone (and the field is not highlighted as a target).
void AcceptAssetDrop(DrawContext& ctx, const TypeInfo& type, void* ptr, const reflect::Meta& meta) {
    if (meta.asset_extensions == nullptr || ctx.read_only || !ImGui::BeginDragDropTarget()) return;
    const ImGuiPayload* dragged = ImGui::GetDragDropPayload();
    if (dragged != nullptr && dragged->IsDataType(kContentPathPayload) && dragged->Data != nullptr && HasAssetExtension(static_cast<const char*>(dragged->Data), meta.asset_extensions)) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kContentPathPayload)) {
            const std::string path(static_cast<const char*>(payload->Data));
            if (type.kind == TypeKind::String) {
                *static_cast<std::string*>(ptr) = path;
            } else {
                std::memset(ptr, 0, type.size);
                std::memcpy(ptr, path.data(), std::min<usize>(path.size(), type.size > 0 ? type.size - 1 : 0));
            }
            if (ctx.result.changed_field == nullptr) ctx.result.changed_field = ctx.top_level_field;
            ctx.result.committed = true; // a drop is a complete edit
        }
    }
    ImGui::EndDragDropTarget();
}

void DrawValue(const TypeInfo& type, void* ptr, const reflect::Meta& meta, DrawContext& ctx) {
    // A property drawer an extension registered for this type (§26.5).
    if (const ExtensionRegistry* extensions = ExtensionRegistry::Active()) {
        if (const ExtensionPropertyDrawer* drawer = extensions->FindPropertyDrawer(type.name); drawer && drawer->draw) {
            NoteEdit(ctx, drawer->draw(ptr, meta));
            return;
        }
    }
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
        AcceptAssetDrop(ctx, type, ptr, meta);
        return;
    case TypeKind::FixedString:
        NoteEdit(ctx, ImGui::InputText("##v", static_cast<char*>(ptr), type.size));
        AcceptAssetDrop(ctx, type, ptr, meta);
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
        if (type.asset_type != nullptr && DrawAssetRef(type, ptr, ctx)) {
            return;
        }
        // A rotation is shown as angles a person can type: three drags in degrees, written back as the quaternion it is.
        if (std::strcmp(type.name, "Quaternion") == 0 && ctx.top_level_field != nullptr) {
            auto& rotation = *static_cast<Quaternion*>(ptr);
            const Vec3 degrees = EulerDegreesFromRotation(rotation);
            f32 values[3] = {degrees.x, degrees.y, degrees.z};
            const bool changed = ImGui::DragScalarN("##v", ImGuiDataType_Float, values, 3, 0.5f, nullptr, nullptr, "%.1f deg");
            if (changed) rotation = RotationFromEulerDegrees(Vec3(values[0], values[1], values[2]));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Euler angles in degrees: X pitch, Y yaw, Z roll");
            NoteEdit(ctx, changed);
            return;
        }
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

bool DrawAssetRef(const TypeInfo& type, void* ptr, DrawContext& ctx) {
    if (type.fields.size() != 1 || type.fields[0].type != &reflect::Reflect<assets::AssetGuid>()) {
        return false;
    }
    auto* guid = static_cast<assets::AssetGuid*>(type.fields[0].Ptr(ptr));
    if (!AssetProvider()) {
        ImGui::TextDisabled("%s", guid->IsNull() ? "(none)" : assets::ToString(*guid).c_str());
        return true;
    }
    std::vector<AssetChoice> choices = AssetProvider()(type.asset_type);
    std::string preview = "(none)";
    if (!guid->IsNull()) {
        preview = "(missing " + assets::ToString(*guid) + ")";
        for (const AssetChoice& choice : choices) {
            if (choice.guid == *guid) {
                preview = choice.name;
            }
        }
    }
    if (ImGui::BeginCombo("##v", preview.c_str())) {
        if (ImGui::Selectable("(none)", guid->IsNull()) && !guid->IsNull()) {
            *guid = {};
            NoteEdit(ctx, true);
            ctx.result.committed = true;
        }
        for (const AssetChoice& choice : choices) {
            ImGui::PushID(assets::ToString(choice.guid).c_str());
            const bool selected = choice.guid == *guid;
            if (ImGui::Selectable(choice.name.c_str(), selected) && !selected) {
                *guid = choice.guid;
                NoteEdit(ctx, true);
                ctx.result.committed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s asset", type.asset_type);
    }
    return true;
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

void SetAssetListProvider(AssetListProvider provider) { AssetProvider() = std::move(provider); }

namespace {
constexpr f32 kEulerPi = 3.14159265358979f;
constexpr f32 kEulerToDegrees = 180.0f / kEulerPi;
constexpr f32 kEulerToRadians = kEulerPi / 180.0f;
} // namespace

Vec3 EulerDegreesFromRotation(const Quaternion& rotation) {
    const Mat4 m = rotation.Normalized().ToMat4(); // R[row][col] is m.cols[col][row]
    const f32 r12 = m.cols[2].y;
    const f32 pitch = std::asin(std::clamp(-r12, -1.0f, 1.0f));
    f32 yaw = 0.0f, roll = 0.0f;
    if (std::fabs(r12) < 0.99999f) {
        yaw = std::atan2(m.cols[2].x, m.cols[2].z);
        roll = std::atan2(m.cols[0].y, m.cols[1].y);
    } else {
        yaw = std::atan2(-m.cols[0].z, m.cols[0].x); // straight up or down: yaw and roll are the same turn
    }
    return Vec3(pitch, yaw, roll) * kEulerToDegrees;
}

Quaternion RotationFromEulerDegrees(const Vec3& degrees) {
    const f32 x = degrees.x * kEulerToRadians, y = degrees.y * kEulerToRadians, z = degrees.z * kEulerToRadians;
    return (Quaternion::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), y) * Quaternion::FromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), x) *
            Quaternion::FromAxisAngle(Vec3(0.0f, 0.0f, 1.0f), z))
        .Normalized();
}

std::string AssetNameOf(const char* asset_type, const assets::AssetGuid& guid) {
    if (!AssetProvider() || guid.IsNull()) return {};
    for (const AssetChoice& choice : AssetProvider()(asset_type)) {
        if (choice.guid == guid) return choice.name;
    }
    return {};
}

InspectResult InspectObject(const reflect::TypeInfo& type, void* object, const char* id) {
    return InspectObject(type, object, id, InspectOptions{});
}

InspectResult InspectObjects(const reflect::TypeInfo& type, std::span<void*> objects, const char* id) {
    return InspectObjects(type, objects, id, InspectOptions{});
}

InspectResult InspectObjects(const reflect::TypeInfo& type, std::span<void*> objects, const char* id,
                             const InspectOptions& options) {
    DrawContext ctx;
    ctx.options = &options;
    if (objects.empty()) return ctx.result;
    ctx.others = objects.subspan(1);
    ImGui::PushID(id);
    if (type.kind == TypeKind::Struct) {
        if (ImGui::BeginTable("##properties", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
            DrawStructFields(type, objects[0], ctx, /*top_level=*/true);
            ImGui::EndTable();
        }
    } else {
        ImGui::SetNextItemWidth(-FLT_MIN);
        DrawValue(type, objects[0], {}, ctx);
    }
    ImGui::PopID();
    return ctx.result;
}

bool ValuesEqual(const reflect::TypeInfo& type, const void* a, const void* b) {
    switch (type.kind) {
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::UInt:
    case TypeKind::Float:
    case TypeKind::Enum:
        return std::memcmp(a, b, type.size) == 0;
    case TypeKind::String:
        return *static_cast<const std::string*>(a) == *static_cast<const std::string*>(b);
    case TypeKind::FixedString:
        return std::strncmp(static_cast<const char*>(a), static_cast<const char*>(b), type.size) == 0;
    case TypeKind::Struct:
        for (const FieldInfo& field : type.fields) {
            if (!ValuesEqual(*field.type, field.Ptr(a), field.Ptr(b))) return false;
        }
        return true;
    case TypeKind::Array: {
        const usize count = type.array_size(a);
        if (count != type.array_size(b)) return false;
        for (usize i = 0; i < count; ++i) {
            if (!ValuesEqual(*type.element, type.ArrayElement(a, i), type.ArrayElement(b, i))) return false;
        }
        return true;
    }
    }
    return false;
}

void CopyFieldValue(const reflect::FieldInfo& field, const void* object) {
    Clipboard().type = field.type;
    Clipboard().value = field.Get(object);
}

bool FieldClipboardFits(const reflect::FieldInfo& field) {
    return Clipboard().type != nullptr && Clipboard().type == field.type;
}

bool PasteFieldValue(const reflect::FieldInfo& field, void* object) {
    if (!FieldClipboardFits(field) || ValuesEqual(*field.type, field.Ptr(object), Clipboard().value.Data())) {
        return false;
    }
    field.type->copy_assign(field.Ptr(object), Clipboard().value.Data());
    return true;
}

bool ResetFieldValue(const reflect::FieldInfo& field, void* object, const reflect::Any& default_value) {
    if (!default_value.HasValue() || default_value.Type() != field.type ||
        ValuesEqual(*field.type, field.Ptr(object), default_value.Data())) {
        return false;
    }
    field.type->copy_assign(field.Ptr(object), default_value.Data());
    return true;
}

void ClearFieldClipboard() {
    Clipboard().type = nullptr;
    Clipboard().value.Reset();
}

reflect::Any DefaultFieldValue(const reflect::TypeInfo& type, const reflect::FieldInfo& field) {
    const reflect::Any default_object = reflect::Any::DefaultOf(type);
    if (!default_object.HasValue()) return {};
    return reflect::Any::CopyOf(*field.type, field.Ptr(default_object.Data()));
}

void CopyObjectValues(const reflect::TypeInfo& type, const void* object) {
    ObjectClipboard().type = &type;
    ObjectClipboard().value = reflect::Any::CopyOf(type, object);
}

bool ObjectClipboardFits(const reflect::TypeInfo& type) {
    return ObjectClipboard().type == &type && ObjectClipboard().value.HasValue();
}

const void* ObjectClipboardData(const reflect::TypeInfo& type) {
    return ObjectClipboardFits(type) ? ObjectClipboard().value.Data() : nullptr;
}

void ClearObjectClipboard() {
    ObjectClipboard().type = nullptr;
    ObjectClipboard().value.Reset();
}

bool IsCopiedField(const reflect::FieldInfo& field) {
    return field.HasFlag(reflect::Field_EditAnywhere) || field.HasFlag(reflect::Field_ReadOnly);
}

InspectResult InspectObject(const reflect::TypeInfo& type, void* object, const char* id, const InspectOptions& options) {
    DrawContext ctx;
    ctx.options = &options;
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

bool FieldMatchesSearch(const char* component_name, const reflect::FieldInfo& field, std::string_view query) {
    if (query.empty()) return true;
    if (ContainsCaseInsensitive(field.name, query)) return true;
    return component_name != nullptr && ContainsCaseInsensitive(component_name, query);
}

bool HasMatchingFields(const reflect::TypeInfo& type, std::string_view query) {
    if (query.empty()) return HasInspectableFields(type);
    return std::any_of(type.fields.begin(), type.fields.end(), [&](const FieldInfo& field) {
        return (field.HasFlag(reflect::Field_EditAnywhere) || field.HasFlag(reflect::Field_ReadOnly)) &&
               FieldMatchesSearch(type.name, field, query);
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
