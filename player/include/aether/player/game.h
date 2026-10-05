#pragma once

#include "aether/cook/cooker.h"
#include "aether/ecs/world.h"
#include "aether/input/actions.h"
#include "aether/gameplay/gameplay_effect.h"
#include "aether/input/bindings.h"
#include "aether/math/math.h"
#include "aether/pak/vfs.h"
#include "aether/loc/localization.h"
#include "aether/plugin/plugin.h"
#include "aether/player/settings_apply.h"
#include "aether/player/user_paths.h"
#include "aether/project/project.h"
#include "aether/save/save_system.h"
#include "aether/save/settings.h"
#include "aether/save/world_state.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/prefab.h"
#include "aether/scene/scheduler.h"
#include "aether/sequencer/sequence.h"
#include "aether/sprite2d/tilemap.h"

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

namespace aether::sprite2d {
class Physics2D;
}

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
    // The enabled plugins, and the runtime modules to start, in order (§26.1).
    std::vector<std::string> plugins;
    std::vector<std::string> modules;
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
    // A key for encrypted archives (§25.6); give it before mounting them.
    void AddKey(const pak::PakKey& key) { vfs_.AddKey(key); }
    // Mounts an archive or a folder; false (and `error`) if it can't.
    bool Mount(const std::string& source, int priority = 0, std::string* error = nullptr);
    // Every .apak in `directory`, sorted by name, so a later name (a patch) wins.
    static std::vector<std::string> FindPaks(const std::filesystem::path& directory);

    // Reads and parses Manifest.json from the mounts, then merges each
    // mounted DLC's DLC/<name>.json (its assets join the manifest's).
    bool LoadManifest(std::string* error = nullptr);
    // The DLCs merged, by name.
    const std::vector<std::string>& Dlcs() const { return dlcs_; }
    const GameManifest& Manifest() const { return manifest_; }
    bool HasManifest() const { return has_manifest_; }

    // A cooked asset by its project path, or by GUID; null if it wasn't cooked.
    const GameManifest::Asset* FindAsset(std::string_view path) const;
    const GameManifest::Asset* FindAssetByGuid(std::string_view guid) const;
    // Content/<path>: an asset's (or a helper file's) bytes.
    bool ReadContent(const std::string& path, std::vector<u8>& out, std::string* error = nullptr) const;
    // The path of `path`'s variant for `language` (Textures/logo.fr.png for
    // Textures/logo.png), trying the language's fallback chain (§29.6), else `path`
    // itself. Only cooked assets count.
    std::string LocalizedPath(const std::string& path, const std::string& language, const std::string& default_language = "en") const;
    // ReadContent of LocalizedPath.
    bool ReadContentLocalized(const std::string& path, const std::string& language, std::vector<u8>& out, std::string* error = nullptr) const;

    pak::VirtualFileSystem& Files() { return vfs_; }
    const pak::VirtualFileSystem& Files() const { return vfs_; }

private:
    pak::VirtualFileSystem vfs_;
    GameManifest manifest_;
    bool has_manifest_ = false;
    std::vector<std::string> dlcs_;
    std::unordered_map<std::string, usize> by_path_, by_guid_;
};

struct GameStats {
    u64 frames = 0;
    u64 fixed_steps = 0;
    f64 time = 0.0;
    usize entities = 0;
    usize prefab_instances = 0;
    usize physics_bodies = 0;
    // Scripts and Blueprints (§26.4): what's running, and what has gone wrong
    // (a compile error, a runtime error in a callback). Errors don't stop play.
    usize script_instances = 0;
    usize script_errors = 0;
    usize blueprint_instances = 0;
    usize blueprint_errors = 0;
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
    // Whether this player was built with Luau scripting.
    static bool HasScripting();

    // 2D physics (§26.6): bodies and colliders in the scene, with the solid
    // cells of its tilemaps, step with the 3D physics each fixed step, and
    // PlatformerController2D entities are driven by the "Move" (x axis) and
    // "Jump" actions. Null before a scene is loaded.
    sprite2d::Physics2D* Physics2D() { return physics2d_.get(); }

    // Input (§26.4). The host (the window, or a test) sets keys, buttons and
    // mouse movement here before each Tick; the game's actions come from the
    // cooked .aaction and .amapping assets (every context is active, in name
    // order), and scripts read them through Input.*.
    input::InputState& Input() { return input_state_; }
    input::InputSystem& InputActions() { return input_; }
    const input::InputAssetLibrary& InputAssets() const { return input_library_; }
    // Script and Blueprint problems, as text.
    std::vector<std::string> ScriptErrors() const;
    std::vector<std::string> BlueprintErrors() const;
    // The manifest's runtime modules, started before the first scene loads
    // (and shut down with the game). Missing ones are warnings.
    std::vector<std::string> StartModules();
    std::vector<std::string> StartedModules() const;

