#include "game_tools.h"

#include "screenshot.h"

#include "aether/gameplay/attribute_library.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/input/keys.h"
#if AETHER_MCP_INTERACTION
#include "aether/interaction/interaction_system.h"
#endif
#include "aether/player/game.h"
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

namespace aether::mcp {

namespace {

namespace stdfs = std::filesystem;
using player::Game;
using player::GamePackage;

constexpr i64 kMaxStepFrames = 100000;

// The package and the game on it. The game refers to the package, so it goes first.
struct GameHost {
    std::unique_ptr<GamePackage> package;
    std::unique_ptr<Game> game;
    std::vector<std::string> mounted;
    std::vector<std::string> notes; // module start-up messages, load warnings
    std::unique_ptr<GameScreenshotter> shots; // made on the first game_screenshot
    unsigned shot_counter = 0;

    void Unload() {
        if (shots) shots->Reset(); // it draws the game and package below
        if (game) game->EndPlay();
        game.reset();
        package.reset();
        mounted.clear();
        notes.clear();
    }
    Game& Require() const {
        if (!game) throw ToolError("No game loaded: call game_load first");
        return *game;
    }
};

std::string RequireString(const Json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw ToolError(std::string("Missing string argument \"") + key + "\"");
    }
    return args[key].get<std::string>();
}

Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

std::vector<Entity> AllEntities(const World& world) {
    std::vector<Entity> entities;
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* chunk = archetype.EntityArray(c);
            entities.insert(entities.end(), chunk, chunk + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return entities;
}

std::string ComponentName(ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    return info.reflected != nullptr ? info.reflected->name : info.name;
}

// An entity's handle in this API: its GUID when it has one, else "#<index>".
std::string EntityKey(const World& world, Entity e) {
    if (const IdComponent* id = world.GetComponent<IdComponent>(e)) return ToString(id->guid);
    return "#" + std::to_string(e.index);
}

Entity FindEntity(Game& game, const std::string& key) {
    World& world = game.GetWorld();
    if (!key.empty() && key[0] == '#') {
        u64 index = 0;
        try {
            index = std::stoull(key.substr(1));
        } catch (...) {
            throw ToolError("Not a valid entity handle: " + key);
        }
        for (Entity e : AllEntities(world)) {
            if (e.index == index) return e;
        }
        throw ToolError("No entity " + key);
    }
    EntityGuid guid;
    if (!ParseEntityGuid(key, guid)) throw ToolError("Not a valid entity handle: " + key);
    Entity e = game.Guids().Find(world, guid);
    if (e.IsNull()) throw ToolError("No entity with GUID " + key);
    return e;
}

Entity RequireEntity(Game& game, const Json& args) { return FindEntity(game, RequireString(args, "entity")); }

ComponentId RequireComponent(const std::string& name) {
    ComponentId id = FindComponentIdByName(name);
    if (id == kInvalidComponentId || id >= RegisteredComponentCount() || GetComponentInfo(id).reflected == nullptr) {
        throw ToolError("Unknown component \"" + name + "\" (see list_component_types)");
    }
    return id;
}

Json EntityJson(const World& world, Entity e) {
    Json components = Json::object();
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!world.HasComponentRaw(e, id)) continue;
        const ComponentInfo& info = GetComponentInfo(id);
        components[ComponentName(id)] = info.reflected != nullptr ? reflect::ToJson(*info.reflected, world.GetComponentRaw(e, id))
                                                                  : Json("<unreflected component>");
    }
    return {{"entity", EntityKey(world, e)}, {"components", components}};
}

