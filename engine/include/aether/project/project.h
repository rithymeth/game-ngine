#pragma once

#include "aether/core/version.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <filesystem>
#include <string>
#include <vector>

namespace aether {

// A game project (Phase 7, docs/ROADMAP.md step 8; format in
// docs/ROADMAP_DETAILS.md §A.1). The .aproject file holds these settings; the
// folder around it holds the project's content:
//
//   MyGame/
//     MyGame.aproject
//     Content/       assets (committed to version control)
//     Config/        per-platform settings overrides
//     Source/        C++ game module (optional)
//     Saved/         layout, logs, autosaves      (not committed)
//     Intermediate/  derived data cache, builds   (not committed)
struct ProjectSettings {
    std::string name;
    std::string engine_version = kEngineVersion; // engine that last saved the project
    // Scene to open at startup, relative to Content/. Becomes an asset GUID
    // with the asset database (Phase 8).
    std::string startup_scene;
    // Content cooked even when nothing the startup scene reaches refers to
    // it (Phase 25): asset paths, or folders ending in '/'.
    std::vector<std::string> always_cook;
    std::vector<std::string> plugins;
    // Physics
    f32 fixed_timestep_hz = 60.0f;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    // Named collision layers (up to 32); index 0 is always "Default".
    std::vector<std::string> layers{"Default"};
    // Which layers collide (Phase 13 step 2): entry i is the mask of layers
    // layer i collides with. Missing entries mean "all", so an empty list
    // (every older project) has every layer colliding with every other.
    // Edit through SetLayersCollide (gameplay.h), which keeps it symmetric.
    std::vector<u32> collision_matrix;
};

inline constexpr const char* kProjectExtension = ".aproject";

// Where everything in a project lives, derived from its .aproject path.
struct ProjectPaths {
    std::filesystem::path file;
    std::filesystem::path root;
    std::filesystem::path content;
    std::filesystem::path config;
    std::filesystem::path source;
    std::filesystem::path saved;
    std::filesystem::path intermediate;

    static ProjectPaths ForFile(const std::filesystem::path& project_file);
};

// Creates `parent_dir/name/` with the standard layout, a .gitignore for the
// generated folders, and `name.aproject`. Fails (returning false with a
// message in `error`) if the name isn't a valid folder name or the folder
// already exists and isn't empty.
bool CreateProject(const std::filesystem::path& parent_dir, const std::string& name, ProjectPaths* out_paths,
                   std::string* error);

// Loads project settings. Missing fields keep their defaults and unknown
// ones are ignored (the reflection archive's usual tolerance); a file that
// isn't JSON, or isn't a project file, fails. Warnings (e.g. a project saved
// by a newer engine) go to `warnings` if given, otherwise to the log.
bool LoadProject(const std::filesystem::path& project_file, ProjectSettings& out, std::string* error,
                 std::vector<std::string>* warnings = nullptr);

// Writes the settings (deterministic JSON, stamped with this engine version).
bool SaveProject(const std::filesystem::path& project_file, const ProjectSettings& settings, std::string* error);

// Makes sure Content/, Config/, Saved/ and Intermediate/ exist (e.g. after a
// fresh clone, where the ignored folders are missing).
bool EnsureProjectFolders(const ProjectPaths& paths, std::string* error);

} // namespace aether

AETHER_REFLECT(aether::ProjectSettings, 1,
    AETHER_FIELD(name, Field_EditAnywhere, {.category = "Project"}),
    AETHER_FIELD(engine_version, Field_ReadOnly, {.category = "Project"}),
    AETHER_FIELD(startup_scene, Field_EditAnywhere, {.tooltip = "Scene opened at startup, relative to Content/", .category = "Project"}),
    AETHER_FIELD(always_cook, Field_EditAnywhere, {.tooltip = "Content cooked even if unreferenced: asset paths, or folders ending in /", .category = "Packaging"}),
    AETHER_FIELD(plugins, Field_EditAnywhere, {.category = "Project"}),
    AETHER_FIELD(fixed_timestep_hz, Field_EditAnywhere, {.category = "Physics", .range_min = 10, .range_max = 480, .units = "Hz"}),
    AETHER_FIELD(gravity, Field_EditAnywhere, {.category = "Physics", .units = "m/s2"}),
    AETHER_FIELD(layers, Field_EditAnywhere, {.tooltip = "Collision layers; the first is always Default", .category = "Physics"}),
    AETHER_FIELD(collision_matrix, Field_ReadOnly, {.tooltip = "Per layer, the mask of layers it collides with (edit in the collision matrix)", .category = "Physics"})
)
