#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The editor extensibility API (Phase 26 step 4, docs/design/PHASE_SPECS.md
// §26.5): what a plugin, a game module or a project's editor script adds to
// the editor without touching it.
//
//   - Panels: a window with a name, listed in the Tools menu and hub under
//     "Extensions" (or the category given).
//   - Menu items: "Menu/Sub/Item" paths, drawn as menus beside Tools; an
//     item may carry a shortcut, which the host's key handling fires.
//   - Property drawers: replace the Inspector's widget for every field of a
//     reflected type (Color, a game's own struct, ...).
//   - Asset types: a file extension with a display name, a template for
//     "Create", and an opener the Content Browser calls on double-click.
//
// Everything is registered under an owner (a plugin or script name), so one
// owner's entries are removed together when it unloads or reloads. All of it
// is plain Dear ImGui and std::function, so it runs headless and in tests.

namespace aether::editor {

struct ExtensionPanel {
    std::string name;
    std::string category;
    std::function<void()> draw;
    std::string owner;
};

struct ExtensionMenuItem {
    std::string path; // "Tools/Level/Bake Lighting"; the last part is the item
    std::string shortcut; // "Ctrl+Shift+B", shown in the menu; empty for none
    std::function<void()> action;
    std::function<bool()> enabled; // empty: always
    std::string owner;
};

// Draws the widget for `value` (a pointer to the type) in the Inspector's
// value column, labelled by the row; returns true when it changed it.
using PropertyDrawFn = std::function<bool(void* value, const reflect::Meta& meta)>;

struct ExtensionPropertyDrawer {
    std::string type; // the reflected type's name: "Color", "Transform"
    PropertyDrawFn draw;
    std::string owner;
};

struct ExtensionAssetType {
    std::string name;      // "Dialogue"
    std::string extension; // ".dialogue", lower case with the dot
    std::string new_text;  // what "Create" writes into a new file
    std::function<void(const std::filesystem::path&)> open; // double-click; may be empty
    std::string owner;
};

class ExtensionRegistry {
public:
    ExtensionRegistry() = default;
    ~ExtensionRegistry();
    ExtensionRegistry(const ExtensionRegistry&) = delete;
    ExtensionRegistry& operator=(const ExtensionRegistry&) = delete;

    // The same name from the same owner replaces the earlier entry.
    void AddPanel(ExtensionPanel panel);
    void AddMenuItem(ExtensionMenuItem item);
    void AddPropertyDrawer(ExtensionPropertyDrawer drawer);
    void AddAssetType(ExtensionAssetType type);

    // Drops everything `owner` registered.
    void RemoveOwner(std::string_view owner);
    void Clear();

    const std::vector<ExtensionPanel>& Panels() const { return panels_; }
    const std::vector<ExtensionMenuItem>& MenuItems() const { return menu_items_; }
    const std::vector<ExtensionAssetType>& AssetTypes() const { return asset_types_; }
    const std::vector<ExtensionPropertyDrawer>& PropertyDrawers() const { return drawers_; }
    // Bumps on every change, so a host can rebuild what it derived.
    u64 Version() const { return version_; }

    const ExtensionPanel* FindPanel(std::string_view name) const;
    const ExtensionAssetType* FindAssetTypeByExtension(std::string_view extension) const;
    const ExtensionAssetType* FindAssetTypeForPath(const std::filesystem::path& file) const;
    const ExtensionPropertyDrawer* FindPropertyDrawer(std::string_view type) const;

    // Writes a new file of `type` (its template text) into `directory`, as
    // "New <Name><ext>" (numbered if taken); returns the path, empty on failure.
    std::filesystem::path CreateAsset(const ExtensionAssetType& type, const std::filesystem::path& directory,
                                      std::string* error = nullptr) const;
    // The asset type's opener for `file`; false if no type claims it or it has none.
    bool OpenAsset(const std::filesystem::path& file) const;

    // Fires the menu item with this shortcut (e.g. "Ctrl+S"); false if none.
    bool TriggerShortcut(std::string_view shortcut) const;

    // The menus, as nested ImGui menus: one BeginMenu per root ("Tools"
    // items belong in the host's own Tools menu, so pass skip_root = "Tools"
    // to leave them for DrawMenuItemsOf).
    void DrawMenus(std::string_view skip_root = {}) const;
    // Just the items under `root` ("Tools"), into the menu already open.
    void DrawMenuItemsOf(std::string_view root) const;

    // Fires the items whose shortcut was pressed this frame (call once per
    // frame, inside an ImGui frame); skipped while a text field has focus.
    // Returns how many fired.
    usize PollShortcuts() const;

    // The registry the host made active, process-wide: the Inspector draws
    // its property drawers, and a plugin's editor module registers into it
    // (ExtensionRegistry::Active()). Null when no host did.
    void MakeActive();
    static ExtensionRegistry* Active();

private:
    void Changed() { ++version_; }

    std::vector<ExtensionPanel> panels_;
    std::vector<ExtensionMenuItem> menu_items_;
    std::vector<ExtensionPropertyDrawer> drawers_;
    std::vector<ExtensionAssetType> asset_types_;
    u64 version_ = 1;
};

} // namespace aether::editor
