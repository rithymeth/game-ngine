#include "aether/player/game.h"
#include "aether/core/json_util.h"

#include "aether/sequencer/sequence_system.h"

#include "aether/core/log.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/gameplay_kit.h"
#if AETHER_KIT_INVENTORY
#include "aether/inventory/inventory_kit.h"
#include "aether/inventory/inventory_system.h"
#endif
#if AETHER_KIT_INTERACTION
#include "aether/interaction/interaction_kit.h"
#include "aether/interaction/interaction_system.h"
#endif
#if AETHER_KIT_QUESTS
#include "aether/quests/quest_system.h"
#include "aether/quests/quests_kit.h"
#endif
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/effect_system.h"
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
#include <system_error>

#include "aether/blueprint/graph.h"
#include "aether/blueprint/system.h"
#if AETHER_GAME_SCRIPTING
#include "aether/script/script_system.h"
#endif

#if AETHER_GAME_PHYSICS
#include "aether/job/job_system.h"
#include "aether/physics/components.h"
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
};
#else
struct Game::Physics {};
#endif

bool Game::HasPhysics() { return AETHER_GAME_PHYSICS != 0; }
bool Game::HasScripting() { return AETHER_GAME_SCRIPTING != 0; }

// What runs the scene's scripts and Blueprints. Its parts refer to the
// world, the GUID index and the input, so it's built per scene and torn down
// before any of them.
struct Game::Runtime {
    std::unique_ptr<seq::SequenceSystem> sequences;
    std::unique_ptr<gas::AttributeSystem> attributes; // Phase 30
    std::unique_ptr<gas::EffectSystem> effects;
    std::unique_ptr<gas::AbilitySystem> abilities;
#if AETHER_KIT_INVENTORY
    std::unique_ptr<inv::InventorySystem> inventory;
#endif
#if AETHER_KIT_INTERACTION
    std::unique_ptr<interact::InteractionSystem> interaction;
#endif
#if AETHER_KIT_QUESTS
    std::unique_ptr<quest::QuestSystem> quests;
#endif
#if AETHER_GAME_SCRIPTING
    script::LuauHost host;
    std::unique_ptr<script::ScriptSystem> scripts;
#endif
    std::unique_ptr<bp::BlueprintSystem> blueprints;
};

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
    return warnings;
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

void Game::BuildKits() {
    // The one place that knows which kits this build has. Each kit says what it needs, which components it
    // registers and which stage it adds; the registry orders them.
    kits_.Add(gas::MakeGameplayKit());
#if AETHER_KIT_INVENTORY
    kits_.Add(inv::MakeInventoryKit());
#endif
#if AETHER_KIT_INTERACTION
    kits_.Add(interact::MakeInteractionKit());
#endif
#if AETHER_KIT_QUESTS
    kits_.Add(quest::MakeQuestsKit());
#endif
    if (!kits_.Resolve()) {
        for (const std::string& e : kits_.Errors()) AETHER_LOG_ERROR("Player", "Kits: %s", e.c_str());
    }
}

void Game::ApplyKitStage(SystemDesc& system, const std::string& name) const {
    for (const kit::KitStage& stage : kits_.Stages()) {
        if (stage.name != name) continue;
        system.name = stage.name;
        system.after = stage.after;
        return;
    }
    AETHER_LOG_ERROR("Player", "Kits: no stage named %s", name.c_str());
}

bool Game::LoadStartupScene(std::string* error) {
    if (!package_.HasManifest() && !package_.LoadManifest(error)) return false;
    if (package_.Manifest().startup_scene.empty()) return Fail(error, "The game has no startup scene");
    return LoadScene(package_.Manifest().startup_scene, error);
}