std::string Base64(const std::vector<u8>& bytes) {
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (usize i = 0; i < bytes.size(); i += 3) {
        const u32 n = (static_cast<u32>(bytes[i]) << 16) | (i + 1 < bytes.size() ? static_cast<u32>(bytes[i + 1]) << 8 : 0) |
                      (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
    }
    return out;
}

Json StatsJson(Game& game) {
    const player::GameStats s = game.Stats();
    Json errors = Json::array();
    for (const std::string& e : game.ScriptErrors()) errors.push_back(e);
    for (const std::string& e : game.BlueprintErrors()) errors.push_back(e);
    return {{"scene", game.SceneName()},
            {"playing", game.IsPlaying()},
            {"paused", game.IsPaused()},
            {"exit_requested", game.ExitRequested()},
            {"frames", s.frames},
            {"fixed_steps", s.fixed_steps},
            {"time", s.time},
            {"entities", s.entities},
            {"prefab_instances", s.prefab_instances},
            {"physics_bodies", s.physics_bodies},
            {"script_instances", s.script_instances},
            {"script_errors", s.script_errors},
            {"blueprint_instances", s.blueprint_instances},
            {"blueprint_errors", s.blueprint_errors},
            {"errors", errors},
            {"warnings", game.Warnings()}};
}

} // namespace

