#include "aether/player/game.h"
#include "game_runtime.h"
#include "aether/renderer/components.h"
#include "aether/ui/controls.h"
#include "aether/core/json_util.h"
#include "aether/audio/backend.h"
#include "aether/audio/audio_system.h"

#include "aether/sequencer/sequence_system.h"
#include "aether/assets/gltf_loader.h"

#include "aether/core/log.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/effect_system.h"
#include "aether/gameplay/gameplay_kit.h"
#include "aether/loc/localized_path.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "aether/sprite2d/components.h"
#include "aether/sprite2d/lights2d.h"
#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

#include "aether/blueprint/graph.h"
#if AETHER_GAME_PHYSICS
#include "aether/job/job_system.h"
#include "aether/physics/components.h"
#include "aether/physics/character.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics/physics_world.h"
#endif
#if AETHER_GAME_SCRIPTING
#include "aether/script/ai_api.h"
#endif

namespace aether::player {

using nlohmann::json;
namespace stdfs = std::filesystem;

namespace {

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// The manifest
// ---------------------------------------------------------------------------

bool ParseGameManifest(std::string_view text, GameManifest& out, std::string* error) {
    const json m = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (m.is_discarded() || !m.is_object()) return Fail(error, "Manifest.json isn't JSON");
    const auto type = m.find("$type");
    if (type == m.end() || !type->is_string() || type->get<std::string>() != "CookManifest") {
        return Fail(error, "Manifest.json isn't a cook manifest");
    }
    if (JsonVersion(m) != 1) return Fail(error, "Manifest.json has an unsupported version");
    try {
        GameManifest g;
        g.project = m.value("project", "");
        if (!cook::ParseConfiguration(m.value("configuration", "Development"), g.configuration)) {
            return Fail(error, "Manifest.json has an unknown configuration");
        }
        g.startup_scene = m.value("startup_scene", "");
        g.fixed_timestep_hz = m.value("fixed_timestep_hz", 60.0f);
        if (!(g.fixed_timestep_hz > 0.0f)) return Fail(error, "Manifest.json's fixed timestep must be above 0 Hz");
        if (const auto it = m.find("gravity"); it != m.end() && it->is_array() && it->size() == 3) {
            g.gravity = Vec3((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>());
        }
        g.layers = m.value("layers", std::vector<std::string>{});
        g.collision_matrix = m.value("collision_matrix", std::vector<u32>{});
        g.window_title = g.project;
        if (const auto it = m.find("window"); it != m.end() && it->is_object()) {
            g.window_title = it->value("title", g.project);
            g.window_width = it->value("width", 1280u);
            g.window_height = it->value("height", 720u);
            g.vsync = it->value("vsync", true);
            if (g.window_width == 0 || g.window_height == 0) return Fail(error, "Manifest.json's window has no size");
        }
        if (const auto it = m.find("quality_presets"); it != m.end() && it->is_array()) {
            for (const json& p : *it) {
                QualityPreset preset;
                if (!p.is_object() || !reflect::FromJson(reflect::Reflect<QualityPreset>(), &preset, p) || preset.name.empty()) {
                    return Fail(error, "Manifest.json has a quality preset that can't be read");
                }
                g.quality_presets.push_back(std::move(preset));
            }
        }
        g.default_quality = m.value("default_quality", "");
        g.plugins = m.value("plugins", std::vector<std::string>{});
        g.modules = m.value("modules", std::vector<std::string>{});
        if (const auto it = m.find("assets"); it != m.end() && it->is_array()) {
            for (const json& a : *it) {
                GameManifest::Asset asset;
                asset.guid = a.value("guid", "");
                asset.path = a.value("path", "");
                asset.importer = a.value("importer", "");
                asset.imported = a.value("imported", false);
                asset.cooked = a.value("cooked", "");
                asset.cooked_format = a.value("cooked_format", "");
                if (asset.guid.empty() || asset.path.empty()) return Fail(error, "Manifest.json lists an asset with no GUID or path");
                g.assets.push_back(std::move(asset));
            }
        }
        out = std::move(g);
    } catch (const json::exception& e) {
        return Fail(error, std::string("Manifest.json: ") + e.what());
    }
    return true;
}

RenderSize ScaledRenderSize(u32 window_width, u32 window_height, f32 resolution_scale) {
    const f32 scale = std::clamp(resolution_scale > 0.0f ? resolution_scale : 1.0f, 0.25f, 1.0f);
    const auto side = [&](u32 n) { return std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(n) * scale))); };
    return {side(window_width), side(window_height)};
}

const QualityPreset* ChooseQuality(const GameManifest& manifest, const std::string& requested) {
    for (const std::string& name : {requested, manifest.default_quality}) {
        for (const QualityPreset& p : manifest.quality_presets) {
            if (!name.empty() && p.name == name) return &p;
        }
    }
    return manifest.quality_presets.empty() ? nullptr : &manifest.quality_presets.front();
}

// ---------------------------------------------------------------------------
// The package
// ---------------------------------------------------------------------------

bool GamePackage::Mount(const std::string& source, int priority, std::string* error) {
    return vfs_.Mount(source, "", priority, error);
}

std::vector<std::string> GamePackage::FindPaks(const stdfs::path& directory) {
    std::vector<std::string> paks;
    std::error_code ec;
    for (stdfs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".apak") paks.push_back(it->path().string());
    }
    std::sort(paks.begin(), paks.end());
    return paks;
}

bool GamePackage::LoadManifest(std::string* error) {
    std::string text, read_error;
    if (!vfs_.ReadText("Manifest.json", text, &read_error)) {
        return Fail(error, vfs_.MountCount() == 0 ? "Nothing is mounted" : "No Manifest.json in the mounted archives");
    }
    GameManifest manifest;
    if (!ParseGameManifest(text, manifest, error)) return false;
    // DLCs: their manifests list their assets.
    std::vector<std::string> dlcs;
    for (const std::string& path : vfs_.List("DLC/")) {
        if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0) continue;
        std::string dlc_text;
        if (!vfs_.ReadText(path, dlc_text, &read_error)) return Fail(error, read_error);
        const json d = json::parse(dlc_text, nullptr, /*allow_exceptions=*/false);
        if (d.is_discarded() || !d.is_object() || JsonString(d, "$type") != "DlcManifest") {
            return Fail(error, path + " isn't a DLC manifest");
        }
        if (JsonVersion(d) != 1) return Fail(error, path + " has an unsupported version");
        GameManifest part;
        json as_base = {{"$type", "CookManifest"}, {"$version", 1}, {"assets", d.value("assets", json::array())}};
        if (!ParseGameManifest(as_base.dump(), part, error)) return false;
        for (GameManifest::Asset& asset : part.assets) manifest.assets.push_back(std::move(asset));
        dlcs.push_back(d.value("name", path.substr(4, path.size() - 9)));
    }
    dlcs_ = std::move(dlcs);
    manifest_ = std::move(manifest);
    by_path_.clear();
    by_guid_.clear();
    for (usize i = 0; i < manifest_.assets.size(); ++i) {
        by_path_[manifest_.assets[i].path] = i;
        by_guid_[manifest_.assets[i].guid] = i;
    }
    has_manifest_ = true;
    return true;
}

const GameManifest::Asset* GamePackage::FindAsset(std::string_view path) const {
    const auto it = by_path_.find(std::string(path));
    return it == by_path_.end() ? nullptr : &manifest_.assets[it->second];
}

const GameManifest::Asset* GamePackage::FindAssetByGuid(std::string_view guid) const {
    const auto it = by_guid_.find(std::string(guid));
    return it == by_guid_.end() ? nullptr : &manifest_.assets[it->second];
}

bool GamePackage::ReadContent(const std::string& path, std::vector<u8>& out, std::string* error) const {
    return vfs_.Read("Content/" + path, out, error);
}

std::string GamePackage::LocalizedPath(const std::string& path, const std::string& language, const std::string& default_language) const {
    for (const std::string& candidate : loc::LocalizedCandidates(path, language, default_language)) {
        if (FindAsset(candidate) != nullptr) return candidate;
    }
    return path;
}

bool GamePackage::ReadContentLocalized(const std::string& path, const std::string& language, std::vector<u8>& out, std::string* error) const {
    return ReadContent(LocalizedPath(path, language), out, error);
}

// ---------------------------------------------------------------------------
// The game
// ---------------------------------------------------------------------------

#if AETHER_GAME_PHYSICS
struct Game::Physics {
    JobSystem jobs{2};
    PhysicsWorld world{jobs};
    std::unique_ptr<PhysicsScene> scene;
    std::unique_ptr<CharacterSystem> characters;
};
#else
struct Game::Physics {};
#endif

bool Game::HasPhysics() { return AETHER_GAME_PHYSICS != 0; }
bool Game::HasScripting() { return AETHER_GAME_SCRIPTING != 0; }

PhysicsScene* Game::PhysicsQueries() {
#if AETHER_GAME_PHYSICS
    return physics_ && physics_->scene ? physics_->scene.get() : nullptr;
#else
    return nullptr;
#endif
}

Game::Game(GamePackage& package)
    : package_(package), world_(std::make_unique<World>()), settings_(std::make_unique<save::SettingsStore<save::GameSettings>>(std::filesystem::path())) {
    lifecycle_ = std::make_unique<Lifecycle>(*world_, guids_);
    platform_listener_ = platform_lifecycle_->Subscribe([this](const platform::LifecycleMessage& message) { OnPlatformEvent(message); });
    BuildKits();
    localization_ = std::make_unique<loc::Localization>();
    localization_->MakeActive();
    // A language chosen in play (Localization.SetLanguage) is the player's setting too.
    WatchSettingsLanguage();
    localization_listener_ = localization_->AddListener([this](const std::string& now, const std::string&) {
        if (settings_ && settings_->Get().language != now) {
            save::GameSettings next = settings_->Get();
            next.language = now;
            settings_->Set(next);
        }
    });
#if AETHER_GAME_PHYSICS
    // Registered by name, so scenes can name them.
    (void)GetComponentId<RigidBody>();
    (void)GetComponentId<BoxCollider>();
    (void)GetComponentId<SphereCollider>();
    (void)GetComponentId<CapsuleCollider>();
    (void)GetComponentId<ConvexCollider>();
    (void)GetComponentId<MeshCollider>();
    (void)GetComponentId<PhysicsJoint>();
#endif
}

