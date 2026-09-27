#pragma once

#include "anim/json_history.h"
#include "aether/ui/layout_file.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// A widget's place in a layout: child indices from the root ({} is the root).
using WidgetPath = std::vector<usize>;

struct LayoutDiagnostic {
    std::string code; // UD001..
    std::string message;
    WidgetPath path; // the widget it's about, if any
    bool error = true;
};

// An open UI layout (.aui) in the UI Designer (Phase 18 step 6): the widget
// tree, its bindings and animations, its file, whole-document undo, cached
// diagnostics, and edits that keep it consistent (unique names that
// bindings and animation tracks follow when renamed; children only where
// they fit; deleting a widget drops what referred to it).
class UILayoutDocument {
public:
    UILayoutDocument(); // a new layout: a Canvas named "Root"
    explicit UILayoutDocument(ui::LayoutDocument layout, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);
    std::string Text() const; // the .aui file as it would be saved

    ui::Widget& Root() const { return *layout_.root; }
    ui::Widget* At(const WidgetPath& path) const; // null if there's none
    std::optional<WidgetPath> PathOf(const ui::Widget* widget) const;
    std::optional<WidgetPath> PathOf(const std::string& name) const;
    const std::vector<ui::Binding>& Bindings() const { return layout_.bindings; }
    const std::vector<ui::UIAnimation>& Animations() const { return layout_.animations; }
    const ui::UIAnimation* FindAnimation(const std::string& name) const;

    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return dirty_; }
    u64 Revision() const { return revision_; }

    // Any change, undoable. `merge_key`: consecutive edits with it share an undo step (a drag).
    void Edit(const std::string& label, const std::function<void(ui::LayoutDocument&)>& change, const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }
    std::string UndoLabel() const { return history_.UndoLabel(); }
    std::string RedoLabel() const { return history_.RedoLabel(); }

    // --- The tree -----------------------------------------------------------------------
    // A registered widget type, named "<Type><n>", as child `index` of
    // `parent` (-1: last). Refused if the parent is full or the type unknown.
    // `setup` runs on the new widget first (its place in a Canvas), in the same undo step.
    std::optional<WidgetPath> AddWidget(const std::string& type, const WidgetPath& parent, i64 index = -1, std::string* error = nullptr,
                                        const std::function<void(ui::Widget&)>& setup = {});
    // Not the root. Bindings and animation tracks of widgets that go are dropped.
    bool DeleteWidgets(std::vector<WidgetPath> paths, std::string* error = nullptr);
    // A copy just after it, renamed where names would clash (a Canvas child moves 16 units).
    std::optional<WidgetPath> Duplicate(const WidgetPath& path, std::string* error = nullptr);
    // Reparent (or reorder): `index` counts the parent's children without the moved one.
    std::optional<WidgetPath> Move(const WidgetPath& from, const WidgetPath& parent, usize index, std::string* error = nullptr);
    // Names are unique (or empty); bindings and tracks follow the new name.
    bool Rename(const WidgetPath& path, const std::string& name, std::string* error = nullptr);
    std::string UniqueName(const std::string& base) const; // base if free, else its stem numbered ("Button1" -> "Button3")

    // --- Properties -----------------------------------------------------------------------
    // The widget's saved form without its children: common settings, "slot" and its own properties.
    nlohmann::json PropertiesOf(const WidgetPath& path) const;
    // The same, plus the settings left out of the file while they're at their
    // defaults (justify, visibility, focusable, ...): everything Details shows.
    nlohmann::json EditableProperties(const WidgetPath& path) const;
    // Sets one key of that form (rebuilding the widget through its type, so
    // any registered widget works). Bad values are refused with the reason.
    bool SetProperty(const WidgetPath& path, const std::string& key, const nlohmann::json& value, std::string* error = nullptr,
                     const std::string& merge_key = {});

    // --- Canvas placement -----------------------------------------------------------------
    // Lays the tree out in `area` (layout units: the preview's safe area), for geometry and placement.
    void Layout(const ui::Rect& area, const ui::Font& font);
    ui::Rect GeometryOf(const WidgetPath& path) const; // from the last Layout
    bool InCanvas(const WidgetPath& path) const;       // a Canvas child (placed by rectangle)
    // Puts a Canvas child at `rect` (layout units, like GeometryOf) under its
    // anchors: position and size on point axes, margins on stretched ones.
    bool PlaceInCanvas(const WidgetPath& path, const ui::Rect& rect, const std::string& merge_key = {});
    // New anchors (and pivot), keeping the widget where it is.
    bool SetAnchors(const WidgetPath& path, const ui::Anchors& anchors, std::optional<ui::Vec2> alignment = std::nullopt);

    // --- Bindings -------------------------------------------------------------------------
    void AddBinding(const ui::Binding& binding);
    bool SetBinding(usize index, const ui::Binding& binding, const std::string& merge_key = {});
    bool RemoveBinding(usize index);
    std::vector<usize> BindingsOf(const std::string& widget) const;
    // What a widget's bindings can set (text for Text, percent for a ProgressBar, ...).
    static std::vector<std::string> BindableProperties(const ui::Widget& widget);

    // --- Animations -----------------------------------------------------------------------
    std::string AddAnimation(const std::string& name); // made unique; the name it got
    bool RenameAnimation(const std::string& from, const std::string& to, std::string* error = nullptr);
    bool RemoveAnimation(const std::string& name);
    bool AddTrack(const std::string& animation, const std::string& widget, const std::string& property, std::string* error = nullptr);
    bool RemoveTrack(const std::string& animation, usize track);
    // A key at `time` (replacing one within a millisecond), kept in time order.
    bool SetKey(const std::string& animation, usize track, f32 time, f32 value, ui::Ease ease = ui::Ease::Linear, const std::string& merge_key = {});
    // Moves a key in time; its new index comes back (-1: refused).
    i64 MoveKey(const std::string& animation, usize track, usize key, f32 time, const std::string& merge_key = {});
    bool RemoveKey(const std::string& animation, usize track, usize key);

    // --- Checks -----------------------------------------------------------------------------
    // UD001 duplicate names, UD002 a binding's widget missing, UD003 a
    // property its widget can't bind, UD004 a binding without a source,
    // UD005 a track's widget missing, UD006 a track's property it can't
    // animate, UD007 an empty animation (warning), UD008 two tracks on the
    // same property (warning). Cached per revision.
    const std::vector<LayoutDiagnostic>& Diagnostics() const;
    usize ErrorCount() const;

    // A copy of the tree to preview on (animations and themes change it; the document stays as saved).
    std::unique_ptr<ui::Widget> CopyTree() const;

private:
    nlohmann::json Snapshot() const;
    void Restore(const nlohmann::json& snapshot);
    void Changed();
    ui::UIAnimation* MutableAnimation(const std::string& name);
    void ForgetWidgets(const std::vector<std::string>& names); // bindings and tracks of widgets gone

    ui::LayoutDocument layout_;
    std::filesystem::path path_;
    JsonHistory history_;
    bool dirty_ = false;
    u64 revision_ = 1;
    mutable u64 diagnostics_revision_ = 0;
    mutable std::vector<LayoutDiagnostic> diagnostics_;
};

} // namespace aether::editor
