#include "workspace/command_palette.h"

#include "aether/assets/asset_database.h"
#include "workspace/editor_workspace.h"

#include <algorithm>
#include <cctype>

namespace aether::editor {

namespace {

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto equal = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), equal) != haystack.end();
}

} // namespace

CommandPalette::CommandPalette(const EditorWorkspace& workspace, const assets::AssetDatabase* assets)
    : workspace_(&workspace), assets_(assets) {
    SetQuery(std::string_view{});
}

void CommandPalette::SetQuery(std::string_view query) {
    query_.assign(query);
    results_.clear();
    if (workspace_ == nullptr) return;
    // Tools first, in workspace order, then assets in the database's path order: the palette's
    // order has to be stable or the same query would list different things between frames.
    for (std::size_t tool = 0; tool < workspace_->ToolCount(); ++tool) {
        if (!ContainsNoCase(workspace_->ToolName(tool), query_) && !ContainsNoCase(workspace_->ToolCategory(tool), query_)) {
            continue;
        }
        PaletteEntry entry;
        entry.kind = PaletteKind::Tool;
        entry.tool = tool;
        results_.push_back(std::move(entry));
    }
    if (assets_ == nullptr) return;
    for (const assets::AssetRecord* record : assets_->All()) {
        if (record == nullptr || !ContainsNoCase(record->path, query_)) continue;
        PaletteEntry entry;
        entry.kind = PaletteKind::Asset;
        entry.path = record->path;
        results_.push_back(std::move(entry));
    }
}

bool CommandPalette::Activate(EditorWorkspace& workspace, std::size_t index) const {
    if (index >= results_.size()) return false;
    const PaletteEntry& entry = results_[index];
    if (entry.kind == PaletteKind::Tool) {
        workspace.Select(entry.tool);
        return true;
    }
    if (!opener_) return false; // nothing to do with an asset in a host that has no opener
    opener_(entry.path);
    return true;
}

bool CommandPalette::ActivateFirst(EditorWorkspace& workspace) const { return Activate(workspace, 0); }

} // namespace aether::editor