Game::~Game() {
    if (platform_lifecycle_ && platform_listener_ != 0) platform_lifecycle_->Unsubscribe(platform_listener_);
    EndPlay();
    localization_.reset();
    saves_.reset(); // drains queued saves; its world context points into what's below
    runtime_.reset(); // scripts and Blueprints before the world, lifecycle and input they use
    physics2d_.reset();
    physics_.reset(); // its bodies before the world they belong to
    while (!modules_.empty()) {
        modules_.back().second->Shutdown();
        modules_.pop_back();
    }
}

void Game::UsePlatformLifecycle(platform::PlatformLifecycle& lifecycle) {
    if (platform_lifecycle_ == &lifecycle) return;
    if (platform_lifecycle_ && platform_listener_ != 0) platform_lifecycle_->Unsubscribe(platform_listener_);
    platform_lifecycle_ = &lifecycle;
    platform_listener_ = platform_lifecycle_->Subscribe([this](const platform::LifecycleMessage& message) { OnPlatformEvent(message); });
    if (platform_lifecycle_->Suspended()) OnPlatformEvent({platform::LifecycleEvent::Suspend, platform::MemoryLevel::Moderate});
}

void Game::RefreshSaveContext() {
    if (!saves_) return;
    tracker_.Begin(*world_, guids_); // the scene as loaded: what a later save counts as destroyed is measured against it
    saves_->SetWorldContext({world_.get(), &guids_, &tracker_, lifecycle_.get()});
}

void Game::WatchSettingsLanguage() {
    settings_->AddObserver([this](const save::GameSettings& now, const save::GameSettings& before) {
        if (now.language != before.language) localization_->SetLanguage(now.language);
        ApplyAudioSettings();
    });
}

void Game::SetUserPaths(const UserPaths& paths) {
    user_paths_ = paths;
    saves_ = std::make_unique<save::SaveSystem>(paths.saves);
    saves_->MakeActive();
    // Keep what the host already loaded or changed when the folder moves.
    const save::GameSettings keep = settings_->Get();
    settings_ = std::make_unique<save::SettingsStore<save::GameSettings>>(paths.settings);
    settings_->Set(keep);
    settings_observer_ = 0;
    WatchSettingsLanguage();
    RefreshSaveContext();
}

std::vector<std::string> Game::LoadSettings(const SettingsTargets& targets) {
    std::vector<std::string> warnings;
    settings_targets_ = targets;
    if (!user_paths_.settings.empty()) {
        const save::SaveResult r = settings_->Load();
        warnings = r.warnings;
    }
    if (settings_observer_ != 0) settings_->RemoveObserver(settings_observer_);
    settings_observer_ = settings_->AddObserver([this](const save::GameSettings& now, const save::GameSettings& before) {
        if (input_loaded_ && reflect::ToJson(now.bindings) != reflect::ToJson(before.bindings)) ActivateInputContexts(); // live rebinding
        for (const std::string& line : ApplySettings(now, settings_targets_, &before)) AETHER_LOG_INFO("Player", "Settings: applied %s", line.c_str());
    });
    for (const std::string& line : ApplySettings(settings_->Get(), settings_targets_)) AETHER_LOG_INFO("Player", "Settings: applied %s", line.c_str());
    if (input_loaded_) ActivateInputContexts(); // the loaded file's rebinds
    localization_->SetLanguage(settings_->Get().language);
    ApplyAudioSettings();
    return warnings;
}

audio::AudioSystem* Game::AudioSystem() { return runtime_ ? runtime_->audio.get() : nullptr; }
anim::AnimationSystem* Game::Animation() { return runtime_ ? runtime_->animation.get() : nullptr; }
vfx::ParticleWorld* Game::Particles() { return runtime_ ? runtime_->particles.get() : nullptr; }

void Game::ApplyAudioSettings() {
    if (!runtime_ || !runtime_->audio) return;
    SettingsTargets targets;
    targets.set_bus_volume_db = [this](const std::string& bus, f32 db) { runtime_->audio->SetBusVolume(bus, db); };
    ApplySettings(settings_->Get(), targets);
}

bool Game::StartAudioOutput(std::string* error) {
    if (!runtime_ || !runtime_->audio) return Fail(error, "Load a scene before starting audio output");
    if (!runtime_->output) {
        runtime_->output = std::make_unique<audio::AudioOutput>(runtime_->mixer, audio::CreateDefaultBackend(runtime_->mixer.SampleRate()));
    }
    return runtime_->output->Start(480, error);
}

std::vector<std::string> Game::StartModules() {
    std::vector<std::string> warnings;
    if (modules_started_) return warnings;
    modules_started_ = true;
    for (const std::string& name : package_.Manifest().modules) {
        std::unique_ptr<plugin::IModule> module = plugin::ModuleRegistry::Get().Create(name);
        if (!module) {
            warnings.push_back("The module '" + name + "' isn't built into this player");
            AETHER_LOG_WARN("Player", "%s", warnings.back().c_str());
            continue;
        }
        module->Startup(plugin::ModuleContext{});
        modules_.push_back({name, std::move(module)});
    }
    return warnings;
}

std::vector<std::string> Game::StartedModules() const {
    std::vector<std::string> names;
    for (const auto& m : modules_) names.push_back(m.first);
    return names;
}

const PrefabData* Game::FindPrefab(const assets::AssetGuid& guid) {
    if (const auto it = prefabs_.find(guid); it != prefabs_.end()) return it->second.get();
    std::unique_ptr<PrefabData>& slot = prefabs_[guid]; // a miss is remembered as null
    const GameManifest::Asset* asset = package_.FindAssetByGuid(assets::ToString(guid));
    if (!asset) return nullptr;
    std::vector<u8> bytes;
    if (!package_.ReadContent(asset->path, bytes)) return nullptr;
    const json j = json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    auto data = std::make_unique<PrefabData>();
    std::string error;
    if (j.is_discarded() || !PrefabFromJson(j, *data, &error)) {
        warnings_.push_back("Prefab " + asset->path + " can't be read" + (error.empty() ? "" : ": " + error));
        return nullptr;
    }
    slot = std::move(data);
    return slot.get();
}

const seq::LevelSequence* Game::FindSequence(const std::string& path) {
    if (const auto it = sequences_.find(path); it != sequences_.end()) return it->second.get();
    std::unique_ptr<seq::LevelSequence>& slot = sequences_[path]; // a miss is remembered as null
    const GameManifest::Asset* asset = package_.FindAsset(path);
    std::vector<u8> bytes;
    if (!asset || !package_.ReadContent(asset->path, bytes)) {
        warnings_.push_back("Sequence " + path + " isn't in the package (cook it: list its folder under always_cook)");
        return nullptr;
    }
    const json j = json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    auto data = std::make_unique<seq::LevelSequence>();
    std::string error;
    if (j.is_discarded() || !seq::SequenceFromJson(j, *data, &error)) {
        warnings_.push_back("Sequence " + path + " can't be read" + (error.empty() ? "" : ": " + error));
        return nullptr;
    }
    slot = std::move(data);
    return slot.get();
}

bool Game::LoadStartupScene(std::string* error) {
    if (!package_.HasManifest() && !package_.LoadManifest(error)) return false;
    if (package_.Manifest().startup_scene.empty()) return Fail(error, "The game has no startup scene");
    return LoadScene(package_.Manifest().startup_scene, error);
}