bool Game::LoadScene(const std::string& path, std::string* error) {
    StartModules(); // their components, before the scene names them
    RegisterSequenceComponents(); // SequenceComponent (§27.2)
    kits_.RegisterComponents(); // the gameplay kits' components: attributes, tags, items, interactables, quests (§30)
    sequences_.clear(); // the old scene's sequences go with its runtime (below)
    sprite2d::RegisterSprite2DComponents(); // sprites and tilemaps (§26.6)
    sprite2d::RegisterPhysics2DComponents(); // 2D bodies and colliders
    sprite2d::RegisterPlatformerComponents();
    sprite2d::RegisterLight2DComponents();
    std::vector<u8> bytes;
    std::string read_error;
    if (!package_.ReadContent(path, bytes, &read_error)) {
        return Fail(error, "Can't read the scene " + path + (read_error.empty() ? "" : ": " + read_error));
    }
    LoadInputAssets();
    LoadLocalization();
    LoadEffects();
    LoadAbilities(); // after the effects they name
    LoadItems();
    LoadQuests();
    EndPlay();
    runtime_.reset();
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

void Game::LoadEffects() {
    if (effects_loaded_) return;
    effects_loaded_ = true;
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (asset.importer != "GameplayEffect") continue;
        std::vector<u8> bytes;
        std::string error;
        if (!package_.ReadContent(asset.path, bytes, &error)) {
            warnings_.push_back(error);
            continue;
        }
        gas::GameplayEffect effect;
        if (!gas::EffectFromJson(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), effect, &error) || !effects_.Register(std::move(effect), &error)) {
            effect_warnings_.push_back(asset.path + ": " + error);
            AETHER_LOG_WARN("Player", "%s: %s", asset.path.c_str(), error.c_str());
        }
    }
}

void Game::LoadQuests() {
    if (quests_loaded_) return;
    quests_loaded_ = true;
#if AETHER_KIT_QUESTS
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (asset.importer != "QuestDefinition") continue;
        std::vector<u8> bytes;
        std::string error;
        if (!package_.ReadContent(asset.path, bytes, &error)) {
            warnings_.push_back(error);
            continue;
        }
        quest::QuestDef def;
        if (!quest::QuestFromJson(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), def, &error) || !quests_.Register(std::move(def), &error)) {
            effect_warnings_.push_back(asset.path + ": " + error);
            AETHER_LOG_WARN("Player", "%s: %s", asset.path.c_str(), error.c_str());
        }
    }
    // Problems across the quests (a prerequisite that isn't one, a cycle, a missing reward effect).
    for (const std::string& problem : quests_.Check(&effects_)) {
        effect_warnings_.push_back(problem);
        AETHER_LOG_WARN("Player", "%s", problem.c_str());
    }
#endif
}

void Game::LoadItems() {
    if (items_loaded_) return;
    items_loaded_ = true;
#if AETHER_KIT_INVENTORY
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (asset.importer != "ItemDefinition") continue;
        std::vector<u8> bytes;
        std::string error;
        if (!package_.ReadContent(asset.path, bytes, &error)) {
            warnings_.push_back(error);
            continue;
        }
        inv::ItemDef item;
        if (!inv::ItemFromJson(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), item, &error) ||
            !(error = inv::ItemLibrary::CheckEffects(item, effects_)).empty() || !items_.Register(std::move(item), &error)) {
            effect_warnings_.push_back(asset.path + ": " + error);
            AETHER_LOG_WARN("Player", "%s: %s", asset.path.c_str(), error.c_str());
        }
    }
#endif
}

