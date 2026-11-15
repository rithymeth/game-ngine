#pragma once

#include "aether/assets/asset_database.h"
#include "aether/assets/content_browser.h"
#include "aether/assets/importer.h"
#include "aether/assets/vcs.h"
#include "content/thumbnail_store.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The Content Browser (Phase 8 step 6's data, with version control status
// from Phase 26 step 5): the project's
// folders and assets, each with a badge for its git status (M modified, A
// added, ? untracked, D deleted, R renamed, ! conflict; a folder shows its
// worst child), a search, a "changed only" filter, and double-click to open
// a folder or an asset type an extension registered. The listing is a grid of
// thumbnails (textures and materials; other kinds get a type tile) or a list.
// Draws into the current window.

namespace aether::editor {

class ImageTexture;

class ContentBrowserPanel {
public:
    explicit ContentBrowserPanel(std::filesystem::path project_file);
    ~ContentBrowserPanel();
    ContentBrowserPanel(const ContentBrowserPanel&) = delete;
    ContentBrowserPanel& operator=(const ContentBrowserPanel&) = delete;
    void Draw();

    // Rescans the assets and asks git again.
    bool Refresh();
    // The listing of the current folder (or the search), with version control states.
    const std::vector<assets::ContentEntry>& Rows() const { return rows_; }
    // The assets the panel is showing, which is also what the command palette searches. Null
    // before a project loads.
    const assets::AssetDatabase* Database() const { return database_.get(); }
    // Phase 43.6: what is wrong with the assets, filled by Refresh() with the scan -- missing
    // sources and references to GUIDs that no longer exist. Empty is healthy.
    const std::vector<assets::AssetIssue>& Issues() const { return issues_; }
    const std::string& Folder() const { return folder_; }
    void OpenFolder(const std::string& folder);
    void OpenParent();

    std::string search;
    bool changed_only = false;
    // The listing as tiles (the default) or as rows; a host or a test can pick without clicking the toolbar.
    bool grid_view = true;
    // Importer names the type chips keep (empty = every type), and whether the listing includes subfolders. Plain UI
    // state like `search`, so a host (or a headless test) can filter without clicking.
    std::vector<std::string> type_filter;
    bool scope_recursive = false;
    // The list view's columns; clicking a header sorts by it, clicking it again flips the order.
    enum class SortColumn { Name, Type };
    SortColumn sort_column = SortColumn::Name;
    bool sort_descending = false;
    // A type chip: an importer present in the listing before the chips apply, with how many entries it has.
    struct TypeChip {
        std::string importer;
        usize count = 0;
    };
    const std::vector<TypeChip>& TypeChips() const { return chips_; }
    // The entries in scope before the type chips and the changed-only filter, for "3 of 12 items".
    usize UnfilteredCount() const { return base_count_; }
    // Empties the search and clears the type chips and the changed-only filter.
    void ClearFilters();
    // Phase 43.6: the asset issue list. Like `search` and `changed_only` this is plain UI
    // state, public so a host (or a headless test) can open it without clicking.
    bool validation_open = false;
    // The pictures the grid shows; public so a host can read its counters and a test can wait for the worker.
    ThumbnailStore& Thumbnails() { return thumbnails_; }
    const ThumbnailStore& Thumbnails() const { return thumbnails_; }

    // What is selected, as content paths in the order they were picked. Public with the operations below, so a
    // headless test (or a host's menu) can select and act without clicking: every action is one batch and one rescan.
    const std::vector<std::string>& Selection() const { return selection_; }
    bool IsSelected(const std::string& path) const;
    // Replaces the selection with `path`; `toggle` (Ctrl) adds or removes it; `extend` (Shift) takes the range from the
    // anchor to it in the current listing.
    void Select(const std::string& path, bool toggle = false, bool extend = false);
    void SelectAll();
    void ClearSelection();
    // Everything whose tile touches this rectangle (window coordinates), replacing the selection -- or adding to it
    // with `add` (Ctrl+drag). The grid's box-select.
    void SelectInRect(float x0, float y0, float x1, float y1, bool add = false);

    // The selection's actions: one call, one database rescan, one refresh. The asset database has no undo, so the batch
    // is the transaction -- a delete is refused as a whole while anything outside it refers to what it would remove.
    bool DuplicateSelection(std::string* error = nullptr);
    bool DeleteSelection(std::string* error = nullptr);
    bool MoveSelection(const std::string& destination, std::string* error = nullptr);
    // Starts renaming one entry in place (F2 or the context menu); Enter commits, Escape cancels.
    void BeginRename(const std::string& path);
    // Renames one entry, what the inline editor commits with: a name with no extension keeps the asset's, and changing
    // the extension is refused because it would change the type. One rescan; the renamed entry stays selected.
    bool RenameEntry(const std::string& path, const std::string& new_name, std::string* error = nullptr);

    const assets::VcsStatus& Vcs() const { return vcs_; }
    // Phase 43.7: stage everything under Content/ and commit it, then refresh so the badges
    // update. The error is git's own message. Needs the project to be inside a repository.
    bool Commit(const std::string& message, std::string* error = nullptr);
    // The message being typed into the commit box (plain UI state, like `search`).
    std::string commit_message;
    // Uses this status instead of asking git (tests, or a host with its own source).
    void SetVcsStatus(assets::VcsStatus status);
    // How many entries of the current listing are not clean.
    usize ChangedRows() const;
    const std::string& Error() const { return error_; }