bool Game::LoadScene(const std::string& path, std::string* error) {
    // The UDP client keeps a pointer to this World. Replacing it mid-session
    // would leave replication writing to freed ECS memory.
    if (authoritative_physics_client_) return Fail(error, "Can't replace a scene while authoritative multiplayer is active");
    StartModules(); // their components, before the scene names them
    bp::RegisterBlueprintComponents(); // BlueprintInstance, before scene deserialization
    ui::RegisterWidgetComponents(); // WidgetComponent (§18.4)
    RegisterRenderComponents(); // lights (DirectionalLight, SkyLight, PointLight, SpotLight) and post-process volumes, so scenes can name them
    RegisterSequenceComponents(); // SequenceComponent (§27.2)
    RegisterAnimationComponents(); // Animator (§16.5, M8.6), before scene deserialization
    RegisterAudioComponents(); // cooked AudioSource/AudioListener/ReverbZone before deserialization
    kits_.RegisterComponents(); // the gameplay kits' components: attributes, tags, items, interactables, quests (§30)
    sprite2d::RegisterSprite2DComponents(); // sprites and tilemaps (§26.6)
    sprite2d::RegisterPhysics2DComponents(); // 2D bodies and colliders
    sprite2d::RegisterPlatformerComponents();
    sprite2d::RegisterLight2DComponents();
#if AETHER_GAME_AI
    RegisterPerceptionComponents(); // AIPerception / AIStimuliSource, for scenes and scripts' entity:Add
#endif
#if AETHER_GAME_PHYSICS
    (void)GetComponentId<CharacterMovement>();
#endif
    std::vector<u8> bytes;
    std::string read_error;
    if (!package_.ReadContent(path, bytes, &read_error)) {
        return Fail(error, "Can't read the scene " + path + (read_error.empty() ? "" : ": " + read_error));
    }
    LoadInputAssets();
    LoadLocalization();
    LoadKitAssets(); // dependency order loads effects before abilities, items and quests
    EndPlay();
    runtime_.reset();
    sequences_.clear(); // players in the old runtime must release their sequence assets first
    physics2d_.reset();
    physics_.reset();
    auto world = std::make_unique<World>();
    // JSON scenes start with '{' (after any whitespace); binary ones with "AESC".
    const auto first = std::find_if(bytes.begin(), bytes.end(), [](u8 c) { return c != ' ' && c != '\n' && c != '\r' && c != '\t'; });
    const bool is_json = first != bytes.end() && *first == '{';
    const bool loaded = is_json ? LoadSceneJsonFromMemory(*world, bytes, path) : LoadSceneFromMemory(*world, bytes, path);
    if (!loaded) return Fail(error, "Can't load the scene " + path);

    if (saves_) saves_->SetWorldContext({}); // it points into the world being replaced
    world_ = std::move(world);
    guids_.Clear();
    guids_.Rebuild(*world_);
    warnings_ = localization_warnings_; // found once, before any scene
    warnings_.insert(warnings_.end(), effect_warnings_.begin(), effect_warnings_.end());
    const auto results = ResolveAllPrefabInstances(*world_, guids_, [this](const assets::AssetGuid& g) { return FindPrefab(g); });
    prefab_instances_ = results.size();
    for (const auto& [root, report] : results) {
        if (!report.ok) {
            warnings_.push_back("A prefab instance in " + path + " couldn't be resolved" +
                                (report.error.empty() ? "" : ": " + report.error));
        }
    }
    guids_.Rebuild(*world_);
    lifecycle_ = std::make_unique<Lifecycle>(*world_, guids_);
    RefreshSaveContext();
    StartRuntime();
    BuildRuntimeStages();
    // 2D physics: the scene's tilemaps are read from the package when first needed.
    tilemaps_.clear();
    tilesets_.clear();
    sprite2d::Resolvers2D tiles;
    const auto read_json = [this](const assets::AssetGuid& guid) -> nlohmann::json {
        const GameManifest::Asset* asset = package_.FindAssetByGuid(assets::ToString(guid));
        std::vector<u8> bytes;
        if (!asset || !package_.ReadContent(asset->path, bytes)) return {};
        return nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    };
    tiles.tilemaps = [this, read_json](const assets::AssetGuid& guid) -> const sprite2d::TilemapData* {
        auto it = tilemaps_.find(guid);
        if (it == tilemaps_.end()) {
            auto map = std::make_unique<sprite2d::TilemapData>();
            const nlohmann::json data = read_json(guid);
            std::string why;
            if (data.is_discarded() || data.is_null() || !sprite2d::TilemapFromJson(data, *map, &why)) map.reset();
            it = tilemaps_.emplace(guid, std::move(map)).first;
        }
        return it->second.get();
    };
    tiles.tilesets = [this, read_json](const assets::AssetGuid& guid) -> const sprite2d::Tileset* {
        auto it = tilesets_.find(guid);
        if (it == tilesets_.end()) {
            auto set = std::make_unique<sprite2d::Tileset>();
            const nlohmann::json data = read_json(guid);
            std::string why;
            if (data.is_discarded() || data.is_null() || !sprite2d::TilesetFromJson(data, *set, &why)) set.reset();
            it = tilesets_.emplace(guid, std::move(set)).first;
        }
        return it->second.get();
    };
    physics2d_ = std::make_unique<sprite2d::Physics2D>(*world_, std::move(tiles));
#if AETHER_GAME_PHYSICS
    physics_ = std::make_unique<Physics>();
    physics_->world.SetGravity(package_.Manifest().gravity);
    ProjectSettings settings;
    settings.layers = package_.Manifest().layers;
    settings.collision_matrix = package_.Manifest().collision_matrix;
    physics_->world.SetCollisionMatrix(MakeCollisionMatrix(settings));
    physics_->scene = std::make_unique<PhysicsScene>(*world_, physics_->world);
    physics_->characters = std::make_unique<CharacterSystem>(*world_, physics_->world, physics_->scene.get());
#endif
    scene_ = path;
    for (const std::string& w : warnings_) AETHER_LOG_WARN("Player", "%s", w.c_str());
    AETHER_LOG_INFO("Player", "Loaded %s: %zu entities, %zu prefab instances", path.c_str(), world_->EntityCount(),
                    prefab_instances_);
    return true;
}

// The cooked input assets: actions registered, and every mapping context
// active (in name order, later ones on top).
void Game::LoadInputAssets() {
    if (input_loaded_) return;
    input_loaded_ = true;
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (asset.importer != "InputAction" && asset.importer != "InputMapping") continue;
        std::vector<u8> bytes;
        std::string error;
        if (!package_.ReadContent(asset.path, bytes, &error)) {
            warnings_.push_back(error);
            continue;
        }
        // A bad file is recorded in input_library_.Errors(), logged just below.
        (void)input_library_.AddFromText(asset.importer, asset.path,
                                   std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    for (const std::string& e : input_library_.Errors()) AETHER_LOG_WARN("Player", "%s", e.c_str());
    input_library_.RegisterActions(input_);
    ActivateInputContexts();
}

std::vector<Game::SequenceCue> Game::SequenceCues() const {
    std::vector<SequenceCue> out;
    if (!runtime_ || !runtime_->audio) return out;
    for (const auto& [key, handle] : runtime_->sequence_audio) {
        if (runtime_->audio->Player().IsPlaying(handle)) out.push_back({key.second, handle});
    }
    return out;
}

void Game::UpdateSubtitle() {
    const seq::SubtitleKey* key = runtime_ && runtime_->sequences ? runtime_->sequences->Subtitle() : nullptr;
    const bool enabled = settings_->Get().subtitles;
    if (key == subtitle_key_ && enabled == subtitle_enabled_ && localization_->Language() == subtitle_language_) return;
    if (key != subtitle_key_) {
        if (key) AETHER_LOG_INFO("Player", "Subtitle shown: %s", key->text.c_str());
        else AETHER_LOG_INFO("Player", "Subtitle hidden");
    }
    subtitle_key_ = key;
    subtitle_enabled_ = enabled;
    subtitle_language_ = localization_->Language();
    if (key && enabled) {
        subtitle_text_ = localization_->Text(key->text, key->text);
        subtitle_speaker_ = key->speaker.empty() ? std::string() : localization_->Text(key->speaker, key->speaker);
    } else {
        subtitle_text_.clear();
        subtitle_speaker_.clear();
    }
}

void Game::LoadLocalization() {
    if (localization_loaded_) return;
    localization_loaded_ = true;
    // A translated asset with no base to fall back to is a mistake worth a line.
    std::vector<std::string> paths;
    for (const GameManifest::Asset& asset : package_.Manifest().assets) paths.push_back(asset.path);
    for (const std::string& orphan : loc::OrphanedVariants(paths)) {
        localization_warnings_.push_back(orphan + " looks like a language variant, but there is no base asset for it");
    }
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (asset.importer != "StringTable") continue;
        std::vector<u8> bytes;
        std::string error;
        if (!package_.ReadContent(asset.path, bytes, &error)) {
            localization_warnings_.push_back(error);
            continue;
        }
        std::vector<std::string> problems;
        localization_->AddFromCsv(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &problems);
        for (const std::string& p : problems) {
            localization_warnings_.push_back(asset.path + ": " + p);
            AETHER_LOG_WARN("Player", "%s: %s", asset.path.c_str(), p.c_str());
        }
    }
}

void Game::ActivateInputContexts() {
    const input::UserBindings* user = settings_ && !settings_->Get().bindings.overrides.empty() ? &settings_->Get().bindings : nullptr;
    i32 priority = 0;
    for (const std::string& name : input_library_.ContextNames()) input_library_.Activate(input_, name, priority++, user);
}

#if AETHER_GAME_AI && AETHER_GAME_PHYSICS
namespace {
// Whether the segment crosses a box, using the same unrotated-box contract as
// the games' hitscan and camera queries.
bool SegmentHitsBox(const Vec3& from, const Vec3& to, const Vec3& center, const Vec3& half) {
    const f32 origin[3] = {from.x, from.y, from.z}, delta[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const f32 mid[3] = {center.x, center.y, center.z}, size[3] = {half.x, half.y, half.z};
    f32 first = 0.0f, last = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        const f32 o = origin[axis], d = delta[axis], low = mid[axis] - size[axis], high = mid[axis] + size[axis];
        if (std::abs(d) < 1e-5f) {
            if (o < low || o > high) return false;
            continue;
        }
        f32 a = (low - o) / d, b = (high - o) / d;
        if (a > b) std::swap(a, b);
        first = std::max(first, a);
        last = std::min(last, b);
        if (first > last) return false;
    }
    return true;
}

template <typename Func>
void ForEachBox(World& world, Func&& func) {
    const ComponentId transform = GetComponentId<Transform>(), collider = GetComponentId<BoxCollider>();
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(transform) || !archetype.Mask().test(collider)) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            for (u32 i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                const Entity e = entities[i];
                func(e, *world.GetComponent<Transform>(e), *world.GetComponent<BoxCollider>(e));
            }
        }
    });
}

// AI.CanSee's walls: every solid box collider where it is now (a gate that
// moves away stops blocking), but the looker's and the target's.
bool BoxLineOfSight(World& world, const Vec3& from, const Vec3& to, Entity looker, Entity target) {
    bool clear = true;
    ForEachBox(world, [&](Entity e, const Transform& t, const BoxCollider& box) {
        if (!clear || e == looker || e == target || box.is_trigger) return;
        if (SegmentHitsBox(from, to, t.position + box.center, box.half_extents)) clear = false;
    });
    return clear;
}

// A scene opts into a navigation mesh by tagging its floors NavWalkable. The
// mesh is baked from every static (no RigidBody) solid box collider - the same
// boxes the character walks on and into - so floors are walked on and walls
// and props walked around; moving bodies (gates, enemies) stay out of it.
std::unique_ptr<nav::NavWorld> BakeSceneNavigation(World& world, std::vector<std::string>& warnings) {
    if (FindEntitiesWithTag(world, "NavWalkable").empty()) return nullptr;
    nav::NavGeometry geometry;
    ForEachBox(world, [&](Entity e, const Transform& t, const BoxCollider& box) {
        if (box.is_trigger || world.GetComponent<RigidBody>(e) != nullptr) return;
        geometry.AddBox(t.position + box.center, box.half_extents);
    });
    auto navigation = std::make_unique<nav::NavWorld>(
        world, [](const ModelRenderer&, std::vector<Vec3>&, std::vector<u32>&) { return false; });
    std::string error;
    if (!navigation->Dynamic().Build(std::move(geometry), nav::NavMeshSettings{}, &error)) {
        warnings.push_back("Navigation: the scene's mesh couldn't be baked" + (error.empty() ? "" : ": " + error));
        return nullptr;
    }
    return navigation;
}
} // namespace
#endif

