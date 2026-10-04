#pragma once

#include "aether/cook/cooker.h"
#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/pak/vfs.h"
#include "aether/project/project.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/prefab.h"
#include "aether/scene/scheduler.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The player's game runtime (Phase 25 step 4, docs/design/PHASE_SPECS.md
// §25.4): a cooked game, without the editor. GamePackage mounts the cooked
// .apak archives and reads their Manifest.json; Game loads the startup
// scene from them (prefab instances resolved from the archives too) and
// runs it on the fixed-timestep frame loop, with physics when the engine
// has it. The aether_player executable puts a window around this.

namespace aether::player {

// Manifest.json, as the cooker writes it (§25.2).
struct GameManifest {
    struct Asset {
        std::string guid;
        std::string path;
        std::string importer;
        bool imported = false;
        std::string cooked; // its cooked form in the archive (Cooked/<guid>.atex), or empty
        std::string cooked_format;
    };
    std::string project;
    cook::BuildConfiguration configuration = cook::BuildConfiguration::Development;
    std::string startup_scene;
    f32 fixed_timestep_hz = 60.0f;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    std::vector<std::string> layers;
    std::vector<u32> collision_matrix;
    // The window (§25.5): the title is the project's name unless set.
    std::string window_title;
    u32 window_width = 1280;
    u32 window_height = 720;
    bool vsync = true;
    std::vector<QualityPreset> quality_presets;
    std::string default_quality;
    std::vector<Asset> assets;
};

bool ParseGameManifest(std::string_view text, GameManifest& out, std::string* error = nullptr);

// The preset to run with: `requested` if it names one, else the manifest's
// default, else the first; null when there are none.
const QualityPreset* ChooseQuality(const GameManifest& manifest, const std::string& requested = "");

// The cooked game's files: one or more mounted .apak archives (or folders,
// for a loose cook), later mounts with a higher priority winning.
class GamePackage {
public:
    // Mounts an archive or a folder; false (and `error`) if it can't.
    bool Mount(const std::string& source, int priority = 0, std::string* error = nullptr);
    // Every .apak in `directory`, sorted by name, so a later name (a patch) wins.
    static std::vector<std::string> FindPaks(const std::filesystem::path& directory);

    // Reads and parses Manifest.json from the mounts.
    bool LoadManifest(std::string* error = nullptr);
    const GameManifest& Manifest() const { return manifest_; }
    bool HasManifest() const { return has_manifest_; }

    // A cooked asset by its project path, or by GUID; null if it wasn't cooked.
    const GameManifest::Asset* FindAsset(std::string_view path) const;
    const GameManifest::Asset* FindAssetByGuid(std::string_view guid) const;
    // Content/<path>: an asset's (or a helper file's) bytes.
    bool ReadContent(const std::string& path, std::vector<u8>& out, std::string* error = nullptr) const;

    pak::VirtualFileSystem& Files() { return vfs_; }
    const pak::VirtualFileSystem& Files() const { return vfs_; }

private:
    pak::VirtualFileSystem vfs_;
    GameManifest manifest_;
    bool has_manifest_ = false;
    std::unordered_map<std::string, usize> by_path_, by_guid_;
};

struct GameStats {
    u64 frames = 0;
    u64 fixed_steps = 0;
    f64 time = 0.0;
    usize entities = 0;
    usize prefab_instances = 0;
    usize physics_bodies = 0;
};

class Game {
public:
    explicit Game(GamePackage& package);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    // The manifest's startup scene.
    bool LoadStartupScene(std::string* error = nullptr);
    // Replaces the world with a cooked scene (JSON or binary), its prefab
    // instances resolved from the package. Ends play first if it's running.
    bool LoadScene(const std::string& path, std::string* error = nullptr);
    const std::string& SceneName() const { return scene_; }

    // OnCreate/OnEnable/OnStart for the scene's entities; physics bodies are made then.
    void BeginPlay();
    void EndPlay();
    bool IsPlaying() const { return lifecycle_ && lifecycle_->IsPlaying(); }

    // One frame of `dt` seconds: PreUpdate, the fixed steps owed (physics,
    // then lifecycle FixedUpdate), Update, LateUpdate, PreRender.
    FrameContext Tick(f32 dt);

    World& GetWorld() { return *world_; }
    GuidIndex& Guids() { return guids_; }
    Lifecycle& GetLifecycle() { return *lifecycle_; }
    // Add game systems here, before the first Tick.
    SystemScheduler& Systems() { return scheduler_; }
    GameStats Stats() const;
    // Problems found while loading (an unresolved prefab instance, ...).
    const std::vector<std::string>& Warnings() const { return warnings_; }
    static bool HasPhysics();

private:
    const PrefabData* FindPrefab(const assets::AssetGuid& guid);
    void BuildFrame();

    GamePackage& package_;
    std::unique_ptr<World> world_;
    GuidIndex guids_;
    std::unique_ptr<Lifecycle> lifecycle_;
    SystemScheduler scheduler_;
    std::unique_ptr<FrameLoop> loop_;
    std::unordered_map<assets::AssetGuid, std::unique_ptr<PrefabData>> prefabs_;
    std::string scene_;
    std::vector<std::string> warnings_;
    usize prefab_instances_ = 0;
    GameStats stats_;
    struct Physics;
    std::unique_ptr<Physics> physics_;
};

} // namespace aether::player