    // Stage 3: the import settings drawer and reimport. The drawer is a window, and its state is public like the search
    // box's, so a host (or a headless test) can open it and edit the draft without clicking.
    bool import_open = false;
    std::string import_path;                  // the source asset the drawer edits
    nlohmann::json import_draft;              // the settings being edited
    std::string import_status;                // what the last reimport did
    std::string import_error;                 // why it was refused, or the importer's error
    // Opens the drawer on `path` with its effective settings (defaults plus the .ameta's choices). Refused for folders,
    // sub-assets and kinds with no importer: they have nothing to configure.
    bool OpenImportSettings(const std::string& path, std::string* error = nullptr);
    // Validates and saves ImportDraft into the .ameta, then reimports. The drawer stays open with ImportStatus or
    // ImportError set; the listing rescans either way.
    bool ApplyImportSettings();
    // Imports `path` (a source asset) again with the settings its .ameta holds, then rescans. Fills the drawer's status
    // and error as well as `error`.
    bool Reimport(const std::string& path, std::string* error = nullptr);
    // Imports files from outside the project into the folder the browser shows (an Explorer drop), with the .ameta the
    // scan makes. All or nothing: one refused file (no importer, a folder, a name already used with other contents)
    // copies nothing. Files that are already there unchanged are selected rather than copied. `imported` counts the
    // copies; the new assets become the selection.
    bool ImportFiles(const std::vector<std::string>& files, usize* imported = nullptr, std::string* error = nullptr);

private:
    enum class Action { None, Move, NewFolder };
    enum class Pending { None, Delete, Duplicate, Move, Create, Reimport };
    struct TileRect {
        std::string path;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    void Rebuild();
    void DrawToolbar();
    void DrawTypeChips();
    void DrawTree();
    void DrawListing();
    bool DrawBreadcrumbs(); // true when it changed folder (the rest of the frame is then stale)
    void DrawGrid(std::string& folder_to_open, std::string& asset_to_open);
    void DrawRows(std::string& folder_to_open, std::string& asset_to_open);
    void DrawEntryMenu(const assets::ContentEntry& entry); // what a tile and a row both offer
    void DrawCreateMenu(const std::string& folder);
    void SortBy(SortColumn column);
    void DrawBoxSelect();
    void DrawSelectionKeys();
    bool InlineRename(const assets::ContentEntry& entry);
    void CommitRename();
    void ApplyPending();
    void ApplyAction();
    void DrawActionDialog();
    void DrawReferenceViewer();
    void DrawValidation();
    void DrawImportSettings();
    // The record `path` names when it is a source asset whose kind has an importer in `importers_`, else null.
    const assets::AssetRecord* ImportEditable(const std::string& path) const;
    bool EnsureCache();
    void StartAction(Action action, const std::string& source = {});
    void AcceptMoveDrop(const std::string& destination_folder);
    bool MovePaths(const std::vector<std::string>& paths, const std::string& destination, std::string* error);
    void RebaseOpenFolder(const std::string& old_path, const std::string& new_path);

    std::filesystem::path project_file_;
    std::shared_ptr<assets::AssetDatabase> database_;
    assets::ImporterRegistry importers_; // the built-ins: reimport and the settings drawer use them
    std::unique_ptr<assets::DerivedDataCache> cache_; // Intermediate/DDC, made on the first reimport
    ThumbnailStore thumbnails_;
    struct TileTexture {
        std::unique_ptr<ImageTexture> texture;
        std::string key; // the thumbnail version the pixels were uploaded from
    };
    std::unordered_map<std::string, TileTexture> textures_; // by asset GUID, for the grid
    std::vector<std::string> selection_;
    std::string selection_anchor_;
    std::vector<TileRect> tiles_; // the grid's tiles this frame, for box-select
    bool box_active_ = false;
    bool box_add_ = false;
    float box_x0_ = 0, box_y0_ = 0;
    std::string renaming_;
    std::string rename_buffer_;
    bool rename_focus_ = false;
    bool commit_rename_ = false;
    Pending pending_ = Pending::None;
    std::string pending_destination_;
    std::string pending_create_extension_;
    std::string pending_create_folder_;
    std::string pending_reimport_path_;
    std::vector<TypeChip> chips_;
    usize base_count_ = 0;
    bool search_focus_ = false;
    assets::VcsStatus vcs_;
    bool vcs_injected_ = false;
    std::string folder_;
    std::vector<std::string> folders_; // every folder, sorted
    std::vector<assets::ContentEntry> rows_;
    std::vector<assets::AssetIssue> issues_; // filled by Refresh(), with the scan
    std::string error_;
    Action action_ = Action::None;
    bool action_popup_pending_ = false;
    std::string action_source_;
    std::string action_value_;
    std::string action_error_;
    std::string operation_warning_;
    bool references_open_ = false;
    assets::AssetGuid reference_guid_;
    assets::ReferenceDirection reference_direction_ = assets::ReferenceDirection::Dependencies;
};

} // namespace aether::editor