void Game::StartRuntime() {
    runtime_ = std::make_unique<Runtime>();
#if AETHER_GAME_AI && AETHER_GAME_PHYSICS
    runtime_->navigation = BakeSceneNavigation(*world_, warnings_);
    runtime_->perception = std::make_unique<ai::PerceptionWorld>(*world_);
    runtime_->perception->SetLineOfSight([world = world_.get()](const Vec3& from, const Vec3& to, Entity looker, Entity target) {
        return BoxLineOfSight(*world, from, to, looker, target);
    });
#endif
    runtime_->mixer.AddDefaultBuses();
    for (const auto& asset : package_.Manifest().assets) {
        std::string extension = stdfs::path(asset.path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".wav" && extension != ".ogg" && extension != ".flac" && extension != ".acue") continue;
        auto bytes = std::make_shared<std::vector<u8>>();
        std::string error;
        if (!package_.ReadContent(asset.path, *bytes, &error)) {
            warnings_.push_back("Audio: " + asset.path + ": " + error);
            continue;
        }
        if (extension == ".acue") {
            audio::SoundCue cue;
            if (!audio::LoadCue(std::string(bytes->begin(), bytes->end()), cue, &error)) {
                warnings_.push_back("Audio: " + asset.path + ": " + error);
                continue;
            }
            runtime_->cues.emplace(asset.path, std::move(cue));
        } else if (!runtime_->sounds.AddStreamed(asset.path, bytes, &error)) {
            warnings_.push_back("Audio: " + asset.path + ": " + error);
        }
    }
    runtime_->audio = std::make_unique<audio::AudioSystem>(*world_, runtime_->mixer, runtime_->sounds,
        [this](const std::string& path) -> const audio::SoundCue* {
            const auto it = runtime_->cues.find(path);
            return it == runtime_->cues.end() ? nullptr : &it->second;
        }, &guids_);
    ApplyAudioSettings();
    runtime_->blueprints = std::make_unique<bp::BlueprintSystem>(*world_);
    runtime_->blueprints->VM().SetPrintHandler([](Entity, const std::string& text) { AETHER_LOG_INFO("Blueprint", "%s", text.c_str()); });
    runtime_->blueprints->SetLoader([this](const assets::AssetGuid& guid, bp::Blueprint& out, std::string& name) {
        const GameManifest::Asset* asset = package_.FindAssetByGuid(assets::ToString(guid));
        std::vector<u8> bytes;
        if (!asset || !package_.ReadContent(asset->path, bytes)) return false;
        name = asset->path;
        const json j = json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
        return !j.is_discarded() && bp::BlueprintFromJson(j, out);
    });
    runtime_->blueprints->Register(*lifecycle_);
    // Sequences (§27): the system finds .asequence files by path, and spawns
    // a Spawn key's prefab (a GUID or a path) at its place.
    runtime_->attributes = std::make_unique<gas::AttributeSystem>(*world_); // attributes of this scene's entities
    runtime_->effects = std::make_unique<gas::EffectSystem>(*world_, *runtime_->attributes, effects_);
    runtime_->abilities = std::make_unique<gas::AbilitySystem>(*world_, *runtime_->attributes, *runtime_->effects, abilities_, effects_);
    CreateKitRuntime();
    runtime_->sequences = std::make_unique<seq::SequenceSystem>(
        *world_, guids_, [this](const std::string& path) { return FindSequence(path); }, lifecycle_.get());
    runtime_->sequences->SetSpawner([this](const seq::Track&, Entity parent, const seq::SpawnKey& key) -> Entity {
        assets::AssetGuid guid;
        const GameManifest::Asset* asset = assets::ParseAssetGuid(key.prefab, guid) ? package_.FindAssetByGuid(key.prefab) : package_.FindAsset(key.prefab);
        if (asset && !assets::ParseAssetGuid(asset->guid, guid)) asset = nullptr;
        const PrefabData* data = asset ? FindPrefab(guid) : nullptr;
        if (!data) {
            warnings_.push_back("A sequence spawns '" + key.prefab + "', which isn't a prefab in the package");
            return kNullEntity;
        }
        Transform placed{key.position, key.rotation};
        if (!parent.IsNull()) {
            if (const Transform* base = world_->GetComponent<Transform>(parent)) {
                const Quaternion& q = base->rotation;
                const Vec3 u(q.x, q.y, q.z);
                const Vec3 rotated = key.position + u.Cross(u.Cross(key.position) + key.position * q.w) * 2.0f;
                placed.position = base->position + rotated;
                placed.rotation = base->rotation * key.rotation;
            }
        }
        return InstantiatePrefab(*world_, guids_, guid, *data, &placed);
    });
    runtime_->sequences->MakeActive();
    // Animation (M8.6): assets load from the package by path the first time an Animator names them.
    anim::AnimationLibrary library;
    library.graph = [this](const std::string& path) -> const anim::AnimGraph* {
        auto [it, inserted] = runtime_->anim_graphs.try_emplace(path);
        if (inserted) {
            std::vector<u8> bytes;
            std::string error;
            const json j = package_.ReadContent(path, bytes, &error) ? json::parse(bytes.begin(), bytes.end(), nullptr, false) : json();
            auto graph = std::make_unique<anim::AnimGraph>();
            if (!j.is_discarded() && !j.is_null() && anim::AnimGraphFromJson(j, *graph, &error)) it->second = std::move(graph);
            else warnings_.push_back("Animation: " + path + ": " + (error.empty() ? "not an animation graph" : error));
        }
        return it->second.get();
    };
    library.skeleton = [this](const std::string& path) -> const anim::Skeleton* {
        auto [it, inserted] = runtime_->anim_rigs.try_emplace(path);
        if (inserted) {
            std::vector<u8> bytes;
            std::string error;
            assets::GltfScene scene;
            auto rig = std::make_unique<anim::GltfCharacter>();
            // Rigs embed their buffer (a data: URI), so the in-memory parser opens no other file.
            if (!package_.ReadContent(path, bytes, &error)) warnings_.push_back("Animation: " + path + ": " + error);
            else if (!assets::LoadGltfFromMemory(bytes, scene)) warnings_.push_back("Animation: " + path + " isn't a self-contained glTF (embed its buffer)");
            else if (!anim::BuildCharacterFromGltf(scene, 0, *rig, &error)) warnings_.push_back("Animation: " + path + ": " + error);
            else it->second = std::move(rig);
        }
        return it->second ? &it->second->skeleton : nullptr;
    };
    library.montage = [this](const std::string& path) -> const anim::Montage* {
        auto [it, inserted] = runtime_->anim_montages.try_emplace(path);
        if (inserted) {
            std::vector<u8> bytes;
            std::string error;
            const json j = package_.ReadContent(path, bytes, &error) ? json::parse(bytes.begin(), bytes.end(), nullptr, false) : json();
            auto montage = std::make_unique<anim::Montage>();
            if (!j.is_discarded() && !j.is_null() && anim::MontageFromJson(j, *montage, &error)) it->second = std::move(montage);
            else warnings_.push_back("Animation: " + path + ": " + (error.empty() ? "not a montage" : error));
        }
        return it->second.get();
    };
    // A graph's clip names are the rigs' glTF animation names ("Rig.gltf#Clip" picks one rig's).
    library.assets.clip = [this](const std::string& name) -> const anim::AnimationClip* {
        const usize hash = name.find('#');
        for (const auto& [path, rig] : runtime_->anim_rigs) {
            if (!rig || (hash != std::string::npos && name.compare(0, hash, path) != 0)) continue;
            const std::string clip = hash == std::string::npos ? name : name.substr(hash + 1);
            for (const anim::AnimationClip& c : rig->clips) {
                if (c.name == clip) return &c;
            }
        }
        return nullptr;
    };
    runtime_->animation = std::make_unique<anim::AnimationSystem>(*world_, std::move(library));
    // Particles (M8.7): systems load from the package by path the first time a ParticleSystem names them.
    runtime_->particles = std::make_unique<vfx::ParticleWorld>(*world_, [this](const std::string& path) -> const vfx::ParticleSystemAsset* {
        auto [it, inserted] = runtime_->vfx_assets.try_emplace(path);
        if (inserted) {
            std::vector<u8> bytes;
            std::string error;
            auto asset = std::make_unique<vfx::ParticleSystemAsset>();
            if (package_.ReadContent(path, bytes, &error) && vfx::LoadParticleSystem(std::string(bytes.begin(), bytes.end()), *asset, &error)) it->second = std::move(asset);
            else warnings_.push_back("Particles: " + path + ": " + (error.empty() ? "not a particle system" : error));
        }
        return it->second.get();
    }, &guids_);
    // The on-screen UI: layouts come from the package; a theme asset (.atheme), if the game ships one, styles every layout.
    runtime_->ui_router = std::make_unique<ui::UIInputRouter>(runtime_->ui_viewport, runtime_->ui_font);
    runtime_->ui_bridge = std::make_unique<ui::UIInputBridge>(*runtime_->ui_router);
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (stdfs::path(asset.path).extension() != ".atheme") continue;
        std::vector<u8> bytes;
        std::string error;
        if (package_.ReadContent(asset.path, bytes, &error) && ui::LoadTheme(std::string(bytes.begin(), bytes.end()), runtime_->ui_theme, {}, &error)) {
            runtime_->ui_has_theme = true;
        } else {
            warnings_.push_back("UI: " + asset.path + ": " + error);
        }
        break;
    }
    runtime_->ui = std::make_unique<ui::UISystem>(
        *world_, runtime_->ui_viewport, *runtime_->ui_router,
        [this](const std::string& layout, ui::LayoutDocument& out, std::string* error) {
            std::vector<u8> bytes;
            if (!package_.ReadContent(layout, bytes, error)) return false;
            // Images get ids (their order of appearance, from 1); the renderer maps them back to paths and loads them.
            const ui::TextureResolver textures = [this](const std::string& image) -> u32 {
                std::vector<std::string>& images = runtime_->ui_images;
                const auto found = std::find(images.begin(), images.end(), image);
                if (found != images.end()) return static_cast<u32>(found - images.begin()) + 1;
                images.push_back(image);
                return static_cast<u32>(images.size());
            };
            return ui::LoadLayout(std::string(bytes.begin(), bytes.end()), out, textures, error);
        },
        runtime_->ui_has_theme ? &runtime_->ui_theme : nullptr);
    runtime_->ui->MakeActive();
