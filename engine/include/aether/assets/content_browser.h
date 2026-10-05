#pragma once

#include "aether/assets/importer.h"
#include "aether/assets/vcs.h"

#include <optional>
#include <string>
#include <vector>

namespace aether::assets {

// The data behind the editor's Content Browser (Phase 8 step 6,
// docs/design/EDITOR_UI.md §6.5): folder tree, listing and search, safe
// rename/move, the Reference Viewer's graph, and thumbnails. The ImGui panel
// draws from these; they know nothing about UI.

// ---------------------------------------------------------------------------
// Folders and listing
// ---------------------------------------------------------------------------

// Every folder under the content root ('/'-separated, relative, sorted), including
// empty ones; "" (the root itself) is not listed. Hidden folders are skipped.
std::vector<std::string> ContentFolders(const AssetDatabase& database);

struct ContentEntry {
    bool is_folder = false;
    std::string name; // file or folder name; for a sub-asset, its key ("mesh:0")
    std::string path; // relative to the content root
    const AssetRecord* asset = nullptr; // null for folders
    VcsState vcs = VcsState::Clean;     // set by ApplyVcsStatus
};

struct ContentQuery {
    std::string folder;                  // "" = the content root
    std::string search;                  // case-insensitive substring of the name; searches subfolders too
    std::vector<std::string> types;      // importer names to show ("Texture", ...); empty = all
    bool recursive = false;              // include subfolders' assets (implied by a search)
    bool include_sub_assets = false;     // list a model's meshes/materials/animations too
};

// Folders first (only when not searching or recursing), then assets, each
// sorted by name (case-insensitive). Invalidated by any database change.
std::vector<ContentEntry> ListContent(const AssetDatabase& database, const ContentQuery& query);

// Fills each entry's `vcs` from a status queried in the content root
// (QueryGitStatus(database.ContentRoot())): an asset takes the worse of its
// own file's state and its .ameta sidecar's (a sub-asset follows its source),
// and a folder the worst of everything inside it. Returns how many entries
// are not Clean. A status that isn't `available` leaves everything Clean.
usize ApplyVcsStatus(std::vector<ContentEntry>& entries, const VcsStatus& status);

// ---------------------------------------------------------------------------
// Rename, move, create
// ---------------------------------------------------------------------------

// A valid file or folder name on every platform: not empty, no path
// separators or characters Windows forbids (<>:"/\|?*), no '#' (sub-asset
// paths use it), no leading '.' (hidden), no trailing '.' or space, not a
// reserved Windows device name (CON, NUL, COM1, ...).
bool IsValidAssetName(const std::string& name, std::string* error = nullptr);

struct MoveResult {
    bool ok = false;
    std::string error;
    // .gltf files whose relative URIs were rewritten to keep pointing at the
    // same files (they're reimported on the next scan/import).
    std::vector<std::string> rewritten;
};

// Moves an asset source or a whole folder (with everything in it, .ameta and
// non-asset files like a glTF's .bin included) to `to`, both relative to the
// content root.
// - GUID references (scenes, prefabs, AssetRef fields) need no change.
// - Relative file references in .gltf files — a moved model's own buffers and
//   images, and other models' references to moved textures — are rewritten.
// - Refused if `to` exists, `to` is inside `from`, or `from` is a sub-asset.
// Rescans the database afterwards.
MoveResult MoveContent(AssetDatabase& database, const std::string& from, const std::string& to);

// Renames in place. `new_name` without an extension keeps the old one;
// changing a file's extension is refused (it would change its importer).
MoveResult RenameContent(AssetDatabase& database, const std::string& path, const std::string& new_name);

// Creates `parent/name` (parent "" = the content root).
bool CreateContentFolder(AssetDatabase& database, const std::string& parent, const std::string& name,
                         std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Reference Viewer
// ---------------------------------------------------------------------------

enum class ReferenceDirection {
    Dependencies, // what this asset uses
    Referencers,  // what uses this asset (including through its sub-assets)
};

struct ReferenceNode {
    AssetGuid guid;
    std::string path;
    u32 depth = 0;        // 0 = the asset asked about
    bool repeated = false; // already listed above (a shared dependency or a cycle); not expanded again
};

// Depth-first, in path order: the asset, then what it uses (or what uses it),
// then theirs, down to `max_depth`.
std::vector<ReferenceNode> CollectReferences(const AssetDatabase& database, const AssetGuid& guid,
                                             ReferenceDirection direction, u32 max_depth = 8);

// ---------------------------------------------------------------------------
// Thumbnails
// ---------------------------------------------------------------------------

struct Thumbnail {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> rgba; // RGBA8, rows top to bottom
};

// Scales an RGBA8 image to fit within `max_size` x `max_size`, keeping its
// aspect ratio (box filter; never enlarges).
Thumbnail MakeThumbnail(const std::vector<u8>& rgba, u32 width, u32 height, u32 max_size);

// A thumbnail for textures (from the imported mips) and materials (the base
// colour texture tinted by the base colour, or a swatch of the colour).
// nullopt for kinds that need the renderer (models, meshes) or that failed
// to import; the Content Browser shows a type icon instead.
std::optional<Thumbnail> AssetThumbnail(AssetDatabase& database, const AssetGuid& guid,
                                        const ImporterRegistry& importers, DerivedDataCache& cache, u32 max_size);

} // namespace aether::assets
