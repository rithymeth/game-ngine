#pragma once

#include "aether/assets/asset_database.h"
#include "aether/pak/pak.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
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
[[nodiscard]] bool ParseConfiguration(const std::string& name, BuildConfiguration& out);

struct CookOptions {
    std::filesystem::path project_file;  // the .aproject
    std::filesystem::path output_dir;    // gets <pak_name>.apak and CookManifest.json
    BuildConfiguration configuration = BuildConfiguration::Development;
    // Cooked in addition to the startup scene and the project's always_cook:
    // asset paths, or folders ending in '/'.
    std::vector<std::string> always_cook;
    pak::CompressionPolicy compression = pak::CompressionPolicy::Auto;
    bool include_imported = true;        // run importers and store their output
    // Textures: block-compressed with mips (Cooked/<guid>.atex), per their
    // .ameta settings ("cook_format": auto|rgba8|bc1|bc3|bc4|bc5|bc7,
    // "normal_map", "srgb", "mips").
    bool cook_textures = true;
    u32 texture_quality = 2;             // 0 (fastest) .. 4 (best)
    // Refuse to write an archive when scanning, importing, or cooking reports
    // any warning. Use for release candidates that require a clean content set.
    bool strict_validation = false;
    std::string pak_name = "Game";
    std::string platform = "default";    // for the derived data cache
    // Called as the cook goes (from the cooking thread): the fraction done,
    // 0..1, and what it's doing now ("Cooking Textures/a.png").
    std::function<void(f32 fraction, const std::string& stage)> progress;
    // Set (from any thread) to stop between assets; the cook then fails
    // with "Cancelled" and writes nothing.
    const std::atomic<bool>* cancel = nullptr;

    // Phase 25 step 6 (§25.6).
    // Encrypts the archive (see pak::PakKey); also opens an encrypted base.
    std::optional<pak::PakKey> encryption_key;
    // A patch: the full cook is compared with this earlier archive, and only
    // what's new or changed is written (with the removed paths listed).
    std::filesystem::path patch_base;
    // A DLC: only `always_cook` (not the startup scene or the project's
    // always_cook) is cooked, skipping assets `dlc_base` already has, and
    // the manifest goes to DLC/<dlc_name>.json, which the game merges.
    std::string dlc_name;
    std::filesystem::path dlc_base;
};

struct CookedAsset {
    assets::AssetGuid guid;
    std::string path;      // content path
    std::string importer;
    std::string reason;    // "startup scene", "always cook", "used by <path>"
    u64 bytes = 0;         // as stored in the archive, before compression
    bool imported = false; // its importer's output is in the archive too
    std::string cooked;    // its cooked form in the archive ("Cooked/<guid>.atex"), if any
    u64 cooked_bytes = 0;
};

struct CookReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<CookedAsset> assets;          // in path order
    std::vector<std::string> extra_files;     // helper files (a .gltf's .bin, ...)
    usize skipped = 0;                        // assets nothing reaches
    usize stripped_fields = 0;                // editor-only fields removed
    usize texture_cache_hits = 0;             // cooked textures reused from the derived data cache
    usize texture_cache_misses = 0;           // cooked textures generated or regenerated
    u64 original_bytes = 0;                   // content as stored, before compression
    u64 pak_bytes = 0;                        // the archive on disk
    std::filesystem::path pak_file;
    std::filesystem::path manifest_file;
    // A patch cook: what the patch archive holds against its base.
    bool is_patch = false;
    pak::PatchReport patch;
    // A DLC cook: assets left out because the base archive has them.
    usize in_base = 0;
    // Phase 26 step 1: the enabled plugins (dependency order), and the
    // runtime modules the game starts, in start order.
    std::vector<std::string> plugins;
    std::vector<std::string> modules;
};

// Cooks a project. The archive holds:
//   Content/<path>        each reachable asset (scenes and prefabs stripped
//                          and minified), and the helper files .gltf models
//                          reference
//   Imported/<guid>.bin   each asset's (and sub-asset's) importer output
//   Cooked/<guid>.atex    each texture, block-compressed with its mips
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