    // Saving (Phase 28 step 6, §28.7). Give the game its player folders and it
    // opens a SaveSystem (made the active one, which SaveGames in Blueprints
    // and Luau use) over `paths.saves`, its world context following each
    // loaded scene, and a settings store over `paths.settings`. Without this
    // call there is no save system (Saves() is null). The folders are made on
    // the first write. Call before the first scene is loaded.
    void SetUserPaths(const UserPaths& paths);
    save::SaveSystem* Saves() { return saves_.get(); }
    // The game's text (§29.2): the cooked `.astrings` tables merged, and the
    // language, which follows Settings().language both ways. Always there; made
    // the active Localization (the one Blueprints and scripts use) for this game's life.
    loc::Localization& Localization() { return *localization_; }
    // A cooked asset's path for the game's current language (§29.6): its variant if
    // there is one, else the asset itself.
    std::string LocalizedAsset(const std::string& path) const { return package_.LocalizedPath(path, localization_->Language()); }
    // The player's settings (always there; Load/Save are the host's to call).
    save::SettingsStore<save::GameSettings>& Settings() { return *settings_; }
    // Loads the settings file and applies it through `targets` (kept: later
    // changes made with Settings().Set apply to them too). Returns the
    // warnings (a damaged file gives the defaults, with a line saying so).
    std::vector<std::string> LoadSettings(const SettingsTargets& targets);

private:
    const PrefabData* FindPrefab(const assets::AssetGuid& guid);
    // A level sequence by asset path (cached for the scene; null with a warning if unreadable).
    const seq::LevelSequence* FindSequence(const std::string& path);
    void BuildFrame();
    void LoadInputAssets();
    void WatchSettingsLanguage(); // the settings' language is the Localization's
    void LoadLocalization(); // every StringTable asset in the package, once
    void LoadEffects();      // every GameplayEffect asset in the package
    // (Re)activates every input context with the settings' rebinds applied.
    void ActivateInputContexts();
    void StartRuntime(); // scripts and Blueprints for the loaded scene

    GamePackage& package_;
    std::unique_ptr<World> world_;
    GuidIndex guids_;
    std::unique_ptr<Lifecycle> lifecycle_;
    SystemScheduler scheduler_;
    std::unique_ptr<FrameLoop> loop_;
    std::unordered_map<assets::AssetGuid, std::unique_ptr<PrefabData>> prefabs_;
    std::unordered_map<std::string, std::unique_ptr<seq::LevelSequence>> sequences_;
    std::string scene_;
    std::vector<std::string> warnings_;
    usize prefab_instances_ = 0;
    GameStats stats_;
    struct Physics;
    std::unique_ptr<Physics> physics_;
    std::unique_ptr<sprite2d::Physics2D> physics2d_;
    std::unordered_map<assets::AssetGuid, std::unique_ptr<sprite2d::TilemapData>> tilemaps_;
    std::unordered_map<assets::AssetGuid, std::unique_ptr<sprite2d::Tileset>> tilesets_;
    std::vector<std::pair<std::string, std::unique_ptr<plugin::IModule>>> modules_;
    bool modules_started_ = false;

    // Declared before the scripts, which subscribe to it.
    input::InputSystem input_;
    input::InputState input_state_;
    input::InputAssetLibrary input_library_;
    gas::EffectLibrary effects_; // declared before runtime_, whose effect system refers to it
    std::vector<std::string> effect_warnings_;
    bool effects_loaded_ = false;
    bool input_loaded_ = false;
    struct Runtime;
    std::unique_ptr<Runtime> runtime_; // it holds references to the above
    // Saving: the world context points into the world, guids and lifecycle
    // above, so it is cleared before any of them goes.
    std::unique_ptr<loc::Localization> localization_;
    u64 localization_listener_ = 0;
    bool localization_loaded_ = false;
    std::vector<std::string> localization_warnings_;
    UserPaths user_paths_;
    std::unique_ptr<save::SaveSystem> saves_;
    std::unique_ptr<save::SettingsStore<save::GameSettings>> settings_;
    save::WorldTracker tracker_;
    SettingsTargets settings_targets_;
    u64 settings_observer_ = 0;
    void RefreshSaveContext();
};

} // namespace aether::player
