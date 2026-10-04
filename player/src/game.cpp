#include "aether/player/game.h"

#include "aether/core/log.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/serialization.h"
#include "aether/sprite2d/components.h"
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
    if (m.value("$type", "") != "CookManifest") return Fail(error, "Manifest.json isn't a cook manifest");
    if (m.value("$version", 0) != 1) return Fail(error, "Manifest.json has an unsupported version");
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
#if AETHER_GAME_SCRIPTING
    script::LuauHost host;
    std::unique_ptr<script::ScriptSystem> scripts;
#endif
    std::unique_ptr<bp::BlueprintSystem> blueprints;
};

Game::Game(GamePackage& package) : package_(package), world_(std::make_unique<World>()) {
    lifecycle_ = std::make_unique<Lifecycle>(*world_, guids_);
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
    runtime_.reset(); // scripts and Blueprints before the world, lifecycle and input they use
    physics2d_.reset();
    physics_.reset(); // its bodies before the world they belong to
    while (!modules_.empty()) {
        modules_.back().second->Shutdown();
        modules_.pop_back();
    }
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

bool Game::LoadStartupScene(std::string* error) {
    if (!package_.HasManifest() && !package_.LoadManifest(error)) return false;
    if (package_.Manifest().startup_scene.empty()) return Fail(error, "The game has no startup scene");
    return LoadScene(package_.Manifest().startup_scene, error);
}

bool Game::LoadScene(const std::string& path, std::string* error) {
    StartModules(); // their components, before the scene names them
    sprite2d::RegisterSprite2DComponents(); // sprites and tilemaps (§26.6)
    sprite2d::RegisterPhysics2DComponents(); // 2D bodies and colliders
    sprite2d::RegisterPlatformerComponents();
    std::vector<u8> bytes;
    std::string read_error;
    if (!package_.ReadContent(path, bytes, &read_error)) {
        return Fail(error, "Can't read the scene " + path + (read_error.empty() ? "" : ": " + read_error));
    }
    LoadInputAssets();
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

    world_ = std::move(world);
    guids_.Clear();
    guids_.Rebuild(*world_);
    warnings_.clear();
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
    i32 priority = 0;
    for (const std::string& name : input_library_.ContextNames()) input_library_.Activate(input_, name, priority++);
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
    // Timers and Blueprint ticks run with the frame's Update.
    SystemDesc scripting;
    scripting.name = "Player.Scripting";
    scripting.phase = SystemPhase::Update;
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
