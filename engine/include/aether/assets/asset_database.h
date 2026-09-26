#pragma once

#include "aether/assets/asset_guid.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::assets {

// A sub-asset split out of a source file by its importer (a glTF's meshes,
// materials, animations), remembered in the source's .ameta so its GUID is
// stable across reimports.
struct SubAssetMeta {
    std::string key;      // "mesh:0"
    AssetGuid guid;
    std::string importer; // "Mesh"
};

// Sidecar metadata stored next to every asset source as "<file>.ameta"
// (format: docs/ROADMAP_DETAILS.md §A.2).
struct AssetMeta {
    AssetGuid guid;
    std::string importer;          // "Texture", "Model", "Scene", ...
    u32 importer_version = 1;
    std::string source_hash;       // hash of the source when it was last imported; "" = never
    nlohmann::json settings = nlohmann::json::object(); // importer-specific options
    std::vector<std::string> labels;
    std::vector<SubAssetMeta> sub_assets; // sorted by key

    static constexpr const char* kExtension = ".ameta";
};

bool LoadAssetMeta(const std::filesystem::path& meta_file, AssetMeta& out, std::string* error = nullptr);
bool SaveAssetMeta(const std::filesystem::path& meta_file, const AssetMeta& meta, std::string* error = nullptr);

// Which importer handles a source file, by extension (case-insensitive), or
// "" if the file isn't an asset the engine knows. Importers themselves arrive
// in the next Phase 8 steps; this decides which files get an .ameta.
std::string ImporterForExtension(const std::filesystem::path& file);

// "fnv1a64:<16 hex digits>" of a file's bytes, or "" if it can't be read.
std::string HashFile(const std::filesystem::path& file);

struct AssetRecord {
    AssetGuid guid;
    std::string path;        // relative to the content root, '/'-separated: "Textures/brick.png"
    std::string importer;
    std::string source_hash; // current hash of the source file ("" if missing)
    bool missing = false;    // .ameta exists but its source doesn't
    bool needs_import = false; // source changed since it was last imported (or never imported)
    std::vector<AssetGuid> dependencies; // assets this one refers to (scenes, prefabs), sorted
    // Sub-assets: `path` is "<source path>#<key>" ("Models/hero.gltf#mesh:0"),
    // `importer` is the sub-asset type, and `parent` is the source's GUID.
    // They share the source's file, hash and missing state, and are never
    // imported on their own (needs_import is always false).
    AssetGuid parent;          // null for a source asset
    std::string sub_key;       // "" for a source asset
    std::vector<AssetGuid> sub_assets; // a source's sub-assets, in key order

    bool IsSubAsset() const { return !parent.IsNull(); }
};

struct ScanResult {
    usize assets = 0;            // records after the scan
    usize created_meta = 0;      // new sources that got an .ameta
    usize missing = 0;           // .ameta without a source
    usize duplicates_fixed = 0;  // GUIDs reassigned because another asset already had them
    usize needs_import = 0;
    std::vector<std::string> warnings;
};

// The editor's index of every asset under a project's Content/ folder
// (docs/design/PHASE_SPECS.md §8.1-8.2). The runtime player won't use this:
// it reads the cooker's GUID -> package table instead (Phase 25).
class AssetDatabase {
public:
    explicit AssetDatabase(std::filesystem::path content_root);

    // Walks the content folder:
    // - A source with no .ameta gets one, with a new GUID.
    // - An .ameta whose source is gone is kept and marked missing (the file
    //   may come back, e.g. on a version-control branch switch).
    // - Two .ameta files with the same GUID (a file copied together with its
    //   sidecar): the older .ameta keeps it, the newer gets a fresh GUID.
    // - A source whose hash differs from its .ameta's is marked needs_import.
    // - An unreadable .ameta (e.g. a merge conflict) is left untouched and its
    //   asset skipped, with a warning: replacing it would lose the GUID.
    // - Scenes and prefabs are searched for the asset GUIDs they refer to.
    // Hidden files and folders (starting with '.') are skipped.
    ScanResult Scan();

    const AssetRecord* Find(const AssetGuid& guid) const;
    const AssetRecord* FindByPath(const std::string& path) const;
    // Every record, sorted by path.
    std::vector<const AssetRecord*> All() const;

    // Moves/renames an asset's source and .ameta together. Its GUID — and so
    // every reference to it — is unchanged, as are its sub-assets' (whose
    // paths follow). Fails if the target exists, or for a sub-asset (move
    // its source instead).
    bool Move(const AssetGuid& guid, const std::string& new_path, std::string* error = nullptr);

    // Replaces a source's sub-assets with `sub_assets` (key + importer; guid
    // ignored): keys it already had keep their GUIDs, new keys get new ones,
    // and keys no longer produced are removed. Writes the .ameta. Returns the
    // GUIDs in the order given (empty on error).
    std::vector<AssetGuid> SetSubAssets(const AssetGuid& source, const std::vector<SubAssetMeta>& sub_assets,
                                        std::string* error = nullptr);

    // Records that the asset's current source has been imported (writes its
    // hash into the .ameta, clearing needs_import).
    // `importer_version`, if non-zero, is also recorded.
    bool MarkImported(const AssetGuid& guid, std::string* error = nullptr, u32 importer_version = 0);

    // Absolute paths of an asset's source and .ameta ("" if unknown GUID).
    std::filesystem::path SourcePath(const AssetGuid& guid) const;
    std::filesystem::path MetaPath(const AssetGuid& guid) const;

    // Assets that refer to `guid` (the reverse of AssetRecord::dependencies),
    // sorted by path. Dependencies are found by Scan: every scene and prefab
    // file (JSON or binary) is searched for asset GUID strings, which is how
    // AssetRef fields are saved; a .gltf depends on the texture assets its
    // image URIs point at.
    std::vector<const AssetRecord*> Referencers(const AssetGuid& guid) const;

    // Deletes an asset's source and .ameta, with its sub-assets. Refused
    // while other assets refer to it or its sub-assets (the error names
    // them), unless `force`. A sub-asset can't be deleted on its own.
    bool Delete(const AssetGuid& guid, bool force, std::string* error = nullptr);

    const std::filesystem::path& ContentRoot() const { return root_; }

private:
    std::filesystem::path Absolute(const std::string& relative) const { return root_ / relative; }

    void RebuildDependencies();
    void EraseRecord(const AssetGuid& guid);

    std::filesystem::path root_;
    std::unordered_map<AssetGuid, std::vector<AssetGuid>> referencers_;
    std::unordered_map<AssetGuid, AssetRecord> records_;
    std::unordered_map<std::string, AssetGuid> by_path_;
};

} // namespace aether::assets
