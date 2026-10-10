#include "aether/player/game.h"
#include "game_runtime.h"
#include "aether/core/json_util.h"
#include "aether/audio/backend.h"
#include "aether/audio/audio_system.h"

#include "aether/sequencer/sequence_system.h"

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
        if (d.is_discarded() || !d.is_object() || d.value("$type", "") != "DlcManifest") {
            return Fail(error, path + " isn't a DLC manifest");
        }
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

Game::Game(GamePackage& package)
    : package_(package), world_(std::make_unique<World>()), settings_(std::make_unique<save::SettingsStore<save::GameSettings>>(std::filesystem::path())) {
    lifecycle_ = std::make_unique<Lifecycle>(*world_, guids_);
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
#endif
}

Game::~Game() {
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
    StartModules(); // their components, before the scene names them
    bp::RegisterBlueprintComponents(); // BlueprintInstance, before scene deserialization
    RegisterSequenceComponents(); // SequenceComponent (§27.2)
    RegisterAudioComponents(); // cooked AudioSource/AudioListener/ReverbZone before deserialization
    kits_.RegisterComponents(); // the gameplay kits' components: attributes, tags, items, interactables, quests (§30)
    sprite2d::RegisterSprite2DComponents(); // sprites and tilemaps (§26.6)
    sprite2d::RegisterPhysics2DComponents(); // 2D bodies and colliders
    sprite2d::RegisterPlatformerComponents();
    sprite2d::RegisterLight2DComponents();
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
        input_library_.AddFromText(asset.importer, asset.path,
                                   std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    for (const std::string& e : input_library_.Errors()) AETHER_LOG_WARN("Player", "%s", e.c_str());
    input_library_.RegisterActions(input_);
    ActivateInputContexts();
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

void Game::StartRuntime() {
    runtime_ = std::make_unique<Runtime>();
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
    CreateInventoryRuntime();
    runtime_->abilities = std::make_unique<gas::AbilitySystem>(*world_, *runtime_->attributes, *runtime_->effects, abilities_, effects_);
    CreateQuestsRuntime();
    CreateInteractionRuntime();
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
#if AETHER_GAME_SCRIPTING
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
    }
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
        if (physics_) {
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
    update.run = [this](World&, const FrameContext& frame) { lifecycle_->Update(frame.dt); };
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
        if (!runtime_->blueprints) return;
        for (const seq::SequenceEvent& e : runtime_->sequences->Events()) {
            if (e.kind == seq::SequenceEvent::Kind::Marker) {
                const bp::VmValue args[] = {e.name, e.payload};
                runtime_->blueprints->VM().Dispatch(e.entity, seq::SequenceSystem::kMarkerEvent, args);
            } else if (e.kind == seq::SequenceEvent::Kind::Finished) {
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
                        runtime_->audio->Player().Stop(handle, e.payload == "fade_out" ? e.fade : 0.0f);
                    } else {
                        const auto cue = runtime_->cues.find(e.name);
                        if (cue != runtime_->cues.end()) {
                            runtime_->audio->Player().Stop(handle);
                            audio::CuePlayParams params;
                            params.force_2d = true;
                            params.volume_db = e.value;
                            params.fade_in = e.payload == "fade_in" ? e.fade : 0.0f;
                            handle = runtime_->audio->Player().Play(cue->second, params);
                        } else warnings_.push_back("Sequence audio cue missing: " + e.name);
                    }
                }
            }
            // Animation keys still require an animation host in the player.
        }
    };
    scheduler_.Add(std::move(sequencer));
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

FrameContext Game::Tick(f32 dt) {
    if (!loop_) BuildFrame();
    UpdateProofMenu();
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
    if (back) {
        proof_menu_ = proof_menu_ == 0 ? 1 : proof_menu_ == 1 ? 0 : 1;
        proof_menu_row_ = 0;
        proof_settings_status_ = 0;
    } else if (proof_menu_ == 1) {
        proof_menu_row_ = (proof_menu_row_ + (down ? 1 : up ? 5 : 0)) % 6;
        if (accept) switch (proof_menu_row_) {
        case 0: proof_menu_ = 0; break;
        case 1: proof_menu_ = 2; proof_menu_row_ = 0; break;
        case 2: proof_menu_ = 3; break;
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
    } else if (proof_menu_ == 3 && accept) { proof_menu_ = 1; proof_menu_row_ = 0; }
    if (previous_menu != proof_menu_) paused_ = proof_menu_ != 0;
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
    attributes->Define("SettingsStatus", 0, -2, 1);
    attributes->SetBase("MenuState", static_cast<f32>(proof_menu_));
    attributes->SetBase("MenuRow", static_cast<f32>(proof_menu_row_));
    attributes->SetBase("SettingsStatus", static_cast<f32>(proof_settings_status_));
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