void RegisterGameTools(McpServer& server) {
    auto host = std::make_shared<GameHost>();

    server.AddTool(
        {"game_load",
         "Load a cooked game and start playing it: mounts the .apak archives (or loose-cook folders), reads Manifest.json, "
         "loads the startup scene (or `scene`), starts the manifest's runtime modules and begins play. Replaces any loaded game. "
         "Runs every system the player runs (physics, scripts, Blueprints, audio, sequences, gameplay kits, saves).",
         Schema({{"paks", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Archives, loose-cook folders, or folders of .apak files"}}},
                 {"scene", {{"type", "string"}, {"description", "Cooked scene path to load instead of the startup scene"}}},
                 {"user_dir", {{"type", "string"}, {"description", "Folder for saves and settings (default: the OS user folder for the project)"}}},
                 {"key", {{"type", "string"}, {"description", "64 hex digits, for encrypted archives"}}}},
                {"paks"}),
         [host](const Json& args) -> Json {
             if (!args["paks"].is_array() || args["paks"].empty()) throw ToolError("\"paks\" must be a non-empty array of paths");
             host->Unload();
             auto package = std::make_unique<GamePackage>();
             if (args.contains("key") && args["key"].is_string()) {
                 pak::PakKey key;
                 if (!pak::PakKey::FromHex(args["key"].get<std::string>(), key)) throw ToolError("\"key\" must be 64 hex digits");
                 package->AddKey(key);
             }
             std::vector<std::string> sources;
             for (const Json& p : args["paks"]) {
                 if (!p.is_string()) throw ToolError("\"paks\" entries must be strings");
                 std::error_code ec;
                 const std::string path = p.get<std::string>();
                 if (stdfs::is_directory(path, ec) && !stdfs::exists(stdfs::path(path) / "Manifest.json", ec)) {
                     for (std::string& f : GamePackage::FindPaks(path)) sources.push_back(std::move(f));
                 } else {
                     sources.push_back(path);
                 }
             }
             if (sources.empty()) throw ToolError("No .apak found in the given paths");
             std::string error;
             for (usize i = 0; i < sources.size(); ++i) {
                 if (!package->Mount(sources[i], static_cast<int>(i), &error)) throw ToolError(error);
             }
             if (!package->LoadManifest(&error)) throw ToolError(error);

             host->package = std::move(package);
             host->mounted = sources;
             host->game = std::make_unique<Game>(*host->package);
             Game& game = *host->game;
             const std::string project = host->package->Manifest().project;
             game.SetUserPaths(player::ResolveUserPaths(
                 project, args.contains("user_dir") && args["user_dir"].is_string() ? stdfs::path(args["user_dir"].get<std::string>()) : stdfs::path()));
             host->notes = game.StartModules();
             const bool loaded = args.contains("scene") && args["scene"].is_string() ? game.LoadScene(args["scene"].get<std::string>(), &error)
                                                                                     : game.LoadStartupScene(&error);
             if (!loaded) {
                 host->Unload();
                 throw ToolError(error);
             }
             game.BeginPlay();
             Json out = StatsJson(game);
             out["project"] = project;
             out["mounted"] = sources;
             out["module_notes"] = host->notes;
             return out;
         }});

    server.AddTool({"game_unload", "Stop and unload the hosted game.", Schema(Json::object()), [host](const Json&) -> Json {
                        const bool had = host->game != nullptr;
                        host->Unload();
                        return {{"unloaded", had}};
                    }});

    server.AddTool({"game_state", "Frames, time, entity and physics counts, script/Blueprint errors and load warnings of the hosted game.",
                    Schema(Json::object()), [host](const Json&) -> Json { return StatsJson(host->Require()); }});

    server.AddTool(
        {"game_step",
         "Run the game for a number of frames (default 1) of dt seconds (default: the project's fixed step), running every system "
         "in order: input, fixed-step physics, scripts, Blueprints, sequences, audio, gameplay kits, saves.",
         Schema({{"frames", {{"type", "integer"}, {"description", "1-100000 (default 1)"}}},
                 {"dt", {{"type", "number"}, {"description", "Seconds per frame, (0, 1]"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             i64 frames = 1;
             if (args.contains("frames")) {
                 if (!args["frames"].is_number_integer()) throw ToolError("\"frames\" must be an integer");
                 frames = args["frames"].get<i64>();
             }
             if (frames < 1 || frames > kMaxStepFrames) throw ToolError("\"frames\" must be between 1 and 100000");
             f64 dt = 1.0 / host->package->Manifest().fixed_timestep_hz;
             if (args.contains("dt")) {
                 if (!args["dt"].is_number()) throw ToolError("\"dt\" must be a number");
                 dt = args["dt"].get<f64>();
             }
             if (!(dt > 0.0 && dt <= 1.0)) throw ToolError("\"dt\" must be in (0, 1] seconds");
             if (!game.IsPlaying()) game.BeginPlay();
             FrameContext last;
             for (i64 i = 0; i < frames && !game.ExitRequested(); ++i) last = game.Tick(static_cast<f32>(dt));
             Json out = StatsJson(game);
             out["frame"] = last.frame;
             return out;
         }});

    server.AddTool(
        {"game_screenshot",
         "Render the game's current frame off screen and return it as a PNG image (also saved to `path`). Needs a graphics device "
         "(Direct3D 12 or Vulkan). Draws what the player draws: the cooked scene and its HUD. Step the game first to see "
         "movement.",
         Schema({{"width", {{"type", "integer"}, {"description", "64-3840 (default: the project's window width)"}}},
                 {"height", {{"type", "integer"}, {"description", "64-2160 (default: the project's window height)"}}},
                 {"path", {{"type", "string"}, {"description", "Where to save the PNG (default: a temp folder)"}}},
                 {"backend", {{"type", "string"}, {"description", "d3d12 or vulkan"}}},
                 {"inline", {{"type", "boolean"}, {"description", "Return the image in the reply (default true)"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             const player::GameManifest& manifest = host->package->Manifest();
             auto dimension = [&](const char* key, u32 fallback, u32 lo, u32 hi) {
                 if (!args.contains(key)) return std::clamp(fallback, lo, hi);
                 if (!args[key].is_number_integer()) throw ToolError(std::string("\"") + key + "\" must be an integer");
                 const long long v = args[key].get<long long>();
                 if (v < lo || v > hi) throw ToolError(std::string("\"") + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi));
                 return static_cast<u32>(v);
             };
             const u32 width = dimension("width", manifest.window_width, 64, 3840);
             const u32 height = dimension("height", manifest.window_height, 64, 2160);
             const std::string backend = args.contains("backend") && args["backend"].is_string() ? args["backend"].get<std::string>() : "";
             if (!host->shots) host->shots = std::make_unique<GameScreenshotter>();
             GameScreenshotter::Result r = host->shots->Capture(game, *host->package, width, height, backend);
             if (!r.ok) throw ToolError("Screenshot failed: " + r.error);

             stdfs::path path;
             if (args.contains("path") && args["path"].is_string()) {
                 path = args["path"].get<std::string>();
             } else {
                 path = stdfs::temp_directory_path() / "aether_mcp_screenshots" / ("shot_" + std::to_string(++host->shot_counter) + ".png");
             }
             std::error_code ec;
             if (path.has_parent_path()) stdfs::create_directories(path.parent_path(), ec);
             {
                 std::ofstream file(path, std::ios::binary);
                 file.write(reinterpret_cast<const char*>(r.png.data()), static_cast<std::streamsize>(r.png.size()));
                 if (!file) throw ToolError("Could not write " + path.string());
             }
             Json out = {{"path", path.string()}, {"width", r.width}, {"height", r.height}, {"bytes", r.png.size()}, {"frame", game.Stats().frames}};
             if (!args.contains("inline") || !args["inline"].is_boolean() || args["inline"].get<bool>()) {
                 out["mcp_content"] = Json::array({{{"type", "image"}, {"data", Base64(r.png)}, {"mimeType", "image/png"}}});
             }
             return out;
         }});

    server.AddTool({"game_set_paused", "Freeze or resume the simulation (input stays available to menus).",
                    Schema({{"paused", {{"type", "boolean"}}}}, {"paused"}), [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        if (!args["paused"].is_boolean()) throw ToolError("\"paused\" must be a boolean");
                        game.SetPaused(args["paused"].get<bool>());
                        return {{"paused", game.IsPaused()}};
                    }});

    server.AddTool(
        {"game_input",
         "Set input for the game. Each entry of `set` is a key name (e.g. \"W\", \"Space\", \"MouseLeft\") and a value: 1 or 0 for "
         "buttons, -1..1 for axes. Values are held until changed. `mouse_delta` [dx, dy] applies to the next frame only. "
         "`clear` releases everything first.",
         Schema({{"set", {{"type", "object"}, {"description", "{ \"W\": 1, \"Space\": 0 }"}}},
                 {"mouse_delta", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                 {"clear", {{"type", "boolean"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             if (args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>()) game.Input().Clear();
             Json applied = Json::object();
             if (args.contains("set")) {
                 if (!args["set"].is_object()) throw ToolError("\"set\" must be an object of key names to values");
                 for (auto& [name, value] : args["set"].items()) {
                     const input::Key key = input::KeyFromName(name);
                     if (key == input::Key::None) throw ToolError("No key named \"" + name + "\"");
                     if (!value.is_number() && !value.is_boolean()) throw ToolError("Value for \"" + name + "\" must be a number");
                     const f32 v = value.is_boolean() ? (value.get<bool>() ? 1.0f : 0.0f) : value.get<f32>();
                     game.Input().SetAxis(key, v);
                     applied[name] = v;
                 }
             }
             if (args.contains("mouse_delta")) {
                 const Json& d = args["mouse_delta"];
                 if (!d.is_array() || d.size() != 2 || !d[0].is_number() || !d[1].is_number()) {
                     throw ToolError("\"mouse_delta\" must be [dx, dy]");
                 }
                 game.Input().AddMouseDelta(d[0].get<f32>(), d[1].get<f32>());
             }
             return {{"applied", applied}};
         }});

    server.AddTool({"game_load_scene", "Replace the game's world with another cooked scene and begin play.",
                    Schema({{"scene", {{"type", "string"}}}}, {"scene"}), [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        std::string error;
                        if (!game.LoadScene(RequireString(args, "scene"), &error)) throw ToolError(error);
                        game.BeginPlay();
                        return StatsJson(game);
                    }});

    server.AddTool({"game_systems", "The game's systems in execution order, by phase.", Schema(Json::object()), [host](const Json&) -> Json {
                        Game& game = host->Require();
                        Json out = Json::object();
                        for (usize p = 0; p < kSystemPhaseCount; ++p) {
                            const SystemPhase phase = static_cast<SystemPhase>(p);
                            out[SystemPhaseName(phase)] = game.Systems().Order(phase);
                        }
                        return out;
                    }});

    server.AddTool(
        {"game_list_entities",
         "Entities in the running game: handle (GUID, or #index if it has none), name, tags and component names. "
         "Optionally only those with a component or tag.",
         Schema({{"component", {{"type", "string"}}}, {"tag", {{"type", "string"}}},
                 {"limit", {{"type", "integer"}, {"description", "Default 200"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             ComponentId filter = kInvalidComponentId;
             if (args.contains("component")) filter = RequireComponent(RequireString(args, "component"));
             std::vector<Entity> candidates;
             if (args.contains("tag")) candidates = FindEntitiesWithTag(world, RequireString(args, "tag"));
             else candidates = AllEntities(world);
             usize limit = 200;
             if (args.contains("limit") && args["limit"].is_number_integer()) limit = static_cast<usize>(std::max<i64>(1, args["limit"].get<i64>()));
             Json list = Json::array();
             usize total = 0;
             for (Entity e : candidates) {
                 if (filter != kInvalidComponentId && !world.HasComponentRaw(e, filter)) continue;
                 ++total;
                 if (list.size() >= limit) continue;
                 Json names = Json::array();
                 for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
                     if (world.HasComponentRaw(e, id)) names.push_back(ComponentName(id));
                 }
                 Json row = {{"entity", EntityKey(world, e)}, {"components", names}};
                 if (const EntityName* n = world.GetComponent<EntityName>(e)) row["name"] = n->value;
                 list.push_back(row);
             }
             return {{"total", total}, {"entities", list}};
         }});

    server.AddTool({"game_get_entity", "Every component of one entity in the running game, as JSON.",
                    Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index from game_list_entities"}}}}, {"entity"}),
                    [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        return EntityJson(game.GetWorld(), RequireEntity(game, args));
                    }});

    server.AddTool(
        {"game_set_component",
         "Change fields of a component on a running entity (partial update: only the fields named change). Not undoable; "
         "gone when the game is reloaded.",
         Schema({{"entity", {{"type", "string"}}}, {"component", {{"type", "string"}}}, {"values", {{"type", "object"}}}},
                {"entity", "component", "values"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             Entity e = RequireEntity(game, args);
             ComponentId id = RequireComponent(RequireString(args, "component"));
             if (!args["values"].is_object()) throw ToolError("\"values\" must be an object of fields");
             void* data = world.GetComponentRaw(e, id);
             if (data == nullptr) throw ToolError("Entity has no " + ComponentName(id));
             const ComponentInfo& info = GetComponentInfo(id);
             Json merged = reflect::ToJson(*info.reflected, data);
             merged.merge_patch(args["values"]);
             reflect::LoadReport report;
             if (!reflect::FromJson(*info.reflected, data, merged, &report)) {
                 throw ToolError("Value does not match the shape of " + ComponentName(id));
             }
             Json out = EntityJson(world, e);
             if (!report.warnings.empty()) out["warnings"] = report.warnings;
             return out;
         }});

    server.AddTool({"game_attributes", "The attributes (name, base, current, min, max) of an entity in the running game.",
                    Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index"}}}}, {"entity"}), [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        Entity e = RequireEntity(game, args);
                        const gas::AttributeSet* set = game.GetWorld().GetComponent<gas::AttributeSet>(e);
                        Json list = Json::array();
                        if (set != nullptr) {
                            for (const gas::Attribute& a : set->attributes) {
                                list.push_back({{"name", a.name}, {"base", a.base}, {"current", a.current}, {"min", a.min}, {"max", a.max}});
                            }
                        }
                        return {{"entity", EntityKey(game.GetWorld(), e)}, {"attributes", list}};
                    }});

    server.AddTool(
        {"game_set_attribute",
         "Change an attribute of a running entity the way the game's own code does (so attribute-changed events fire and bounds apply): "
         "set its `base`, add a `delta` to the base, or `define` it with bounds. Effects and abilities then see the new value.",
         Schema({{"entity", {{"type", "string"}}},
                 {"name", {{"type", "string"}}},
                 {"base", {{"type", "number"}, {"description", "Set the base to this"}}},
                 {"delta", {{"type", "number"}, {"description", "Add this to the base"}}},
                 {"define", {{"type", "object"}, {"description", "{base, min?, max?}: create or redefine the attribute"}}}},
                {"entity", "name"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             Entity e = RequireEntity(game, args);
             const std::string name = RequireString(args, "name");
             const int modes = (args.contains("base") ? 1 : 0) + (args.contains("delta") ? 1 : 0) + (args.contains("define") ? 1 : 0);
             if (modes != 1) throw ToolError("Give exactly one of \"base\", \"delta\" or \"define\"");
             auto number = [](const Json& j, const char* key, f32 fallback) {
                 if (!j.contains(key)) return fallback;
                 if (!j[key].is_number()) throw ToolError(std::string("\"") + key + "\" must be a number");
                 return j[key].get<f32>();
             };
             bool ok = false;
             if (args.contains("define")) {
                 if (!args["define"].is_object()) throw ToolError("\"define\" must be an object");
                 const Json& d = args["define"];
                 ok = gas::Attributes::DefineAttribute(e, name, number(d, "base", 0.0f), number(d, "min", -3.0e38f), number(d, "max", 3.0e38f));
             } else if (!gas::Attributes::HasAttribute(e, name)) {
                 throw ToolError("The entity has no attribute \"" + name + "\" (use define to create it)");
             } else if (args.contains("base")) {
                 ok = gas::Attributes::SetAttributeBase(e, name, number(args, "base", 0.0f));
             } else {
                 ok = gas::Attributes::AddAttributeBase(e, name, number(args, "delta", 0.0f));
             }
             if (!ok) throw ToolError("The change was refused (a NaN, a bad name, or no attribute system is active)");
             return {{"name", name},
                     {"base", gas::Attributes::GetAttributeBase(e, name, 0.0f)},
                     {"current", gas::Attributes::GetAttribute(e, name, 0.0f)},
                     {"min", gas::Attributes::GetAttributeMin(e, name, 0.0f)},
                     {"max", gas::Attributes::GetAttributeMax(e, name, 0.0f)}};
         }});

#if AETHER_MCP_INTERACTION
    // -- Interaction (the Interactable component and the game's InteractionSystem) ---------------------------------
    server.AddTool(
        {"game_interactables",
         "The usable things in the running game: entities with an Interactable, with prompt, range, state (enabled, used, cooling down) and, "
         "if an `interactor` is given, whether and why it can use each (the reason names are the ones scripts hear: out_of_range, "
         "missing_tag, blocked_tag, cooldown, used, disabled...).",
         Schema({{"interactor", {{"type", "string"}, {"description", "Entity that would use them (GUID or #index)"}}}}), [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             interact::InteractionSystem* system = interact::InteractionSystem::Active();
             Entity interactor{};
             if (args.contains("interactor")) interactor = RequireEntity(game, Json{{"entity", args["interactor"]}});
             Json out = Json::array();
             for (Entity e : AllEntities(world)) {
                 const interact::Interactable* i = world.GetComponent<interact::Interactable>(e);
                 if (i == nullptr) continue;
                 Json row = {{"entity", EntityKey(world, e)}, {"enabled", i->enabled}, {"prompt", i->prompt}, {"range", i->range},
                             {"one_shot", i->one_shot}, {"used", i->used}, {"cooldown", i->cooldown}, {"remaining", i->remaining}};
                 if (const EntityName* n = world.GetComponent<EntityName>(e)) row["name"] = n->value;
                 if (!interactor.IsNull() && system != nullptr) row["reason"] = interact::ReasonName(system->Check(interactor, e));
                 out.push_back(row);
             }
             return out;
         }});

    server.AddTool({"game_interact",
                    "Make `interactor` use `target` the way the game does: checks range, tags, cooldown, applies the target's effect and ability to the "
                    "interactor, starts its cooldown, and queues OnInteract for the target's script/Blueprint (heard on the next frame; step the game). "
                    "Returns whether it worked and, if not, the reason.",
                    Schema({{"interactor", {{"type", "string"}}}, {"target", {{"type", "string"}}}}, {"interactor", "target"}),
                    [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        interact::InteractionSystem* system = interact::InteractionSystem::Active();
                        if (system == nullptr) throw ToolError("This game has no interaction system");
                        Entity interactor = FindEntity(game, RequireString(args, "interactor"));
                        Entity target = FindEntity(game, RequireString(args, "target"));
                        const interact::Reason reason = system->Interact(interactor, target);
                        return {{"interacted", reason == interact::Reason::None}, {"reason", interact::ReasonName(reason)}};
                    }});

    server.AddTool({"game_can_interact", "Whether `interactor` could use `target` right now, and if not why (out_of_range, missing_tag, ...). Changes nothing.",
                    Schema({{"interactor", {{"type", "string"}}}, {"target", {{"type", "string"}}}}, {"interactor", "target"}),
                    [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        interact::InteractionSystem* system = interact::InteractionSystem::Active();
                        if (system == nullptr) throw ToolError("This game has no interaction system");
                        const interact::Reason reason = system->Check(FindEntity(game, RequireString(args, "interactor")), FindEntity(game, RequireString(args, "target")));
                        return {{"can_interact", reason == interact::Reason::None}, {"reason", interact::ReasonName(reason)}};
                    }});

    server.AddTool(
        {"game_interaction_focus",
         "What `interactor` would be prompted to use: the nearest usable target within its range, in front of `forward` ([x,y,z]; omit to look all "
         "round), measured from the interactor's position. Returns the target, its distance and prompt, or null.",
         Schema({{"interactor", {{"type", "string"}}}, {"forward", {{"type", "array"}, {"items", {{"type", "number"}}}}}}, {"interactor"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             interact::InteractionSystem* system = interact::InteractionSystem::Active();
             if (system == nullptr) throw ToolError("This game has no interaction system");
             Entity interactor = FindEntity(game, RequireString(args, "interactor"));
             const Transform* t = game.GetWorld().GetComponent<Transform>(interactor);
             if (t == nullptr) throw ToolError("The interactor has no Transform");
             Vec3 forward{0.0f, 0.0f, 0.0f};
             if (args.contains("forward")) {
                 const Json& f = args["forward"];
                 if (!f.is_array() || f.size() != 3 || !f[0].is_number() || !f[1].is_number() || !f[2].is_number()) throw ToolError("\"forward\" must be [x, y, z]");
                 forward = Vec3(f[0].get<f32>(), f[1].get<f32>(), f[2].get<f32>());
             }
             const interact::FocusResult focus = system->Focus(interactor, t->position, forward);
             if (focus.target.IsNull()) return {{"target", nullptr}};
             return {{"target", EntityKey(game.GetWorld(), focus.target)}, {"distance", focus.distance}, {"prompt", focus.prompt}};
         }});

    server.AddTool({"game_set_interactable", "Enable or disable an interactable in the running game, and/or reset a spent one-shot or cooling one so it can be used again.",
                    Schema({{"target", {{"type", "string"}}}, {"enabled", {{"type", "boolean"}}}, {"reset", {{"type", "boolean"}}}}, {"target"}),
                    [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        interact::InteractionSystem* system = interact::InteractionSystem::Active();
                        if (system == nullptr) throw ToolError("This game has no interaction system");
                        Entity target = FindEntity(game, RequireString(args, "target"));
                        if (!args.contains("enabled") && !args.contains("reset")) throw ToolError("Give \"enabled\" and/or \"reset\"");
                        if (args.contains("enabled")) {
                            if (!args["enabled"].is_boolean() || !system->SetEnabled(target, args["enabled"].get<bool>())) throw ToolError("The target is not an interactable (or \"enabled\" is not a boolean)");
                        }
                        if (args.contains("reset") && args["reset"].is_boolean() && args["reset"].get<bool>() && !system->Reset(target)) throw ToolError("The target is not an interactable");
                        const interact::Interactable* i = game.GetWorld().GetComponent<interact::Interactable>(target);
                        return {{"enabled", i->enabled}, {"used", i->used}, {"remaining", i->remaining}};
                    }});

#endif // AETHER_MCP_INTERACTION
    server.AddTool({"game_destroy_entity", "Destroy an entity in the running game (its OnDestroy runs).",
                    Schema({{"entity", {{"type", "string"}}}}, {"entity"}), [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        Entity e = RequireEntity(game, args);
                        game.GetLifecycle().Destroy(e);
                        return {{"destroyed", args["entity"]}};
                    }});
}

} // namespace aether::mcp
