#pragma once

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/assets/vcs.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The Content Browser (Phase 8 step 6's data, with version control status
// from Phase 26 step 5, docs/design/PHASE_SPECS.md §26.7): the project's
// folders and assets, each with a badge for its git status (M modified, A
// added, ? untracked, D deleted, R renamed, ! conflict; a folder shows its
// worst child), a search, a "changed only" filter, and double-click to open
// a folder or an asset type an extension registered. Draws into the current window.

namespace aether::editor {

class ContentBrowserPanel {
public:
    explicit ContentBrowserPanel(std::filesystem::path project_file);
    void Draw();

    // Rescans the assets and asks git again.
    bool Refresh();
    // The listing of the current folder (or the search), with version control states.
    const std::vector<assets::ContentEntry>& Rows() const { return rows_; }
    const std::string& Folder() const { return folder_; }
    void OpenFolder(const std::string& folder);
    void OpenParent();

    std::string search;
    bool changed_only = false;

    const assets::VcsStatus& Vcs() const { return vcs_; }
    // Uses this status instead of asking git (tests, or a host with its own source).
    void SetVcsStatus(assets::VcsStatus status);
    // How many entries of the current listing are not clean.
    usize ChangedRows() const;
    const std::string& Error() const { return error_; }

private:
    void Rebuild();
    void DrawToolbar();
    void DrawTree();
    void DrawListing();

    std::filesystem::path project_file_;
    std::unique_ptr<assets::AssetDatabase> database_;
    assets::VcsStatus vcs_;
    bool vcs_injected_ = false;
    std::string folder_;
    std::vector<std::string> folders_; // every folder, sorted
    std::vector<assets::ContentEntry> rows_;
    std::string error_;
};

} // namespace aether::editor