void Game::LoadAbilities() {
    if (abilities_loaded_) return;
    abilities_loaded_ = true;
    for (const GameManifest::Asset& asset : package_.Manifest().assets) {
        if (asset.importer != "GameplayAbility") continue;
        std::vector<u8> bytes;
        std::string error;
        if (!package_.ReadContent(asset.path, bytes, &error)) {
            warnings_.push_back(error);
            continue;
        }
        gas::GameplayAbility ability;
        if (!gas::AbilityFromJson(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), ability, &error) ||
            !(error = gas::AbilityLibrary::CheckEffects(ability, effects_)).empty() || !abilities_.Register(std::move(ability), &error)) {
            effect_warnings_.push_back(asset.path + ": " + error);
            AETHER_LOG_WARN("Player", "%s: %s", asset.path.c_str(), error.c_str());
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
#if AETHER_KIT_INVENTORY
    runtime_->inventory = std::make_unique<inv::InventorySystem>(*world_, runtime_->effects.get(), items_);
#endif
    runtime_->abilities = std::make_unique<gas::AbilitySystem>(*world_, *runtime_->attributes, *runtime_->effects, abilities_, effects_);
#if AETHER_KIT_QUESTS
    runtime_->quests = std::make_unique<quest::QuestSystem>(*world_, runtime_->effects.get(), quests_);
#endif
#if AETHER_KIT_INTERACTION
    runtime_->interaction = std::make_unique<interact::InteractionSystem>(*world_, runtime_->effects.get(), runtime_->abilities.get());
#endif
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
        });
    runtime_->scripts->Register(*lifecycle_);
    runtime_->scripts->BindInput(&input_);
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
        if (physics_) physics_->scene->Step(frame.fixed_dt);
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
            // Audio and Animation keys are collected for hosts with those systems; the player has none yet.
        }
    };
    scheduler_.Add(std::move(sequencer));
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
    // Attribute changes (§30.2) reach the changed entity's Blueprint as
    // Event.OnAttributeChanged (name, old, new), after gameplay has run.
    // Lasting effects (§30.3) tick before the attributes they change are reported.
    SystemDesc effects;
    effects.name = "Player.Effects";
    effects.phase = SystemPhase::Update;
    effects.after = {"Player.Update", "Player.Sequencer"};
    effects.main_thread_only = true;
    effects.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->effects) return;
        runtime_->effects->Update(frame.dt);
        const std::vector<gas::EffectEvent> events = runtime_->effects->Events();
        runtime_->effects->ClearEvents();
        for (const gas::EffectEvent& e : events) {
#if AETHER_GAME_SCRIPTING
            // A script hears it as OnEffectApplied / OnEffectRemoved (effect, handle).
            if (runtime_->scripts) runtime_->scripts->SendEvent(e.entity, e.applied ? "OnEffectApplied" : "OnEffectRemoved", {e.effect, static_cast<f64>(e.handle)});
#endif
            if (!runtime_->blueprints) continue;
            const bp::VmValue args[] = {e.effect, static_cast<i32>(e.handle)};
            runtime_->blueprints->VM().Dispatch(e.entity, e.applied ? gas::EffectSystem::kAppliedEvent : gas::EffectSystem::kRemovedEvent, args);
        }
    };
    scheduler_.Add(std::move(effects));
    // Abilities (§30.4) run after this frame's effects, so cooldown tags are current when scripts activate.
    SystemDesc abilities;
    abilities.name = "Player.Abilities";
    abilities.phase = SystemPhase::Update;
    abilities.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    abilities.main_thread_only = true;
    abilities.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->abilities) return;
        runtime_->abilities->Update(frame.dt);
        const std::vector<gas::AbilityEvent> events = runtime_->abilities->Events();
        runtime_->abilities->ClearEvents();
        for (const gas::AbilityEvent& e : events) {
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
            if (!runtime_->blueprints) continue;
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
        }
    };
    scheduler_.Add(std::move(abilities));
