#include "extensions.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace aether::editor {

namespace stdfs = std::filesystem;

namespace {

ExtensionRegistry*& ActiveSlot() {
    static ExtensionRegistry* active = nullptr;
    return active;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

template <typename T, typename Same>
void AddReplacing(std::vector<T>& list, T&& entry, Same same) {
    for (T& existing : list) {
        if (same(existing, entry)) {
            existing = std::move(entry);
            return;
        }
    }
    list.push_back(std::move(entry));
}

std::vector<std::string_view> SplitPath(std::string_view path) {
    std::vector<std::string_view> parts;
    usize start = 0;
    while (start <= path.size()) {
        const usize slash = path.find('/', start);
        const usize end = slash == std::string_view::npos ? path.size() : slash;
        if (end > start) parts.push_back(path.substr(start, end - start));
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return parts;
}

// Draws the entries of `items` (already filtered to share the first `depth`
// path parts) as menu items and submenus, in first-appearance order.
void DrawLevel(const std::vector<const ExtensionMenuItem*>& items, usize depth) {
    std::vector<std::string_view> seen;
    for (const ExtensionMenuItem* item : items) {
        const std::vector<std::string_view> parts = SplitPath(item->path);
        if (parts.size() <= depth) continue;
        const std::string_view child = parts[depth];
        if (std::find(seen.begin(), seen.end(), child) != seen.end()) continue;
        seen.push_back(child);
        if (parts.size() == depth + 1) {
            const bool enabled = !item->enabled || item->enabled();
            if (ImGui::MenuItem(std::string(child).c_str(), item->shortcut.empty() ? nullptr : item->shortcut.c_str(),
                                false, enabled) &&
                item->action) {
                item->action();
            }
        } else if (ImGui::BeginMenu(std::string(child).c_str())) {
            std::vector<const ExtensionMenuItem*> inside;
            for (const ExtensionMenuItem* other : items) {
                const std::vector<std::string_view> other_parts = SplitPath(other->path);
                if (other_parts.size() > depth + 1 && other_parts[depth] == child) inside.push_back(other);
            }
            DrawLevel(inside, depth + 1);
            ImGui::EndMenu();
        }
    }
}

} // namespace

ExtensionRegistry::~ExtensionRegistry() {
    if (ActiveSlot() == this) ActiveSlot() = nullptr;
}

void ExtensionRegistry::AddPanel(ExtensionPanel panel) {
    if (panel.category.empty()) panel.category = "Extensions";
    AddReplacing(panels_, std::move(panel), [](const ExtensionPanel& a, const ExtensionPanel& b) {
        return a.name == b.name && a.owner == b.owner;
    });
    Changed();
}

void ExtensionRegistry::AddMenuItem(ExtensionMenuItem item) {
    AddReplacing(menu_items_, std::move(item), [](const ExtensionMenuItem& a, const ExtensionMenuItem& b) {
        return a.path == b.path && a.owner == b.owner;
    });
    Changed();
}

void ExtensionRegistry::AddPropertyDrawer(ExtensionPropertyDrawer drawer) {
    AddReplacing(drawers_, std::move(drawer), [](const ExtensionPropertyDrawer& a, const ExtensionPropertyDrawer& b) {
        return a.type == b.type && a.owner == b.owner;
    });
    Changed();
}

void ExtensionRegistry::AddAssetType(ExtensionAssetType type) {
    type.extension = Lower(type.extension);
    if (!type.extension.empty() && type.extension[0] != '.') type.extension.insert(type.extension.begin(), '.');
    AddReplacing(asset_types_, std::move(type), [](const ExtensionAssetType& a, const ExtensionAssetType& b) {
        return a.extension == b.extension && a.owner == b.owner;
    });
    Changed();
}

void ExtensionRegistry::RemoveOwner(std::string_view owner) {
    const auto mine = [&](const auto& e) { return e.owner == owner; };
    panels_.erase(std::remove_if(panels_.begin(), panels_.end(), mine), panels_.end());
    menu_items_.erase(std::remove_if(menu_items_.begin(), menu_items_.end(), mine), menu_items_.end());
    drawers_.erase(std::remove_if(drawers_.begin(), drawers_.end(), mine), drawers_.end());
    asset_types_.erase(std::remove_if(asset_types_.begin(), asset_types_.end(), mine), asset_types_.end());
    Changed();
}

void ExtensionRegistry::Clear() {
    panels_.clear();
    menu_items_.clear();
    drawers_.clear();
    asset_types_.clear();
    Changed();
}

const ExtensionPanel* ExtensionRegistry::FindPanel(std::string_view name) const {
    for (const ExtensionPanel& p : panels_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const ExtensionAssetType* ExtensionRegistry::FindAssetTypeByExtension(std::string_view extension) const {
    const std::string wanted = Lower(extension);
    for (const ExtensionAssetType& t : asset_types_) {
        if (t.extension == wanted) return &t;
    }
    return nullptr;
}

const ExtensionAssetType* ExtensionRegistry::FindAssetTypeForPath(const stdfs::path& file) const {
    return FindAssetTypeByExtension(file.extension().string());
}

const ExtensionPropertyDrawer* ExtensionRegistry::FindPropertyDrawer(std::string_view type) const {
    // The latest registration wins, so a project can override a plugin's drawer.
    for (auto it = drawers_.rbegin(); it != drawers_.rend(); ++it) {
        if (it->type == type) return &*it;
    }
    return nullptr;
}

stdfs::path ExtensionRegistry::CreateAsset(const ExtensionAssetType& type, const stdfs::path& directory,
                                           std::string* error) const {
    std::error_code ec;
    stdfs::create_directories(directory, ec);
    stdfs::path file;
    for (int n = 0; n < 1000; ++n) {
        file = directory / ("New " + type.name + (n == 0 ? "" : " " + std::to_string(n)) + type.extension);
        if (!stdfs::exists(file, ec)) break;
    }
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        if (error) *error = "can't write " + file.string();
        return {};
    }
    out << type.new_text;
    return file;
}

bool ExtensionRegistry::OpenAsset(const stdfs::path& file) const {
    const ExtensionAssetType* type = FindAssetTypeForPath(file);
    if (!type || !type->open) return false;
    type->open(file);
    return true;
}

bool ExtensionRegistry::TriggerShortcut(std::string_view shortcut) const {
    if (shortcut.empty()) return false;
    for (const ExtensionMenuItem& item : menu_items_) {
        if (item.shortcut == shortcut && item.action && (!item.enabled || item.enabled())) {
            item.action();
            return true;
        }
    }
    return false;
}

void ExtensionRegistry::DrawMenus(std::string_view skip_root) const {
    std::vector<std::string_view> roots;
    for (const ExtensionMenuItem& item : menu_items_) {
        const std::vector<std::string_view> parts = SplitPath(item.path);
        if (parts.size() < 2 || parts[0] == skip_root) continue;
        if (std::find(roots.begin(), roots.end(), parts[0]) == roots.end()) roots.push_back(parts[0]);
    }
    for (std::string_view root : roots) {
        if (!ImGui::BeginMenu(std::string(root).c_str())) continue;
        DrawMenuItemsOf(root);
        ImGui::EndMenu();
    }
}

void ExtensionRegistry::DrawMenuItemsOf(std::string_view root) const {
    std::vector<const ExtensionMenuItem*> items;
    for (const ExtensionMenuItem& item : menu_items_) {
        const std::vector<std::string_view> parts = SplitPath(item.path);
        if (parts.size() >= 2 && parts[0] == root) items.push_back(&item);
    }
    DrawLevel(items, 1);
}

void ExtensionRegistry::MakeActive() { ActiveSlot() = const_cast<ExtensionRegistry*>(this); }
ExtensionRegistry* ExtensionRegistry::Active() { return ActiveSlot(); }

namespace {

// "Ctrl+Shift+B" -> the chord, or 0 if it isn't a shortcut ImGui can poll.
ImGuiKeyChord ParseChord(std::string_view text) {
    ImGuiKeyChord mods = 0;
    ImGuiKey key = ImGuiKey_None;
    usize start = 0;
    while (start <= text.size()) {
        const usize plus = text.find('+', start);
        const std::string part = Lower(text.substr(start, (plus == std::string_view::npos ? text.size() : plus) - start));
        if (part == "ctrl") {
            mods |= ImGuiMod_Ctrl;
        } else if (part == "shift") {
            mods |= ImGuiMod_Shift;
        } else if (part == "alt") {
            mods |= ImGuiMod_Alt;
        } else if (part.size() == 1 && part[0] >= 'a' && part[0] <= 'z') {
            key = static_cast<ImGuiKey>(ImGuiKey_A + (part[0] - 'a'));
        } else if (part.size() == 1 && part[0] >= '0' && part[0] <= '9') {
            key = static_cast<ImGuiKey>(ImGuiKey_0 + (part[0] - '0'));
        } else if (part.size() >= 2 && part[0] == 'f' && std::isdigit(static_cast<unsigned char>(part[1]))) {
            const int n = std::atoi(part.c_str() + 1);
            if (n >= 1 && n <= 12) key = static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
        } else {
            return 0;
        }
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return key == ImGuiKey_None ? 0 : (mods | key);
}

} // namespace

usize ExtensionRegistry::PollShortcuts() const {
    if (ImGui::GetIO().WantTextInput) return 0;
    usize fired = 0;
    for (const ExtensionMenuItem& item : menu_items_) {
        const ImGuiKeyChord chord = ParseChord(item.shortcut);
        if (chord == 0 || !item.action || (item.enabled && !item.enabled())) continue;
        if (ImGui::IsKeyChordPressed(chord)) {
            item.action();
            ++fired;
        }
    }
    return fired;
}

} // namespace aether::editor