#if AETHER_GAME_SCRIPTING
    // print() in a script goes to the log (category "Script"), where the console and the MCP server's log_recent can see it.
    runtime_->host.SetPrintHandler([](const std::string& text) { AETHER_LOG_INFO("Script", "%s", text.c_str()); });
    runtime_->scripts = std::make_unique<script::ScriptSystem>(
        runtime_->host, *world_, guids_, [this](const assets::AssetGuid& guid, std::string& source, std::string& name) {
            const GameManifest::Asset* asset = package_.FindAssetByGuid(assets::ToString(guid));
            std::vector<u8> bytes;
            if (!asset || !package_.ReadContent(asset->path, bytes)) return false;
            source.assign(bytes.begin(), bytes.end());
            name = asset->path;
            return true;
        }, false);
    runtime_->scripts->Register(*lifecycle_);
    runtime_->scripts->BindInput(&input_);
#if AETHER_GAME_PHYSICS
    runtime_->scripts->BindPhysicsScene(physics_ ? physics_->scene.get() : nullptr);
    if (physics_ && physics_->scene) {
        physics_->scene->SetEventHandler([this](const PhysicsEvent& event) {
            if (!runtime_ || !runtime_->scripts) return;
            const char* event_name = PhysicsEventName(event.type);
            constexpr char prefix[] = "Event.";
            const std::string full_name = event_name ? event_name : "";
            const std::string method = full_name.rfind(prefix, 0) == 0
                ? full_name.substr(sizeof(prefix) - 1) : full_name;
            if (!method.empty()) {
                runtime_->scripts->SendEvent(event.self, method,
                    {script::EntityRef{event.other}, static_cast<f64>(event.approach_speed)});
            }
        });
    }
#endif
    kit::KitScriptApiContext api_context;
    api_context.install = [this](const std::string& kit_name) { runtime_->scripts->InstallKitApi(kit_name); };
    kits_.InstallScriptApi(&api_context);
#endif
}

ui::Viewport* Game::Ui() { return runtime_ ? &runtime_->ui_viewport : nullptr; }

std::string Game::UiImagePath(u32 id) const {
    if (!runtime_ || id == 0 || id > runtime_->ui_images.size()) return {};
    return runtime_->ui_images[id - 1];
}

std::vector<Game::AiDebugInfo> Game::AiDebug() const {
    std::vector<AiDebugInfo> out;
#if AETHER_GAME_SCRIPTING
    if (!runtime_ || !runtime_->scripts) return out;
    for (script::AiDebugEntry& e : runtime_->scripts->AiDebug().Collect(*world_)) {
        out.push_back({e.entity, std::move(e.state), e.has_perception, e.target, std::move(e.known), std::move(e.path), e.next_corner,
                       e.path_complete, e.goal});
    }
#endif
    return out;
}

std::vector<std::string> Game::ScriptErrors() const {
#if AETHER_GAME_SCRIPTING
    if (runtime_ && runtime_->scripts) return runtime_->scripts->Errors();
#endif
    return {};
}

std::vector<std::string> Game::BlueprintErrors() const {
    std::vector<std::string> errors;
    if (!runtime_ || !runtime_->blueprints) return errors;
    for (const auto& e : runtime_->blueprints->CompileErrors()) {
        for (const auto& d : e.diagnostics) errors.push_back(e.name + ": " + d.code + " " + d.message);
    }
    for (const auto& e : runtime_->blueprints->VM().Errors()) errors.push_back(e.code + ": " + e.message);
    return errors;
}

void Game::BeginPlay() {
    if (IsPlaying()) return;
#if AETHER_GAME_PHYSICS
    if (physics_) physics_->scene->Sync();
#endif
    lifecycle_->BeginPlay();
}

void Game::EndPlay() {
    if (lifecycle_ && lifecycle_->IsPlaying()) lifecycle_->EndPlay();
    if (runtime_) {
        if (runtime_->output) runtime_->output->Stop();
        if (runtime_->audio) runtime_->audio->StopAll();
        runtime_->sequence_audio.clear();
        runtime_->sequence_vfx.clear();
    }
    subtitle_key_ = nullptr; // the sequences it points into go with the runtime
    subtitle_text_.clear();
    subtitle_speaker_.clear();
}


void Game::BuildRuntimeStages() {
    // Attribute changes (§30.2) reach the changed entity's Blueprint as
    // Event.OnAttributeChanged (name, old, new), after gameplay has run.
    // Lasting effects (§30.3) tick before the attributes they change are reported.
    SystemDesc effects = gas::MakeEffectSystem(runtime_ ? runtime_->effects.get() : nullptr, [this](const gas::EffectEvent& e) {
        if (!runtime_) return;
#if AETHER_GAME_SCRIPTING
        // A script hears it as OnEffectApplied / OnEffectRemoved (effect, handle).
        if (runtime_->scripts) runtime_->scripts->SendEvent(e.entity, e.applied ? "OnEffectApplied" : "OnEffectRemoved", {e.effect, static_cast<f64>(e.handle)});
#endif
        if (!runtime_->blueprints) return;
        const bp::VmValue args[] = {e.effect, static_cast<i32>(e.handle)};
        runtime_->blueprints->VM().Dispatch(e.entity, e.applied ? gas::EffectSystem::kAppliedEvent : gas::EffectSystem::kRemovedEvent, args);
    });
    ApplyKitStage(effects, "Player.Effects");
    runtime_->stages[effects.name] = std::move(effects);
    // Abilities (§30.4) run after this frame's effects, so cooldown tags are current when scripts activate.
    SystemDesc abilities = gas::MakeAbilitySystem(runtime_ ? runtime_->abilities.get() : nullptr, [this](const gas::AbilityEvent& e) {
        if (!runtime_) return;
        using Kind = gas::AbilityEvent::Kind;
        const i32 handle = static_cast<i32>(e.handle);
#if AETHER_GAME_SCRIPTING
        if (runtime_->scripts) {
            const f64 h = static_cast<f64>(e.handle);
            switch (e.kind) {
            case Kind::Activated: runtime_->scripts->SendEvent(e.entity, "OnAbilityActivated", {e.ability, h}); break;
            case Kind::Ended:
            case Kind::Cancelled: runtime_->scripts->SendEvent(e.entity, "OnAbilityEnded", {e.ability, h, e.kind == Kind::Cancelled}); break;
            case Kind::Failed: runtime_->scripts->SendEvent(e.entity, "OnAbilityFailed", {e.ability, std::string(gas::FailReasonName(e.reason))}); break;
            }
        }
#endif
        if (!runtime_->blueprints) return;
        switch (e.kind) {
        case Kind::Activated: {
            const bp::VmValue args[] = {e.ability, handle};
            runtime_->blueprints->VM().Dispatch(e.entity, gas::AbilitySystem::kActivatedEvent, args);
            break;
        }
        case Kind::Ended:
        case Kind::Cancelled: {
            const bp::VmValue args[] = {e.ability, handle, e.kind == Kind::Cancelled};
            runtime_->blueprints->VM().Dispatch(e.entity, gas::AbilitySystem::kEndedEvent, args);
            break;
        }
        case Kind::Failed: {
            const bp::VmValue args[] = {e.ability, std::string(gas::FailReasonName(e.reason))};
            runtime_->blueprints->VM().Dispatch(e.entity, gas::AbilitySystem::kFailedEvent, args);
            break;
            }
        }
    });
    ApplyKitStage(abilities, "Player.Abilities");
    runtime_->stages[abilities.name] = std::move(abilities);
    AddInventoryStage();
    AddInteractionStage();
    AddQuestsStage();
    SystemDesc attributes = gas::MakeAttributeSystem(runtime_ ? runtime_->attributes.get() : nullptr, [this](const gas::AttributeEvent& e) {
        if (!runtime_) return;
#if AETHER_GAME_SCRIPTING
        // A script hears it as OnAttributeChanged (name, old, new).
        if (runtime_->scripts) runtime_->scripts->SendEvent(e.entity, "OnAttributeChanged", {e.name, static_cast<f64>(e.old_value), static_cast<f64>(e.new_value)});
#endif
        if (!runtime_->blueprints) return;
        const bp::VmValue args[] = {e.name, e.old_value, e.new_value};
        runtime_->blueprints->VM().Dispatch(e.entity, gas::AttributeSystem::kChangedEvent, args);
    });
    ApplyKitStage(attributes, "Player.Attributes");
    runtime_->stages[attributes.name] = std::move(attributes);
}