#if AETHER_KIT_INVENTORY
    // Inventory changes (§30.7) reach the owner's script and Blueprint, after effects have run.
    SystemDesc inventory;
    ApplyKitStage(inventory, "Player.Inventory");
    inventory.phase = SystemPhase::Update;
    inventory.main_thread_only = true;
    inventory.run = [this](World&, const FrameContext&) {
        if (!runtime_ || !runtime_->inventory) return;
        const std::vector<inv::ItemEvent> events = runtime_->inventory->Events();
        runtime_->inventory->ClearEvents();
        for (const inv::ItemEvent& e : events) {
            using Kind = inv::ItemEvent::Kind;
#if AETHER_KIT_QUESTS
            // Items picked up and put down advance "count" objectives that name the item.
            if (runtime_->quests && (e.kind == Kind::Added || e.kind == Kind::Removed)) {
                runtime_->quests->Notify(e.entity, quest::Objective::Kind::Count, e.item, e.kind == Kind::Added ? e.count : -e.count);
            }
#endif
#if AETHER_GAME_SCRIPTING
            if (runtime_->scripts) {
                const f64 n = static_cast<f64>(e.count);
                switch (e.kind) {
                case Kind::Added: runtime_->scripts->SendEvent(e.entity, "OnItemAdded", {e.item, n}); break;
                case Kind::Removed: runtime_->scripts->SendEvent(e.entity, "OnItemRemoved", {e.item, n}); break;
                case Kind::Equipped: runtime_->scripts->SendEvent(e.entity, "OnItemEquipped", {e.item, e.slot}); break;
                case Kind::Unequipped: runtime_->scripts->SendEvent(e.entity, "OnItemUnequipped", {e.item, e.slot}); break;
                case Kind::Used: runtime_->scripts->SendEvent(e.entity, "OnItemUsed", {e.item}); break;
                }
            }
#endif
            if (!runtime_->blueprints) continue;
            switch (e.kind) {
            case Kind::Added: {
                const bp::VmValue args[] = {e.item, e.count};
                runtime_->blueprints->VM().Dispatch(e.entity, inv::InventorySystem::kAddedEvent, args);
                break;
            }
            case Kind::Removed: {
                const bp::VmValue args[] = {e.item, e.count};
                runtime_->blueprints->VM().Dispatch(e.entity, inv::InventorySystem::kRemovedEvent, args);
                break;
            }
            case Kind::Equipped:
            case Kind::Unequipped: {
                const bp::VmValue args[] = {e.item, e.slot};
                runtime_->blueprints->VM().Dispatch(e.entity, e.kind == Kind::Equipped ? inv::InventorySystem::kEquippedEvent : inv::InventorySystem::kUnequippedEvent, args);
                break;
            }
            case Kind::Used: {
                const bp::VmValue args[] = {e.item};
                runtime_->blueprints->VM().Dispatch(e.entity, inv::InventorySystem::kUsedEvent, args);
                break;
            }
            }
        }
    };
    scheduler_.Add(std::move(inventory));
#endif
#if AETHER_KIT_INTERACTION
    // Interaction (§30.8): cooldowns tick, and uses reach the target's script and Blueprint (OnInteract),
    // failures the user's (OnInteractFailed).
    SystemDesc interaction;
    ApplyKitStage(interaction, "Player.Interaction");
    interaction.phase = SystemPhase::Update;
    interaction.main_thread_only = true;
    interaction.run = [this](World&, const FrameContext& frame) {
        if (!runtime_ || !runtime_->interaction) return;
        runtime_->interaction->Update(frame.dt);
        const std::vector<interact::InteractionEvent> events = runtime_->interaction->Events();
        runtime_->interaction->ClearEvents();
        for (const interact::InteractionEvent& e : events) {
            const bool ok = e.kind == interact::InteractionEvent::Kind::Interacted;
#if AETHER_GAME_SCRIPTING
            if (runtime_->scripts) {
                if (ok) runtime_->scripts->SendEvent(e.target, "OnInteract", {script::EntityRef{e.interactor}});
                else runtime_->scripts->SendEvent(e.interactor, "OnInteractFailed", {script::EntityRef{e.target}, std::string(interact::ReasonName(e.reason))});
            }
#endif
            if (!runtime_->blueprints) continue;
            if (ok) {
                const bp::VmValue args[] = {e.interactor};
                runtime_->blueprints->VM().Dispatch(e.target, interact::InteractionSystem::kInteractEvent, args);
            } else {
                const bp::VmValue args[] = {e.target, std::string(interact::ReasonName(e.reason))};
                runtime_->blueprints->VM().Dispatch(e.interactor, interact::InteractionSystem::kFailedEvent, args);
            }
        }
    };
    scheduler_.Add(std::move(interaction));
