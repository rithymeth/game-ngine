#pragma once

#include "aether/reflection/reflection.h"

#include <span>
#include <string>
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

    bool Changed() const { return changed_field != nullptr; }
};

// Draws `object`'s fields as a two-column (label | widget) property table.
//
// Which fields appear: a top-level field is shown if it's flagged
// Field_EditAnywhere (editable) or Field_ReadOnly (shown disabled); other
// fields are internal and hidden. Nested struct fields inherit their parent's
// editability. Widgets follow the field's type and Meta: sliders/drags
// clamped to Meta ranges, units after numbers ("1.50 m"), enum dropdowns,
// text fields for strings, compact X/Y/Z(/W) rows for small math types, and
// collapsible sub-tables for other structs. Meta::category groups fields
// under a heading; Meta::tooltip shows on hovering the label.
//
// `id` scopes ImGui ids, so two inspectors of the same type can coexist.
InspectResult InspectObject(const reflect::TypeInfo& type, void* object, const char* id);

template <typename T>
InspectResult InspectObject(T& object, const char* id) {
    return InspectObject(reflect::Reflect<T>(), &object, id);
}

// True if InspectObject would show at least one field of `type`.
bool HasInspectableFields(const reflect::TypeInfo& type);

// A full-width "Add Component" button with a searchable popup listing
// `candidates` by name. Returns the index of the candidate picked this frame,
// or -1.
int AddComponentButton(std::span<const reflect::TypeInfo* const> candidates, const char* id = "add_component");

} // namespace aether::editor