void Game::BuildFrame() {
    // The engine's own systems, ahead of the game's in each phase; they
    // reach the current world and lifecycle through `this`, so a new scene
    // needs no rebuild.
    SystemDesc physics;
    physics.name = "Player.Physics";
    physics.phase = SystemPhase::FixedUpdate;
    physics.main_thread_only = true;
    physics.run = [this](World&, const FrameContext& frame) {
#if AETHER_GAME_PHYSICS
        if (physics_ && !authoritative_physics_client_) {
            physics_->characters->Step(frame.fixed_dt);
            physics_->scene->Step(frame.fixed_dt);
        }
#else
        (void)this; // no physics module: the step does nothing
        (void)frame;
#endif
    };
    scheduler_.Add(std::move(physics));
    // 2D: the platformer controllers read the player's input, then the bodies step.
    SystemDesc physics2d;
    physics2d.name = "Player.Physics2D";
    physics2d.phase = SystemPhase::FixedUpdate;
    physics2d.after = {"Player.Physics"};
    physics2d.main_thread_only = true;
    physics2d.run = [this](World& world, const FrameContext& frame) {
        if (!physics2d_) return;
        const f32 move = input_.GetAxis2D("Move").x;
        const bool jump = input_.GetAction("Jump").phase == input::ActionPhase::Triggered;
        world.ForEach<sprite2d::PlatformerController2D>([&](sprite2d::PlatformerController2D& pc) {
            pc.input_move = move;
            pc.input_jump = jump;
        });
        sprite2d::UpdatePlatformers(world, *physics2d_, frame.fixed_dt);
        physics2d_->Step(frame.fixed_dt);
        sprite2d::UpdateCameraFollow2D(world, frame.fixed_dt);
    };
    scheduler_.Add(std::move(physics2d));
    SystemDesc fixed;
    fixed.name = "Player.FixedUpdate";
    fixed.phase = SystemPhase::FixedUpdate;
    fixed.after = {"Player.Physics", "Player.Physics2D"};
    fixed.main_thread_only = true;
    fixed.run = [this](World&, const FrameContext& frame) { lifecycle_->FixedUpdate(frame.fixed_dt); };
    scheduler_.Add(std::move(fixed));
    SystemDesc update;
    update.name = "Player.Update";
    update.phase = SystemPhase::Update;
    update.main_thread_only = true;
    update.run = [this](World&, const FrameContext& frame) {
        // Menus: the keys and the pad move focus and press the focused control (only when the game has UI on screen, so a game
        // without one never sees its input used here); scripts read the clicks next frame through UI.WasClicked.
        if (runtime_ && runtime_->ui_bridge && runtime_->ui_viewport.LayerCount() > 0) runtime_->ui_bridge->Update(input_state_, frame.dt);
#if AETHER_GAME_AI
        // Senses before scripts decide, so AI.GetKnown / AI.GetTarget are this frame's. The runtime's own
        // perception world (physics builds), else the active one a host supplied (a test, a tool).
        if (runtime_) {
            ai::PerceptionWorld* perception = runtime_->perception ? runtime_->perception.get() : ai::PerceptionWorld::Active();
            if (perception) perception->Update(frame.dt);
        }
#endif
        lifecycle_->Update(frame.dt);
        if (runtime_ && runtime_->ui) runtime_->ui->Update(frame.dt); // after scripts, so what they set shows this frame
    };
    scheduler_.Add(std::move(update));
    // Level sequences (§27) animate after gameplay's Update and before
    // scripts and Blueprints see their events.
    SystemDesc sequencer;
    sequencer.name = "Player.Sequencer";
    sequencer.phase = SystemPhase::Update;
    sequencer.after = {"Player.Update"};
    sequencer.main_thread_only = true;
    sequencer.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->sequences) return;
        runtime_->sequences->Update(frame.dt);
        UpdateSubtitle();
        for (const seq::SequenceEvent& e : runtime_->sequences->Events()) {
            if (e.kind == seq::SequenceEvent::Kind::Finished) {
                AETHER_LOG_INFO("Player", "Sequence %s: %s", e.skipped ? "skipped" : "finished", e.name.c_str());
            }
        }
        for (const seq::SequenceEvent& e : runtime_->sequences->Events()) {
            // Audio keys play without Blueprints; markers and completion are Blueprint events.
            if (e.kind == seq::SequenceEvent::Kind::Marker) {
                if (!runtime_->blueprints) continue;
                const bp::VmValue args[] = {e.name, e.payload};
                runtime_->blueprints->VM().Dispatch(e.entity, seq::SequenceSystem::kMarkerEvent, args);
            } else if (e.kind == seq::SequenceEvent::Kind::Finished) {
                if (!runtime_->blueprints) continue;
                const bp::VmValue args[] = {e.name};
                runtime_->blueprints->VM().Dispatch(e.entity, seq::SequenceSystem::kFinishedEvent, args);
            }
            else if (e.kind == seq::SequenceEvent::Kind::Audio && runtime_->audio) {
                if (!e.subject.IsNull() && world_->IsAlive(e.subject)) {
                    AudioSource* source = world_->GetComponent<AudioSource>(e.subject);
                    if (!source) { world_->AddComponent(e.subject, AudioSource{}); source = world_->GetComponent<AudioSource>(e.subject); source->auto_play = false; }
                    if (e.payload == "play" || e.payload == "fade_in") {
                        if (source->cue != e.name) source->SetCue(e.name);
                        source->SetVolume(e.value);
                        if (e.payload == "play") source->Play();
                        else source->FadeIn(e.fade);
                    }
                    else if (e.payload == "fade_out") source->FadeOut(e.fade);
                    else if (e.payload == "stop") source->Stop();
                } else {
                    const auto key = std::make_pair(e.entity.index, e.name);
                    auto& handle = runtime_->sequence_audio[key];
                    if (e.payload == "stop" || e.payload == "fade_out") {
                        if (runtime_->audio->Player().Stop(handle, e.payload == "fade_out" ? e.fade : 0.0f)) {
                            AETHER_LOG_INFO("Player", "Sequence cue stopped: %s", e.name.c_str());
                        }
                    } else {
                        const auto cue = runtime_->cues.find(e.name);
                        if (cue != runtime_->cues.end()) {
                            runtime_->audio->Player().Stop(handle);
                            audio::CuePlayParams params;
                            params.force_2d = true;
                            params.volume_db = e.value;
                            params.fade_in = e.payload == "fade_in" ? e.fade : 0.0f;
                            handle = runtime_->audio->Player().Play(cue->second, params);
                            AETHER_LOG_INFO("Player", "Sequence cue started: %s", e.name.c_str());
                        } else warnings_.push_back("Sequence audio cue missing: " + e.name);
                    }
                }
            }
            else if (e.kind == seq::SequenceEvent::Kind::Animation) {
                // Montages on the track's entity, through its Animator (applied by Player.Animation this frame).
                Animator* animator = !e.subject.IsNull() && world_->IsAlive(e.subject) ? world_->GetComponent<Animator>(e.subject) : nullptr;
                if (!animator) {
                    warnings_.push_back("Sequence animation '" + e.name + "' is on an entity without an Animator");
                } else if (e.payload == "stop") {
                    animator->StopMontage();
                } else {
                    animator->PlayMontage(e.name, e.value > 0.0f ? e.value : 1.0f);
                }
            }
            else if (e.kind == seq::SequenceEvent::Kind::Vfx && runtime_->particles) {
                // Play spawns the effect on the track's entity (at its offset, following it) or at the offset in the
                // world; Stop stops it spawning. Player.Particles steps it this frame, so it shows on the key's frame.
                const auto key = std::make_pair(e.entity.index, e.name);
                if (e.payload == "stop") {
                    const auto it = runtime_->sequence_vfx.find(key);
                    if (it == runtime_->sequence_vfx.end()) continue;
                    if (ParticleSystem* ps = world_->IsAlive(it->second) ? world_->GetComponent<ParticleSystem>(it->second) : nullptr) ps->Deactivate();
                    runtime_->sequence_vfx.erase(it);
                    AETHER_LOG_INFO("Player", "Sequence vfx stopped: %s", e.name.c_str());
                } else {
                    if (const auto it = runtime_->sequence_vfx.find(key); it != runtime_->sequence_vfx.end() && world_->IsAlive(it->second)) {
                        if (ParticleSystem* ps = world_->GetComponent<ParticleSystem>(it->second)) ps->Deactivate(); // a replay: the old one lives out
                    }
                    const bool attached = !e.subject.IsNull() && world_->IsAlive(e.subject);
                    const Entity fx = attached ? runtime_->particles->SpawnAttached(e.name, e.subject, e.offset) : runtime_->particles->SpawnAtLocation(e.name, e.offset);
                    if (fx.IsNull()) {
                        warnings_.push_back("Sequence vfx '" + e.name + "' didn't start");
                        continue;
                    }
                    runtime_->sequence_vfx[key] = fx;
                    AETHER_LOG_INFO("Player", "Sequence vfx started: %s", e.name.c_str());
                }
            }
        }
    };
    scheduler_.Add(std::move(sequencer));
    // Characters animate after scripts set their parameters and sequences start their montages.
    SystemDesc animation;
    animation.name = "Player.Animation";
    animation.phase = SystemPhase::Update;
    animation.after = {"Player.Sequencer"};
    animation.main_thread_only = true;
    animation.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->animation) return;
        anim::AnimationSystem& system = *runtime_->animation;
        system.Update(frame.dt);
        for (const anim::EntityAnimEvent& e : system.Events()) {
            const anim::AnimEvent& ev = e.event;
            if (ev.type != anim::AnimEventType::StateChanged && ev.type != anim::AnimEventType::MontageStarted &&
                ev.type != anim::AnimEventType::MontageEnded) continue;
            std::string who = std::to_string(e.entity.index);
            if (const Tags* tags = world_->GetComponent<Tags>(e.entity); tags && !tags->names.empty()) who = tags->names.front();
            if (ev.type == anim::AnimEventType::StateChanged) {
                AETHER_LOG_INFO("Player", "Anim state: %s %s: %s -> %s", who.c_str(), ev.machine.c_str(), ev.detail.c_str(), ev.name.c_str());
            } else if (ev.type == anim::AnimEventType::MontageStarted) {
                AETHER_LOG_INFO("Player", "Anim montage started: %s %s", who.c_str(), ev.name.c_str());
            } else {
                AETHER_LOG_INFO("Player", "Anim montage ended: %s %s%s", who.c_str(), ev.name.c_str(), ev.interrupted ? " (interrupted)" : "");
            }
            if (runtime_->blueprints) {
                // Animation events reach the entity's Blueprints like the editor's preview sends them.
                const char* event = anim::AnimationSystem::BlueprintEventName(ev.type);
                if (ev.type == anim::AnimEventType::StateChanged) {
                    const bp::VmValue args[] = {ev.machine, ev.detail, ev.name};
                    runtime_->blueprints->VM().Dispatch(e.entity, event, args);
                } else if (ev.type == anim::AnimEventType::MontageStarted) {
                    const bp::VmValue args[] = {ev.name};
                    runtime_->blueprints->VM().Dispatch(e.entity, event, args);
                } else {
                    const bp::VmValue args[] = {ev.name, ev.interrupted};
                    runtime_->blueprints->VM().Dispatch(e.entity, event, args);
                }
            }
        }
        for (; runtime_->animation_problems < system.Problems().size(); ++runtime_->animation_problems) {
            const std::string& problem = system.Problems()[runtime_->animation_problems];
            AETHER_LOG_WARN("Player", "Animation: %s", problem.c_str());
            warnings_.push_back("Animation: " + problem);
        }
    };
    scheduler_.Add(std::move(animation));
    // Particles step after characters animate and sequences start or stop their effects.
    SystemDesc particles;
    particles.name = "Player.Particles";
    particles.phase = SystemPhase::Update;
    particles.after = {"Player.Animation"};
    particles.main_thread_only = true;
    particles.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->particles) return;
        vfx::ParticleWorld& system = *runtime_->particles;
        system.Update(frame.dt);
        for (const vfx::ParticleSystemFinishedEvent& e : system.Events()) {
            AETHER_LOG_INFO("Player", "Vfx finished: %s", e.asset.c_str());
            if (runtime_->blueprints) {
                const bp::VmValue args[] = {e.asset};
                runtime_->blueprints->VM().Dispatch(e.entity, vfx::ParticleWorld::kFinishedEvent, args);
            }
        }
        for (; runtime_->particle_problems < system.Problems().size(); ++runtime_->particle_problems) {
            const std::string& problem = system.Problems()[runtime_->particle_problems];
            AETHER_LOG_WARN("Player", "Particles: %s", problem.c_str());
            warnings_.push_back("Particles: " + problem);
        }
    };
    scheduler_.Add(std::move(particles));
    SystemDesc audio;
    audio.name = "Player.Audio";
    audio.phase = SystemPhase::Update;
    audio.after = {"Player.Sequencer"};
    audio.main_thread_only = true;
    audio.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->audio) return;
        runtime_->audio->Update(frame.dt);
        std::erase_if(runtime_->sequence_audio, [this](const auto& item) { return !runtime_->audio->Player().IsPlaying(item.second); });
        if (runtime_->blueprints) {
            for (const auto& event : runtime_->audio->Events()) {
                const bp::VmValue args[] = {event.cue};
                runtime_->blueprints->VM().Dispatch(event.entity, audio::AudioSystem::kFinishedEvent, args);
            }
        }
        if ((!runtime_->output || !runtime_->output->Running()) &&
            (runtime_->mixer.VoiceCount() > 0 || runtime_->audio->Player().ActiveCount() > 0)) {
            const u32 frames = static_cast<u32>(std::clamp(frame.dt, 0.0f, 0.25f) * runtime_->mixer.SampleRate());
            if (frames > 0) {
                std::vector<f32> samples(static_cast<usize>(frames) * 2);
                runtime_->mixer.Render(samples.data(), frames);
            }
        }
    };
    scheduler_.Add(std::move(audio));
    // Finished saves (§28.7): queued writes are delivered here, on the main
    // thread, then Blueprints hear Event.OnSaveFinished (slot, success).
    // Luau's callbacks run from the same Pump.
    SystemDesc save_system;
    save_system.name = "Player.Save";
    save_system.phase = SystemPhase::Update;
    save_system.after = {"Player.Sequencer"};
    save_system.main_thread_only = true;
    save_system.run = [this](World&, const FrameContext&) {
        if (!saves_) return;
        saves_->Pump();
        for (const save::SaveSystem::FinishedSave& f : saves_->TakeFinishedSaves()) {
            if (!runtime_ || !runtime_->blueprints) continue;
            const bp::VmValue args[] = {f.slot, f.success};
            runtime_->blueprints->VM().DispatchAll("Event.OnSaveFinished", args);
        }
    };
    scheduler_.Add(std::move(save_system));
    // Kit factories bind scene-owned systems. Keep their descriptors in the
    // runtime and have the persistent scheduler resolve the current scene.
    if (runtime_) for (const auto* name : {"Player.Effects", "Player.Abilities", "Player.Inventory",
                                         "Player.Interaction", "Player.Quests", "Player.Attributes"}) {
        const auto found = runtime_->stages.find(name);
        if (found == runtime_->stages.end()) continue;
        SystemDesc stage = found->second;
        stage.run = [this, name](World& world, const FrameContext& frame) {
            if (!runtime_) return;
            const auto current = runtime_->stages.find(name);
            if (current != runtime_->stages.end()) current->second.run(world, frame);
        };
        scheduler_.Add(std::move(stage));
    }
    // Timers and Blueprint ticks run with the frame's Update.
    SystemDesc scripting;
    scripting.name = "Player.Scripting";
    scripting.phase = SystemPhase::Update;
    scripting.after = {"Player.Sequencer", "Player.Save"};
    for (const kit::KitStage& stage : kits_.Stages()) scripting.after.push_back(stage.name); // scripts see what the kits queued
    scripting.main_thread_only = true;
    scripting.run = [this](World&, const FrameContext& frame) {
#if AETHER_GAME_SCRIPTING
        if (runtime_ && runtime_->scripts) runtime_->scripts->Tick(frame.dt);
#endif
        if (runtime_ && runtime_->blueprints) runtime_->blueprints->Update(frame.dt);
    };
    scheduler_.Add(std::move(scripting));
    SystemDesc late;
    late.name = "Player.LateUpdate";
    late.phase = SystemPhase::LateUpdate;
    late.main_thread_only = true;
    late.run = [this](World&, const FrameContext& frame) { lifecycle_->LateUpdate(frame.dt); };
    scheduler_.Add(std::move(late));
    loop_ = std::make_unique<FrameLoop>(scheduler_, FixedTimestep(1.0f / package_.Manifest().fixed_timestep_hz));
}