#endif
#if AETHER_KIT_QUESTS
    // Quest changes (§30.10) reach the owner's script and Blueprint; reward items go to the inventory kit.
    SystemDesc quests;
    ApplyKitStage(quests, "Player.Quests");
    quests.phase = SystemPhase::Update;
    // Reward items go to the inventory and objectives follow its events, so quests run after the kits it can hear from.
    for (const char* other : {"Inventory", "Interaction"}) {
        if (kits_.Has(other)) quests.after.push_back(std::string("Player.") + other);
    }
    quests.main_thread_only = true;
    quests.run = [this](World&, const FrameContext&) {
        if (!runtime_ || !runtime_->quests) return;
        const std::vector<quest::QuestEvent> events = runtime_->quests->Events();
        runtime_->quests->ClearEvents();
        for (const quest::QuestEvent& e : events) {
            using Kind = quest::QuestEvent::Kind;
#if AETHER_KIT_INVENTORY
            if (e.kind == Kind::Completed && runtime_->inventory) {
                for (const quest::RewardItem& r : e.reward_items) runtime_->inventory->Add(e.entity, r.item, r.count);
            }
#endif
#if AETHER_GAME_SCRIPTING
            if (runtime_->scripts) {
                const f64 progress = static_cast<f64>(e.progress), required = static_cast<f64>(e.required);
                switch (e.kind) {
                case Kind::Started: runtime_->scripts->SendEvent(e.entity, "OnQuestStarted", {e.quest}); break;
                case Kind::ObjectiveProgress: runtime_->scripts->SendEvent(e.entity, "OnQuestProgress", {e.quest, e.objective, progress, required}); break;
                case Kind::ObjectiveCompleted: runtime_->scripts->SendEvent(e.entity, "OnQuestObjectiveCompleted", {e.quest, e.objective}); break;
                case Kind::Completed: runtime_->scripts->SendEvent(e.entity, "OnQuestCompleted", {e.quest}); break;
                case Kind::Failed: runtime_->scripts->SendEvent(e.entity, "OnQuestFailed", {e.quest}); break;
                case Kind::Abandoned: runtime_->scripts->SendEvent(e.entity, "OnQuestAbandoned", {e.quest}); break;
                }
            }
#endif
            if (!runtime_->blueprints) continue;
            switch (e.kind) {
            case Kind::Started: {
                const bp::VmValue args[] = {e.quest};
                runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestStarted", args);
                break;
            }
            case Kind::ObjectiveProgress: {
                const bp::VmValue args[] = {e.quest, e.objective, e.progress, e.required};
                runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestProgress", args);
                break;
            }
            case Kind::Completed: {
                const bp::VmValue args[] = {e.quest};
                runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestCompleted", args);
                break;
            }
            case Kind::Failed: {
                const bp::VmValue args[] = {e.quest};
                runtime_->blueprints->VM().Dispatch(e.entity, "Event.OnQuestFailed", args);
                break;
            }
            case Kind::ObjectiveCompleted:
            case Kind::Abandoned: break; // scripts hear these; Blueprints have the progress and failed events
            }
        }
    };
    scheduler_.Add(std::move(quests));
#endif
    SystemDesc attributes;
    attributes.name = "Player.Attributes";
    attributes.phase = SystemPhase::Update;
    attributes.after = {"Player.Update", "Player.Sequencer", "Player.Effects"};
    attributes.main_thread_only = true;
    attributes.run = [this](World&, const FrameContext&) {
        if (!runtime_ || !runtime_->attributes) return;
        // Take the queue first: a handler that changes an attribute queues the next event for next frame.
        const std::vector<gas::AttributeEvent> events = runtime_->attributes->Events();
        runtime_->attributes->ClearEvents();
        for (const gas::AttributeEvent& e : events) {
#if AETHER_GAME_SCRIPTING
            // A script hears it as OnAttributeChanged (name, old, new).
            if (runtime_->scripts) runtime_->scripts->SendEvent(e.entity, "OnAttributeChanged", {e.name, static_cast<f64>(e.old_value), static_cast<f64>(e.new_value)});
#endif
            if (!runtime_->blueprints) continue;
            const bp::VmValue args[] = {e.name, e.old_value, e.new_value};
            runtime_->blueprints->VM().Dispatch(e.entity, gas::AttributeSystem::kChangedEvent, args);
        }
    };
    scheduler_.Add(std::move(attributes));
    // Timers and Blueprint ticks run with the frame's Update.
    SystemDesc scripting;
    scripting.name = "Player.Scripting";
    scripting.phase = SystemPhase::Update;
    scripting.after = {"Player.Sequencer", "Player.Save", "Player.Attributes", "Player.Effects", "Player.Abilities"};
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
    input_.Update(input_state_, dt); // the host's keys and mouse, as this frame's actions
    const FrameContext frame = loop_->Tick(*world_, dt);
    input_state_.EndFrame();         // movement deltas are per frame; held keys stay
    stats_.frames = frame.frame + 1;
    stats_.fixed_steps = frame.fixed_step;
    stats_.time = frame.time;
    return frame;
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
