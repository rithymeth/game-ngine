#pragma once

#include "aether/assets/asset_guid.h"
#include "aether/math/quaternion.h"
#include "aether/reflection/reflection.h"

#include <functional>

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Generic, reflection-driven property editing for the editor (Phase 6 step 6;
// design in docs/design/EDITOR_UI.md §5.1 and §6.4). Depends only on Dear
// ImGui and the reflection system, not on any graphics backend, so it builds
// and is tested headless on every platform.

namespace aether::editor {

struct InspectResult {
    // The top-level field whose value changed this frame (nullptr if none).
    const reflect::FieldInfo* changed_field = nullptr;
    // True on the frame an edit finished (mouse released after a drag, Enter
    // pressed in a text field): the point to record one undo step.
    bool committed = false;
    // The top-level field the user chose "Revert to Prefab" on (right-click
    // on an overridden field's label), or nullptr.
    const reflect::FieldInfo* revert_field = nullptr;

    bool Changed() const { return changed_field != nullptr; }
};

struct InspectOptions {
    // Top-level fields for which this returns true are marked as overridden
    // (prefab instances, docs/design/EDITOR_UI.md: a blue bar and highlighted
    // label) and get a "Revert to Prefab" context menu. Empty = none.
    std::function<bool(const reflect::FieldInfo&)> is_overridden;
    // The value "Reset to Default" in the property menu puts in a field; an
    // empty Any means no default is known and the item is hidden.
    std::function<reflect::Any(const reflect::FieldInfo&)> default_value;
    // The Inspector's property search: only top-level fields whose name (or
    // whose component's name) contains this, ignoring case, are drawn.
    std::string_view search;
};

// Draws `object`'s fields as a two-column (label | widget) property table.
//
// Which fields appear: a top-level field is shown if it's flagged
// Field_EditAnywhere (editable) or Field_ReadOnly (shown disabled); other
// fields are internal and hidden. Nested struct fields inherit their parent's
// editability. Widgets follow the field's type and Meta: sliders/drags
// clamped to Meta ranges, units after numbers ("1.50 m"), enum dropdowns,
// text fields for strings, Euler X/Y/Z(/W) rows for vectors, Euler angles in
// degrees for a quaternion (X pitch, Y yaw, Z roll, turned about Y, then X,
// then Z), and collapsible sub-tables for other structs. Meta::category groups
// fields under a heading; Meta::tooltip shows on hovering the label.
//
// `id` scopes ImGui ids, so two inspectors of the same type can coexist.
//
// Right-clicking a field's label opens the property menu: Copy Value, Paste
// Value (only when the clipboard holds a value of that field's type) and
// Reset to Default (only when InspectOptions::default_value supplies one).
// A paste or reset is reported like the edit it is: changed_field names the
// field and committed is true, so the caller records one undo step.
InspectResult InspectObject(const reflect::TypeInfo& type, void* object, const char* id);
InspectResult InspectObject(const reflect::TypeInfo& type, void* object, const char* id, const InspectOptions& options);

// Several objects edited together, the way a multi-selection is: the first object's value is what the widgets
// show, a field whose values differ across the objects is marked "(multiple values)", and an edit is written to
// every object. The result names the field that changed once — the point at which the caller records one undo step
// for the whole selection.
InspectResult InspectObjects(const reflect::TypeInfo& type, std::span<void*> objects, const char* id);
InspectResult InspectObjects(const reflect::TypeInfo& type, std::span<void*> objects, const char* id,
                             const InspectOptions& options);

// True if two live values of `type` hold the same data: numbers and enums byte for byte, strings and arrays by
// value, structs field by field. How the multi-object inspector tells a shared value from a mixed one.
bool ValuesEqual(const reflect::TypeInfo& type, const void* a, const void* b);

// The inspector's property menu («Copy Value» / «Paste Value» / «Reset to Default»), also callable without a mouse.
// The clipboard holds one value of one type: a copy can be pasted into any field of that type, on any object.
void CopyFieldValue(const reflect::FieldInfo& field, const void* object);
bool FieldClipboardFits(const reflect::FieldInfo& field);
// Writes the clipboard into the field. False (and untouched) if it holds no value of the field's type, or the field
// already holds it.
bool PasteFieldValue(const reflect::FieldInfo& field, void* object);
// Writes `default_value` into the field. False (and untouched) for an empty default or a field already holding it.
bool ResetFieldValue(const reflect::FieldInfo& field, void* object, const reflect::Any& default_value);
void ClearFieldClipboard();

// The value `field` has in a default-constructed `type` — what "Reset to Default" restores in a component.
reflect::Any DefaultFieldValue(const reflect::TypeInfo& type, const reflect::FieldInfo& field);

// The component card's Copy Component: one whole object of one type, kept apart from the single-value clipboard.
void CopyObjectValues(const reflect::TypeInfo& type, const void* object);
bool ObjectClipboardFits(const reflect::TypeInfo& type);
// The copied object, or nullptr when the clipboard holds none of `type`.
const void* ObjectClipboardData(const reflect::TypeInfo& type);
void ClearObjectClipboard();

// The top-level fields a whole-object paste or reset writes: every field the inspector shows, read-only ones included,
// because a read-only field is kept in step with an editable one (a model's path with its asset). Hidden fields are
// runtime state and are never written.
bool IsCopiedField(const reflect::FieldInfo& field);

// A rotation as Euler angles in degrees and back: turned about Y (yaw), then X (pitch), then Z (roll), the order the editor's
// cameras use. Pointing straight up or down is the one ambiguous case; it puts the whole turn into yaw.
Vec3 EulerDegreesFromRotation(const Quaternion& rotation);
Quaternion RotationFromEulerDegrees(const Vec3& degrees);

template <typename T>
InspectResult InspectObject(T& object, const char* id) {
    return InspectObject(reflect::Reflect<T>(), &object, id);
}

// Asset reference fields (assets::AssetRef<T>) are drawn as a dropdown of
// the assets of that type. The editor supplies the list — typically from its
// AssetDatabase — as (GUID, display name) pairs for an importer name such as
// "Model". With no provider set, a reference shows its GUID read-only.
struct AssetChoice {
    assets::AssetGuid guid;
    std::string name;
};
using AssetListProvider = std::function<std::vector<AssetChoice>(const char* asset_type)>;
void SetAssetListProvider(AssetListProvider provider);
// The display name the provider gives `guid` among the assets of `asset_type` (for a model, its content path); empty with no provider or no such asset.
std::string AssetNameOf(const char* asset_type, const assets::AssetGuid& guid);

// True if InspectObject would show at least one field of `type`.
bool HasInspectableFields(const reflect::TypeInfo& type);

// The property search: true if `field`'s name, or `component_name` when given, contains `query` without regard to
// case (an empty query matches everything), and if any field `type` shows matches `query`.
bool FieldMatchesSearch(const char* component_name, const reflect::FieldInfo& field, std::string_view query);
bool HasMatchingFields(const reflect::TypeInfo& type, std::string_view query);

// A full-width "Add Component" button with a searchable popup listing
// `candidates` by name. Returns the index of the candidate picked this frame,
// or -1.
int AddComponentButton(std::span<const reflect::TypeInfo* const> candidates, const char* id = "add_component");

} // namespace aether::editor