void Game::UpdateUiPointers(f32 dt) {
    if (!runtime_ || !runtime_->ui_router || runtime_->ui_viewport.LayerCount() == 0) return;
    ui::UIInputRouter& router = *runtime_->ui_router;
    // A windowed player lays the UI out as it draws; without a renderer (tests, the MCP server) nobody has, and a layer nobody has laid out has no
    // rectangles to hit. Lay it out here, once per layer, with the built-in font (its size, 1920 x 1080 unless a renderer set one, is the pixel space
    // pointers are in).
    for (usize layer = 0; layer < runtime_->ui_viewport.LayerCount(); ++layer) {
        const ui::Widget* root = runtime_->ui_viewport.Layer(layer);
        if (root != nullptr && root->Geometry().Empty()) {
            runtime_->ui_viewport.Layout(runtime_->ui_font);
            break;
        }
    }
    for (usize i = 0; i < input::InputState::kMaxPointers; ++i) {
        const input::InputState::Pointer now = input_state_.GetPointer(i);
        const input::InputState::Pointer before = ui_pointers_[i];
        const ui::Vec2 at{now.x * ui_pointer_scale_x_, now.y * ui_pointer_scale_y_};
        const u32 id = static_cast<u32>(i);
        if (now.active && (!before.active || now.x != before.x || now.y != before.y)) router.PointerMove(at, id);
        if (now.active && now.down && !before.down) ui_pointer_used_[i] = router.PointerDown(at, 0, id);
        if (!now.down && before.down) router.PointerUp(at, 0, id);
        // The mouse and the first finger are also Key::MouseLeft: gameplay must not see a press the UI took.
        if (ui_pointer_used_[i] && now.down && i <= 1) input_state_.SetButton(input::Key::MouseLeft, false);
        if (!now.down) ui_pointer_used_[i] = false;
        ui_pointers_[i] = now;
    }
    router.Tick(dt);
}

void Game::ApplyVirtualControls() {
    if (!runtime_ || runtime_->ui_viewport.LayerCount() == 0) {
        ui_held_.clear();
        return;
    }
    std::vector<HeldControl> now;
    const auto visit = [&](auto&& self, ui::Widget& widget) -> void {
        if (auto* stick = dynamic_cast<ui::VirtualStick*>(&widget)) {
            if (stick->IsActive()) {
                const input::Key x = input::KeyFromName(stick->key_x), y = input::KeyFromName(stick->key_y);
                if (x != input::Key::None) input_state_.SetAxis(x, stick->value.x);
                if (y != input::Key::None) input_state_.SetAxis(y, stick->value.y);
                now.push_back({stick, x, y, true});
            }
        } else if (auto* button = dynamic_cast<ui::Button*>(&widget); button != nullptr && !button->hold_key.empty() && button->IsPressed()) {
            const input::Key key = input::KeyFromName(button->hold_key);
            if (key != input::Key::None) input_state_.SetButton(key, true);
            now.push_back({button, key, input::Key::None, false});
        }
        for (usize c = 0; c < widget.ChildCount(); ++c) self(self, *widget.Child(c));
    };
    for (usize layer = 0; layer < runtime_->ui_viewport.LayerCount(); ++layer) {
        if (ui::Widget* root = runtime_->ui_viewport.Layer(layer)) visit(visit, *root);
    }
    // What was held last Tick and is not now (the finger lifted, or the widget went away) is released.
    for (const HeldControl& before : ui_held_) {
        const bool still = std::any_of(now.begin(), now.end(), [&](const HeldControl& h) { return h.widget == before.widget; });
        if (still) continue;
        if (before.stick) {
            if (before.key_a != input::Key::None) input_state_.SetAxis(before.key_a, 0.0f);
            if (before.key_b != input::Key::None) input_state_.SetAxis(before.key_b, 0.0f);
        } else if (before.key_a != input::Key::None) {
            input_state_.SetButton(before.key_a, false);
        }
    }
    ui_held_ = std::move(now);
}

void Game::OnPlatformEvent(const platform::LifecycleMessage& message) {
    switch (message.event) {
    case platform::LifecycleEvent::Suspend:
        // In the background: no sound, nothing half-written. (PlatformLifecycle ignores a second Suspend, so this runs once.)
        audio_was_running_ = runtime_ && runtime_->output && runtime_->output->Running();
        if (audio_was_running_) runtime_->output->Stop();
        if (saves_) {
            saves_->Flush();
            saves_->Pump();
        }
        break;
    case platform::LifecycleEvent::Resume:
        skip_next_dt_ = true;
        if (audio_was_running_ && runtime_ && runtime_->output) {
            std::string error;
            if (!runtime_->output->Start(480, &error)) AETHER_LOG_WARN("Player", "Audio did not restart after resume: %s", error.c_str());
        }
        audio_was_running_ = false;
        break;
    case platform::LifecycleEvent::MemoryPressure:
        ++memory_pressure_events_;
        if (message.level == platform::MemoryLevel::Critical) ++critical_memory_pressure_events_;
        break;
    }
}

