#pragma once

#include "aether/assets/asset_guid.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::assets {

// Sidecar metadata stored next to every asset source as "<file>.ameta"
// (format: docs/ROADMAP_DETAILS.md §A.2).
struct AssetMeta {
    AssetGuid guid;
    std::string importer;          // "Texture", "Model", "Scene", ...
    u32 importer_version = 1;
    std::string source_hash;       // hash of the source when it was last imported; "" = never
    nlohmann::json settings = nlohmann::json::object(); // importer-specific options
    std::vector<std::string> labels;

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
    // every reference to it — is unchanged. Fails if the target exists.
    bool Move(const AssetGuid& guid, const std::string& new_path, std::string* error = nullptr);

    // Records that the asset's current source has been imported (writes its
    // hash into the .ameta, clearing needs_import).
    bool MarkImported(const AssetGuid& guid, std::string* error = nullptr);

    // Assets that refer to `guid` (the reverse of AssetRecord::dependencies),
    // sorted by path. Dependencies are found by Scan: every scene and prefab
    // file (JSON or binary) is searched for asset GUID strings, which is how
    // AssetRef fields are saved.
    std::vector<const AssetRecord*> Referencers(const AssetGuid& guid) const;

    // Deletes an asset's source and .ameta. Refused while other assets refer
    // to it (the error names them), unless `force`.
    bool Delete(const AssetGuid& guid, bool force, std::string* error = nullptr);

    const std::filesystem::path& ContentRoot() const { return root_; }

private:
    std::filesystem::path Absolute(const std::string& relative) const { return root_ / relative; }

    void RebuildDependencies();

    std::filesystem::path root_;
    std::unordered_map<AssetGuid, std::vector<AssetGuid>> referencers_;
    std::unordered_map<AssetGuid, AssetRecord> records_;
    std::unordered_map<std::string, AssetGuid> by_path_;
};

} // namespace aether::assets
