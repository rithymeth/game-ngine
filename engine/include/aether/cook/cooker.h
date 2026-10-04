#pragma once

#include "aether/assets/asset_database.h"
#include "aether/pak/pak.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// The cooker (Phase 25 step 2, docs/design/PHASE_SPECS.md §25.2): turns a
// project's content into what a packaged game ships - only the assets the
// game can reach, with editor-only data stripped, imported data alongside,
// and a manifest - in one .apak archive.
namespace aether::cook {

enum class BuildConfiguration : u8 { Debug, Development, Shipping };
const char* ConfigurationName(BuildConfiguration c);
bool ParseConfiguration(const std::string& name, BuildConfiguration& out);

struct CookOptions {
    std::filesystem::path project_file;  // the .aproject
    std::filesystem::path output_dir;    // gets <pak_name>.apak and CookManifest.json
    BuildConfiguration configuration = BuildConfiguration::Development;
    // Cooked in addition to the startup scene and the project's always_cook:
    // asset paths, or folders ending in '/'.
    std::vector<std::string> always_cook;
    pak::CompressionPolicy compression = pak::CompressionPolicy::Auto;
    bool include_imported = true;        // run importers and store their output
    std::string pak_name = "Game";
    std::string platform = "default";    // for the derived data cache
};

struct CookedAsset {
    assets::AssetGuid guid;
    std::string path;      // content path
    std::string importer;
    std::string reason;    // "startup scene", "always cook", "used by <path>"
    u64 bytes = 0;         // as stored in the archive, before compression
    bool imported = false; // its importer's output is in the archive too
};

struct CookReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<CookedAsset> assets;          // in path order
    std::vector<std::string> extra_files;     // helper files (a .gltf's .bin, ...)
    usize skipped = 0;                        // assets nothing reaches
    usize stripped_fields = 0;                // editor-only fields removed
    u64 original_bytes = 0;                   // content as stored, before compression
    u64 pak_bytes = 0;                        // the archive on disk
    std::filesystem::path pak_file;
    std::filesystem::path manifest_file;
};

// Cooks a project. The archive holds:
//   Content/<path>        each reachable asset (scenes and prefabs stripped
//                          and minified), and the helper files .gltf models
//                          reference
//   Imported/<guid>.bin   each asset's (and sub-asset's) importer output
//   Manifest.json         the project's runtime settings, the configuration,
//                          and every cooked asset: GUID, path, importer,
//                          dependencies, whether it has imported data
CookReport Cook(const CookOptions& options);

// The building blocks, for tools and tests.

// Every asset reachable from `roots` (paths or folders ending in '/'),
// following dependencies and sub-assets to their sources; `reasons` gets
// why each is included. Unknown roots are warnings.
std::vector<assets::AssetGuid> CollectCookSet(const assets::AssetDatabase& database,
                                              const std::vector<std::pair<std::string, std::string>>& roots,
                                              std::map<assets::AssetGuid, std::string>& reasons,
                                              std::vector<std::string>& warnings);

// Removes Field_EditorOnly fields from a scene's or prefab's components
// (looked up by their reflected names, nested structs and arrays included);
// returns how many it removed.
usize StripEditorOnly(nlohmann::json& document);

} // namespace aether::cook