FrameContext Game::Tick(f32 dt) {
    if (!loop_) BuildFrame();
    if (platform_lifecycle_->Suspended()) {
        // In the background the game stands still: no time passes and input is left alone.
        FrameContext frame = last_frame_;
        frame.dt = 0;
        return frame;
    }
    if (skip_next_dt_) {
        dt = 0.0f; // the time spent suspended is not game time
        skip_next_dt_ = false;
    }
    UpdateProofMenu();
    UpdateUiPointers(dt);
    ApplyVirtualControls();
    input_.Update(input_state_, dt); // the host's keys and mouse, as this frame's actions
    if (paused_) {
        input_state_.EndFrame();
        FrameContext frame = last_frame_;
        frame.dt = 0;
        return frame;
    }
    const FrameContext frame = loop_->Tick(*world_, dt);
    input_state_.EndFrame();         // movement deltas are per frame; held keys stay
    if (proof_load_input_) { input_state_.SetButton(input::Key::F5, false); proof_load_input_ = false; }
    if (proof_new_input_) { input_state_.SetButton(input::Key::N, false); proof_new_input_ = false; }
    last_frame_ = frame;
    stats_.frames = frame.frame + 1;
    stats_.fixed_steps = frame.fixed_step;
    stats_.time = frame.time;
    return frame;
}

void Game::UpdateProofMenu() {
    if (package_.Manifest().project != "AETHER-01") return;
    using input::Key;
    const auto pressed = [&](Key key) { return input_state_.IsDown(key) && !menu_previous_.IsDown(key); };
    const bool back = pressed(Key::Escape) || pressed(Key::GamepadStart) || (proof_menu_ != 0 && pressed(Key::GamepadB));
    const bool accept = pressed(Key::Enter) || pressed(Key::GamepadA);
    const bool up = pressed(Key::Up) || pressed(Key::GamepadDPadUp);
    const bool down = pressed(Key::Down) || pressed(Key::GamepadDPadDown);
    const bool left = pressed(Key::Left) || pressed(Key::GamepadDPadLeft);
    const bool right = pressed(Key::Right) || pressed(Key::GamepadDPadRight);
    menu_previous_ = input_state_;
    const i32 previous_menu = proof_menu_;
    const std::vector<RebindableInput> bindings = RebindableInputs();
    if (proof_key_capture_active_) {
        if (pressed(Key::Escape) || pressed(Key::GamepadB) || pressed(Key::GamepadStart)) {
            proof_key_capture_.Cancel();
            proof_key_capture_active_ = false;
            proof_settings_status_ = 0;
        } else {
            Key captured = Key::None;
            const input::KeyCapture::Result result = proof_key_capture_.Update(input_state_, captured);
            if (result == input::KeyCapture::Result::Captured) {
                proof_key_capture_active_ = false;
                save::GameSettings next = settings_->Get();
                next.bindings.Set(proof_capture_context_, proof_capture_action_, proof_capture_slot_, captured);
                settings_->Set(next);
                const bool saved = settings_->Save().ok;
                const input::InputMappingContext* context = input_library_.FindContext(proof_capture_context_);
                const bool has_conflict = context != nullptr &&
                    !input::FindConflicts(input::ApplyUserBindings(*context, settings_->Get().bindings), captured,
                                          proof_capture_action_, proof_capture_slot_).empty();
                proof_settings_status_ = saved ? (has_conflict ? 2 : 1) : -1;
            } else if (result == input::KeyCapture::Result::Canceled) {
                proof_key_capture_active_ = false;
                proof_settings_status_ = 0;
            }
        }
    } else if (back) {
        proof_menu_ = proof_menu_ == 0 ? 1 : proof_menu_ == 1 ? 0 : 1;
        proof_menu_row_ = 0;
        proof_settings_status_ = 0;
    } else if (proof_menu_ == 1) {
        proof_menu_row_ = (proof_menu_row_ + (down ? 1 : up ? 5 : 0)) % 6;
        if (accept) switch (proof_menu_row_) {
        case 0: proof_menu_ = 0; break;
        case 1: proof_menu_ = 2; proof_menu_row_ = 0; break;
        case 2: proof_menu_ = 3; proof_menu_row_ = 0; break;
        case 3:
            if (saves_ && saves_->Exists(scene_.find("Campaign") != std::string::npos ? "aether-campaign" : "first-contact")) {
                input_state_.SetButton(Key::F5, true); proof_load_input_ = true; proof_menu_ = 0;
            } else proof_settings_status_ = -2;
            break;
        case 4: input_state_.SetButton(Key::N, true); proof_new_input_ = true; proof_menu_ = 0; break;
        case 5: exit_requested_ = true; break;
        }
    } else if (proof_menu_ == 2) {
        proof_menu_row_ = (proof_menu_row_ + (down ? 1 : up ? 4 : 0)) % 5;
        if (proof_menu_row_ == 4 && accept) { proof_menu_ = 1; proof_menu_row_ = 0; }
        else if (proof_menu_row_ < 4 && (left || right || accept)) {
            save::GameSettings next = settings_->Get();
            f32* volumes[] = {&next.master, &next.music, &next.sfx, &next.voice};
            *volumes[proof_menu_row_] = std::clamp(*volumes[proof_menu_row_] + (left ? -0.1f : 0.1f), 0.0f, 1.0f);
            settings_->Set(next);
            proof_settings_status_ = settings_->Save().ok ? 1 : -1;
        }
    } else if (proof_menu_ == 3) {
        const i32 binding_count = static_cast<i32>(bindings.size());
        const i32 reset_all_row = binding_count;
        const i32 back_row = binding_count + 1;
        const i32 row_count = binding_count + 2;
        if (row_count > 0) proof_menu_row_ = (proof_menu_row_ + (down ? 1 : up ? row_count - 1 : 0)) % row_count;
        if (pressed(Key::Delete) && proof_menu_row_ < binding_count) {
            const RebindableInput& binding = bindings[static_cast<usize>(proof_menu_row_)];
            save::GameSettings next = settings_->Get();
            const bool changed = next.bindings.Reset(binding.context, binding.action, binding.slot);
            if (changed) {
                settings_->Set(next);
                proof_settings_status_ = settings_->Save().ok ? 1 : -1;
            } else {
                proof_settings_status_ = 0;
            }
        }
        if (accept) {
            if (proof_menu_row_ < binding_count) {
                const RebindableInput& binding = bindings[static_cast<usize>(proof_menu_row_)];
                proof_capture_context_ = binding.context;
                proof_capture_action_ = binding.action;
                proof_capture_slot_ = binding.slot;
                proof_key_capture_.Begin(input_state_);
                proof_key_capture_active_ = true;
                proof_settings_status_ = 3;
            } else if (proof_menu_row_ == reset_all_row) {
                save::GameSettings next = settings_->Get();
                if (!next.bindings.overrides.empty()) {
                    next.bindings.ResetAll();
                    settings_->Set(next);
                    proof_settings_status_ = settings_->Save().ok ? 1 : 0;
                } else {
                    proof_settings_status_ = 0;
                }
            } else if (proof_menu_row_ == back_row) {
                proof_menu_ = 1;
                proof_menu_row_ = 0;
                proof_settings_status_ = 0;
            }
        }
    }
    if (previous_menu != proof_menu_ && paused_ != (proof_menu_ != 0)) {
        paused_ = proof_menu_ != 0;
        // Pausing holds the world (and its sequences) still; device output stops too so
        // dialogue doesn't run on ahead of its subtitles. Headless audio only renders on Tick.
        if (paused_ && runtime_ && runtime_->output && runtime_->output->Running()) {
            runtime_->output->Stop();
            pause_stopped_audio_ = true;
        } else if (!paused_ && pause_stopped_audio_) {
            pause_stopped_audio_ = false;
            std::string error;
            if (runtime_ && runtime_->output && !runtime_->output->Start(480, &error)) AETHER_LOG_WARN("Player", "Audio did not restart after the pause menu: %s", error.c_str());
        }
    }
    if (previous_menu != 0 && proof_menu_ == 0 && !proof_load_input_ && !proof_new_input_) {
        // Consume the menu confirmation/back edge before gameplay reads it.
        // The same gamepad buttons also bind jump and interaction.
        input_.Update(input_state_, 0);
    }
    const auto players = FindEntitiesWithTag(*world_, "Player");
    if (players.empty()) return;
    auto* attributes = world_->GetComponent<gas::AttributeSet>(players.front());
    if (!attributes) return;
    attributes->Define("MenuState", 0, 0, 3);
    attributes->Define("MenuRow", 0, 0, 5);
    attributes->Define("SettingsStatus", 0, -2, 3);
    attributes->SetBase("MenuState", static_cast<f32>(proof_menu_));
    attributes->SetBase("MenuRow", static_cast<f32>(proof_menu_row_));
    attributes->SetBase("SettingsStatus", static_cast<f32>(proof_settings_status_));
}

std::vector<Game::RebindableInput> Game::RebindableInputs() const {
    std::vector<RebindableInput> out;
    if (!settings_) return out;
    const input::UserBindings& user = settings_->Get().bindings;
    for (const std::string& name : input_library_.ContextNames()) {
        const input::InputMappingContext* context = input_library_.FindContext(name);
        if (!context) continue;
        const input::InputMappingContext current = input::ApplyUserBindings(*context, user);
        std::unordered_map<std::string, u32> slots;
        for (usize i = 0; i < context->bindings.size(); ++i) {
            const input::InputBinding& binding = context->bindings[i];
            const u32 slot = slots[binding.action]++;
            out.push_back({name, binding.action, slot, binding.key, current.bindings[i].key});
        }
    }
    return out;
}

GameStats Game::Stats() const {
    GameStats s = stats_;
    s.entities = world_->EntityCount();
    s.prefab_instances = prefab_instances_;
#if AETHER_GAME_PHYSICS
    if (physics_) s.physics_bodies = physics_->scene->BodyCount();
#endif
    if (runtime_) {
        if (runtime_->blueprints) s.blueprint_instances = runtime_->blueprints->VM().InstanceCount();
#if AETHER_GAME_SCRIPTING
        if (runtime_->scripts) s.script_instances = runtime_->scripts->InstanceCount();
#endif
    }
    s.script_errors = ScriptErrors().size();
    s.blueprint_errors = BlueprintErrors().size();
    return s;
}

} // namespace aether::player
