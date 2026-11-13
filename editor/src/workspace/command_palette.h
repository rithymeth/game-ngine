#pragma once

// Phase 43.5: the command palette.
//
// A query filters the workspace's tools and, when an asset database is supplied, the project's
// assets. Tools select themselves when chosen; assets hand their path to an opener the host
// supplies, because what "opening" an asset means is a host decision (the Windows editor
// focuses the content browser at it, a headless host might do nothing).
//
// Keeping the matching and the choice out of the drawing code is what makes it testable: the
// behaviour -- what a query lists and what choosing an entry does -- is checked without a window.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::assets {
class AssetDatabase;
}

namespace aether::editor {

class EditorWorkspace;

enum class PaletteKind {
    Tool,
    Command,
    Asset,
};

struct PaletteEntry {
    PaletteKind kind = PaletteKind::Tool;
    std::size_t tool = 0; // Tool: index into the workspace's tool list
    std::string path;     // Asset: content-relative path, '/'-separated
    std::string command;  // Command: the registry's stable id
};

class CommandPalette {
public:
    // Handed an asset entry's path when one is chosen; the host decides what that means.
    using AssetOpener = std::function<void(const std::string& content_relative_path)>;

    // The workspace (and, optionally, the asset database) must outlive the palette.
    explicit CommandPalette(const EditorWorkspace& workspace, const assets::AssetDatabase* assets = nullptr);

    void SetAssetOpener(AssetOpener opener) { opener_ = std::move(opener); }

    // An empty query lists every tool in workspace order, then every command in registry order, then every asset
    // in path order, so the palette doubles as an index. Otherwise the match is a case-insensitive substring of a
    // tool name, a tool category, a command label or id, or an asset path -- so "anim" finds both.
    void SetQuery(std::string_view query);
    const std::string& Query() const { return query_; }
    const std::vector<PaletteEntry>& Results() const { return results_; }

    // Chooses an entry: a tool is selected in the workspace, a command runs through it (false when the command is
    // disabled), an asset goes to the opener. False when the index is out of range, or when it is an asset and no
    // opener is set.
    bool Activate(EditorWorkspace& workspace, std::size_t index) const;
    // Chooses the first result, for Enter with nothing highlighted.
    bool ActivateFirst(EditorWorkspace& workspace) const;

private:
    const EditorWorkspace* workspace_ = nullptr;
    const assets::AssetDatabase* assets_ = nullptr;
    AssetOpener opener_;
    std::string query_;
    std::vector<PaletteEntry> results_;
};

} // namespace aether::editor
