#include "game_tools.h"

#include "aether/platform/window.h"

#include "screenshot.h"

#include "aether/audio/audio_system.h"
#include "aether/core/log.h"
#include "aether/sequencer/sequence_system.h"
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
#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/viewport.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
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

// Merges `values` (a partial object of fields) onto the component `id` of `e`, which must have it. Returns the load warnings.
std::vector<std::string> ApplyComponentValues(World& world, Entity e, ComponentId id, const Json& values) {
    if (!values.is_object()) throw ToolError("The values for " + ComponentName(id) + " must be an object of fields");
    void* data = world.GetComponentRaw(e, id);
    if (data == nullptr) throw ToolError("Entity has no " + ComponentName(id));
    const ComponentInfo& info = GetComponentInfo(id);
    Json merged = reflect::ToJson(*info.reflected, data);
    merged.merge_patch(values);
    reflect::LoadReport report;
    if (!reflect::FromJson(*info.reflected, data, merged, &report)) throw ToolError("Value does not match the shape of " + ComponentName(id));
    return report.warnings;
}

const char* VisibilityName(ui::Visibility v) {
    switch (v) {
    case ui::Visibility::Visible: return "Visible";
    case ui::Visibility::Hidden: return "Hidden";
    case ui::Visibility::Collapsed: return "Collapsed";
    case ui::Visibility::HitTestInvisible: return "HitTestInvisible";
    case ui::Visibility::SelfHitTestInvisible: return "SelfHitTestInvisible";
    }
    return "Visible";
}

// One widget and what it shows, then its children.
Json UiWidgetJson(const ui::Widget& w, usize& budget) {
    const ui::Rect& g = w.Geometry();
    Json j = {{"type", w.TypeName()}, {"rect", Json::array({g.x, g.y, g.w, g.h})}, {"visibility", VisibilityName(w.visibility)}, {"enabled", w.IsEnabled()}};
    if (!w.name.empty()) j["name"] = w.name;
    if (const auto* text = dynamic_cast<const ui::Text*>(&w)) j["text"] = text->text;
    if (const auto* slider = dynamic_cast<const ui::Slider*>(&w)) j["value"] = slider->value;
    if (const auto* toggle = dynamic_cast<const ui::Toggle*>(&w)) j["checked"] = toggle->checked;
    if (const auto* bar = dynamic_cast<const ui::ProgressBar*>(&w)) j["percent"] = bar->percent;
    if (const auto* input = dynamic_cast<const ui::TextInput*>(&w)) j["text"] = input->text;
    if (w.Interactive()) j["interactive"] = true;
    if (w.ChildCount() > 0) {
        Json children = Json::array();
        for (usize i = 0; i < w.ChildCount(); ++i) {
            if (budget == 0) {
                j["truncated"] = true;
                break;
            }
            --budget;
            children.push_back(UiWidgetJson(*w.Child(i), budget));
        }
        j["children"] = std::move(children);
    }
    return j;
}

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
        {"game_spawn_entity",
         "Create a new entity in the running game with the components named in `components` (component name -> fields to set, {} for defaults). "
         "A Transform is always added. Gets a GUID; scripts, physics and the other systems pick it up on the next frame. Not undoable; gone when "
         "the game is reloaded.",
         Schema({{"components", {{"type", "object"}, {"description", "e.g. {\"Transform\": {\"position\": [0,1,0]}, \"ModelRenderer\": {...}}"}}},
                 {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Tags to give it"}}}},
                {"components"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["components"].is_object()) throw ToolError("\"components\" must be an object of component name -> fields");
             std::vector<std::pair<ComponentId, Json>> wanted;
             ComponentMask mask;
             mask.set(GetComponentId<Transform>());
             for (const auto& [name, values] : args["components"].items()) {
                 const ComponentId id = RequireComponent(name);
                 mask.set(id);
                 wanted.emplace_back(id, values);
             }
             if (args.contains("tags")) {
                 if (!args["tags"].is_array()) throw ToolError("\"tags\" must be an array of strings");
                 mask.set(GetComponentId<Tags>());
             }
             const Entity e = world.CreateEntityRaw(mask);
             EnsureGuid(world, e, &game.Guids());
             std::vector<std::string> warnings;
             try {
                 for (const auto& [id, values] : wanted) {
                     for (const std::string& w : ApplyComponentValues(world, e, id, values)) warnings.push_back(w);
                 }
                 if (args.contains("tags")) {
                     Tags* tags = world.GetComponent<Tags>(e);
                     for (const Json& tag : args["tags"]) {
                         if (!tag.is_string()) throw ToolError("\"tags\" must be an array of strings");
                         tags->names.push_back(tag.get<std::string>());
                     }
                 }
             } catch (...) {
                 game.GetLifecycle().Destroy(e); // a half-made entity is not left behind
                 throw;
             }
             Json out = EntityJson(world, e);
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool(
        {"game_add_component",
         "Add a component to a running entity (default values, then `values` merged on); if it already has one, `values` are merged onto it. Not undoable.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index"}}},
                 {"component", {{"type", "string"}}},
                 {"values", {{"type", "object"}, {"description", "Fields to set (optional)"}}}},
                {"entity", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             const Entity e = RequireEntity(game, args);
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             const bool had = world.HasComponentRaw(e, id);
             if (!had) world.AddComponentRaw(e, id);
             std::vector<std::string> warnings;
             if (args.contains("values")) warnings = ApplyComponentValues(world, e, id, args["values"]);
             Json out = EntityJson(world, e);
             out["added"] = !had;
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool(
        {"game_remove_component", "Remove a component from a running entity. Not undoable.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index"}}}, {"component", {{"type", "string"}}}}, {"entity", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             const Entity e = RequireEntity(game, args);
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             if (!world.HasComponentRaw(e, id)) throw ToolError("Entity has no " + ComponentName(id));
             if (id == GetComponentId<Transform>() || id == GetComponentId<IdComponent>()) throw ToolError(ComponentName(id) + " cannot be removed from a running entity");
             world.RemoveComponentRaw(e, id);
             return EntityJson(world, e);
         }});

    server.AddTool(
        {"game_ui",
         "The hosted game's UI as it is now: each layer's widget tree with type, name, rectangle (layout units), visibility, enabled, and what it "
         "shows (Text and TextInput text, Slider value, Toggle checked, ProgressBar percent). Interactive widgets are marked. Optionally just one widget by name.",
         Schema({{"widget", {{"type", "string"}, {"description", "Only this widget (and its children), by name"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             ui::Viewport* viewport = game.Ui();
             if (viewport == nullptr) return {{"layers", Json::array()}, {"note", "the game has no UI yet"}};
             usize budget = 2000;
             if (args.contains("widget")) {
                 if (!args["widget"].is_string()) throw ToolError("\"widget\" must be a string");
                 const ui::Widget* found = viewport->Find(args["widget"].get<std::string>());
                 if (found == nullptr) throw ToolError("No widget named \"" + args["widget"].get<std::string>() + "\" in the game's UI");
                 return {{"widget", UiWidgetJson(*found, budget)}};
             }
             Json layers = Json::array();
             for (usize i = 0; i < viewport->LayerCount(); ++i) {
                 if (const ui::Widget* root = viewport->Layer(i)) layers.push_back(UiWidgetJson(*root, budget));
             }
             const ui::Vec2 size = viewport->Size();
             return {{"size", Json::array({size.x, size.y})}, {"layers", layers}};
         }});

    server.AddTool(
        {"game_ui_click",
         "Click a widget of the hosted game's UI by name, the way a player would: the pointer moves to the middle of the widget, presses and "
         "lets go, with a frame between each. Returns where it clicked. Fails for a widget that is hidden, collapsed or disabled.",
         Schema({{"widget", {{"type", "string"}, {"description", "The widget's name"}}}}, {"widget"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             const std::string name = RequireString(args, "widget");
             ui::Viewport* viewport = game.Ui();
             ui::Widget* widget = viewport != nullptr ? viewport->Find(name) : nullptr;
             if (widget == nullptr) throw ToolError("No widget named \"" + name + "\" in the game's UI");
             if (widget->visibility != ui::Visibility::Visible && widget->visibility != ui::Visibility::SelfHitTestInvisible) throw ToolError("\"" + name + "\" is not visible");
             if (!widget->IsEnabled()) throw ToolError("\"" + name + "\" is disabled");
             const ui::Rect g = widget->Geometry();
             if (g.Empty()) throw ToolError("\"" + name + "\" has no size yet (step the game a frame so the UI is laid out)");
             // Pointers are in pixels, geometry in layout units.
             const ui::Vec2 layout = viewport->LayoutSize();
             const ui::Vec2 pixels = viewport->Size();
             const f32 sx = layout.x > 0.0f ? pixels.x / layout.x : 1.0f, sy = layout.y > 0.0f ? pixels.y / layout.y : 1.0f;
             const f32 x = (g.x + g.w * 0.5f) * sx, y = (g.y + g.h * 0.5f) * sy;
             const f32 dt = 1.0f / static_cast<f32>(host->package->Manifest().fixed_timestep_hz);
             input::InputState& in = game.Input();
             in.SetPointer(0, x, y, false, true);
             game.Tick(dt); // hover
             in.SetPointer(0, x, y, true, true);
             in.SetButton(input::Key::MouseLeft, true);
             game.Tick(dt); // press
             in.SetPointer(0, x, y, false, true);
             in.SetButton(input::Key::MouseLeft, false);
             game.Tick(dt); // release: the click
             game.Tick(dt); // scripts see it a frame later
             return {{"widget", name}, {"at", Json::array({x, y})}, {"type", widget->TypeName()}};
         }});

    server.AddTool(
        {"log_recent",
         "The engine's recent log lines (the last 512), oldest first: engine and system messages, and what the game's scripts and Blueprints "
         "print() (categories \"Script\" and \"Blueprint\"). Filter by minimum level, category, text, or time (seconds since the logger started).",
         Schema({{"level", {{"type", "string"}, {"enum", Json::array({"Trace", "Info", "Warn", "Error", "Fatal"})}, {"description", "Minimum level, any case (default Trace: everything); lines report levels in upper case"}}},
                 {"category", {{"type", "string"}, {"description", "Only this category, e.g. \"Script\""}}},
                 {"contains", {{"type", "string"}, {"description", "Only lines whose message contains this text"}}},
                 {"since", {{"type", "number"}, {"description", "Only lines at or after this logger time (the \"time\" of a line you saw)"}}},
                 {"limit", {{"type", "integer"}, {"description", "Most recent N matching lines, 1-512 (default 100)"}}}}),
         [](const Json& args) -> Json {
             LogLevel minimum = LogLevel::Trace;
             if (args.contains("level")) {
                 const std::string name = args["level"].is_string() ? args["level"].get<std::string>() : "";
                 bool found = false;
                 for (const LogLevel l : {LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Fatal}) {
                     const std::string level_name = LogLevelName(l); // upper case ("INFO"); a request may use any case
                     if (name.size() == level_name.size() &&
                         std::equal(name.begin(), name.end(), level_name.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); })) {
                         minimum = l, found = true;
                     }
                 }
                 if (!found) throw ToolError("\"level\" must be Trace, Info, Warn, Error or Fatal");
             }
             const std::string category = args.value("category", std::string());
             const std::string contains = args.value("contains", std::string());
             const f64 since = args.contains("since") && args["since"].is_number() ? args["since"].get<f64>() : -1.0;
             i64 limit = 100;
             if (args.contains("limit")) {
                 if (!args["limit"].is_number_integer()) throw ToolError("\"limit\" must be an integer");
                 limit = args["limit"].get<i64>();
             }
             if (limit < 1 || limit > 512) throw ToolError("\"limit\" must be between 1 and 512");
             Json lines = Json::array();
             for (const LogLine& line : Logger::Instance().Recent()) {
                 if (static_cast<int>(line.level) < static_cast<int>(minimum)) continue;
                 if (!category.empty() && line.category != category) continue;
                 if (!contains.empty() && line.message.find(contains) == std::string::npos) continue;
                 if (since >= 0.0 && line.time < since) continue;
                 lines.push_back({{"time", line.time}, {"level", LogLevelName(line.level)}, {"category", line.category}, {"message", line.message}});
             }
             while (static_cast<i64>(lines.size()) > limit) lines.erase(lines.begin());
             return {{"count", lines.size()}, {"lines", lines}};
         }});

    server.AddTool(
        {"log_clear", "Forget the recent log lines (log_recent starts empty).", Schema(Json::object()), [](const Json&) -> Json {
             Logger::Instance().ClearRecent();
             return {{"cleared", true}};
         }});

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
                 {"path", {{"type", "string"}, {"minLength", 1}, {"description", "Where to save the PNG (default: a temp folder)"}}},
                 {"backend", {{"type", "string"}, {"enum", Json::array({"d3d12", "vulkan"})}, {"description", "d3d12 or vulkan"}}},
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
             if (args.contains("backend") && !args["backend"].is_string()) throw ToolError("\"backend\" must be a string");
             const std::string backend = args.contains("backend") ? args["backend"].get<std::string>() : "";
             if (args.contains("backend") && backend != "d3d12" && backend != "vulkan") throw ToolError("\"backend\" must be \"d3d12\" or \"vulkan\"");
             if (args.contains("path") && !args["path"].is_string()) throw ToolError("\"path\" must be a string");
             if (args.contains("path") && args["path"].get<std::string>().empty()) throw ToolError("\"path\" must not be empty");
             if (args.contains("inline") && !args["inline"].is_boolean()) throw ToolError("\"inline\" must be a boolean");
             if (!host->shots) host->shots = std::make_unique<GameScreenshotter>();
             GameScreenshotter::Result r = host->shots->Capture(game, *host->package, width, height, backend);
             if (!r.ok) throw ToolError("Screenshot failed: " + r.error);

             stdfs::path path;
             if (args.contains("path")) {
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
             if (!args.contains("inline") || args["inline"].get<bool>()) {
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
         "`clear` releases everything first. Pointers (for the game's on-screen UI, in window pixels): `mouse` {x, y, down} puts the mouse "
         "cursor there (and holds or releases its left button), in the UI's pixel space (the window or screenshot size once the game has been drawn, "
         "1920 x 1080 before), and `touches` is a list of {id, x, y, phase} with phase \"down\", \"move\" "
         "or \"up\" for fingers (up to 9 at once; the first finger also holds MouseLeft). The result lists the pointers that are active.",
         Schema({{"set", {{"type", "object"}, {"description", "{ \"W\": 1, \"Space\": 0 }"}}},
                 {"mouse_delta", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                 {"mouse", {{"type", "object"}, {"description", "{ \"x\": 100, \"y\": 80, \"down\": true } (down must be boolean when supplied)"}}},
                 {"touches", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "[{ \"id\": 1, \"x\": 100, \"y\": 80, \"phase\": \"down\" }]"}}},
                 {"clear", {{"type", "boolean"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             // Validate the complete request before changing held keys, pointer state, or buttons.
             if (args.contains("clear") && !args["clear"].is_boolean()) throw ToolError("\"clear\" must be a boolean");
             if (args.contains("set")) {
                 if (!args["set"].is_object()) throw ToolError("\"set\" must be an object of key names to values");
                 for (const auto& [name, value] : args["set"].items()) {
                     if (input::KeyFromName(name) == input::Key::None) throw ToolError("No key named \"" + name + "\"");
                     if (!value.is_number() && !value.is_boolean()) throw ToolError("Value for \"" + name + "\" must be a number");
                     const f64 v = value.is_boolean() ? (value.get<bool>() ? 1.0 : 0.0) : value.get<f64>();
                     if (v < -1.0 || v > 1.0) throw ToolError("Value for \"" + name + "\" must be between -1 and 1");
                 }
             }
             if (args.contains("mouse_delta")) {
                 const Json& d = args["mouse_delta"];
                 if (!d.is_array() || d.size() != 2 || !d[0].is_number() || !d[1].is_number()) {
                     throw ToolError("\"mouse_delta\" must be [dx, dy]");
                 }
                 if (std::abs(d[0].get<f64>()) > std::numeric_limits<f32>::max() ||
                     std::abs(d[1].get<f64>()) > std::numeric_limits<f32>::max()) {
                     throw ToolError("\"mouse_delta\" values are too large");
                 }
             }
             if (args.contains("mouse")) {
                 const Json& m = args["mouse"];
                 if (!m.is_object() || !m.contains("x") || !m.contains("y") || !m["x"].is_number() || !m["y"].is_number()) {
                     throw ToolError("\"mouse\" must be { x, y, down? } with numbers");
                 }
                 if (m.contains("down") && !m["down"].is_boolean()) throw ToolError("\"mouse.down\" must be a boolean");
                 if (std::abs(m["x"].get<f64>()) > std::numeric_limits<f32>::max() ||
                     std::abs(m["y"].get<f64>()) > std::numeric_limits<f32>::max()) {
                     throw ToolError("\"mouse\" coordinates are too large");
                 }
             }
             if (args.contains("touches")) {
                 if (!args["touches"].is_array()) throw ToolError("\"touches\" must be a list of { id, x, y, phase }");
                 for (const Json& t : args["touches"]) {
                     if (!t.is_object() || !t.contains("id") || !t["id"].is_number_integer() || !t.contains("x") || !t["x"].is_number() ||
                         !t.contains("y") || !t["y"].is_number() || !t.contains("phase") || !t["phase"].is_string()) {
                         throw ToolError("Each touch is { id, x, y, phase } with phase \"down\", \"move\" or \"up\"");
                     }
                     const f64 id = t["id"].get<f64>();
                     if (id < 0.0 || id > std::numeric_limits<u32>::max()) throw ToolError("A touch id must be between 0 and 4294967295");
                     const std::string phase = t["phase"].get<std::string>();
                     if (phase != "down" && phase != "move" && phase != "up") throw ToolError("A touch's phase is \"down\", \"move\" or \"up\", not \"" + phase + "\"");
                     if (std::abs(t["x"].get<f64>()) > std::numeric_limits<f32>::max() ||
                         std::abs(t["y"].get<f64>()) > std::numeric_limits<f32>::max()) {
                         throw ToolError("Touch coordinates are too large");
                     }
                 }
             }
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
             std::vector<WindowEvent> events;
             if (args.contains("mouse")) {
                 const Json& m = args["mouse"];
                 if (!m.is_object() || !m.contains("x") || !m.contains("y") || !m["x"].is_number() || !m["y"].is_number()) {
                     throw ToolError("\"mouse\" must be { x, y, down? } with numbers");
                 }
                 WindowEvent move;
                 move.type = WindowEventType::MouseMove;
                 move.x = m["x"].get<f32>(), move.y = m["y"].get<f32>();
                 events.push_back(move);
                 if (m.contains("down")) {
                     WindowEvent button;
                     button.type = WindowEventType::Key, button.key = input::Key::MouseLeft, button.down = m["down"].is_boolean() && m["down"].get<bool>();
                     events.push_back(button);
                 }
             }
             if (args.contains("touches")) {
                 if (!args["touches"].is_array()) throw ToolError("\"touches\" must be a list of { id, x, y, phase }");
                 for (const Json& t : args["touches"]) {
                     if (!t.is_object() || !t.contains("id") || !t["id"].is_number_integer() || !t.contains("x") || !t["x"].is_number() ||
                         !t.contains("y") || !t["y"].is_number() || !t.contains("phase") || !t["phase"].is_string()) {
                         throw ToolError("Each touch is { id, x, y, phase } with phase \"down\", \"move\" or \"up\"");
                     }
                     const std::string phase = t["phase"].get<std::string>();
                     if (phase != "down" && phase != "move" && phase != "up") throw ToolError("A touch's phase is \"down\", \"move\" or \"up\", not \"" + phase + "\"");
                     WindowEvent e;
                     e.type = phase == "move" ? WindowEventType::TouchMove : WindowEventType::Touch;
                     e.down = phase == "down";
                     e.pointer = static_cast<u32>(t["id"].get<i64>());
                     e.x = t["x"].get<f32>(), e.y = t["y"].get<f32>();
                     events.push_back(e);
                 }
             }
             if (!events.empty()) ApplyWindowEvents(events, game.Input());
             Json pointers = Json::array();
             for (usize i = 0; i < input::InputState::kMaxPointers; ++i) {
                 const input::InputState::Pointer& p = game.Input().GetPointer(i);
                 if (p.active) pointers.push_back({{"slot", i}, {"x", p.x}, {"y", p.y}, {"down", p.down}});
             }
             return {{"applied", applied}, {"pointers", pointers}};
         }});

    server.AddTool(
        {"game_lifecycle",
         "Send the game a platform lifecycle event, as a phone's operating system would: \"suspend\" (the app goes to the background: the game stops "
         "advancing, audio output stops and queued saves are written), \"resume\" (audio restarts; the next frame ignores the time that passed) or "
         "\"memory_pressure\" with a `level` of \"moderate\" or \"critical\" (counted; the game has no cache to drop safely yet). Returns whether the game is "
         "suspended and how many memory-pressure events it has seen.",
         Schema({{"event", {{"type", "string"}, {"enum", Json::array({"suspend", "resume", "memory_pressure"})}}},
                 {"level", {{"type", "string"}, {"enum", Json::array({"moderate", "critical"})}, {"description", "Only with memory_pressure (default: moderate)"}}}},
                {"event"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             const std::string event = RequireString(args, "event");
             if (event != "suspend" && event != "resume" && event != "memory_pressure") {
                 throw ToolError("event is \"suspend\", \"resume\" or \"memory_pressure\"");
             }
             if (args.contains("level") && !args["level"].is_string()) throw ToolError("\"level\" must be a string");
             if (event != "memory_pressure" && args.contains("level")) {
                 throw ToolError("\"level\" is only valid with the \"memory_pressure\" event");
             }
             platform::LifecycleMessage message;
             if (event == "suspend") message.event = platform::LifecycleEvent::Suspend;
             else if (event == "resume") message.event = platform::LifecycleEvent::Resume;
             else if (event == "memory_pressure") {
                 message.event = platform::LifecycleEvent::MemoryPressure;
                 const std::string level = args.contains("level") ? args["level"].get<std::string>() : "moderate";
                 if (level != "moderate" && level != "critical") throw ToolError("level is \"moderate\" or \"critical\"");
                 message.level = level == "critical" ? platform::MemoryLevel::Critical : platform::MemoryLevel::Moderate;
             }
             game.Platform().Dispatch(message);
             return {{"suspended", game.Suspended()}, {"memory_pressure_events", game.MemoryPressureEvents()}};
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
         "Optionally only those with a component or tag. Results can be paged with a bounded limit and offset.",
         Schema({{"component", {{"type", "string"}}}, {"tag", {{"type", "string"}}},
                 {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Page size, 1-1000 (default 200)"}}},
                 {"offset", {{"type", "integer"}, {"minimum", 0}, {"description", "Number of matching entities to skip"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             ComponentId filter = kInvalidComponentId;
             if (args.contains("component")) filter = RequireComponent(RequireString(args, "component"));
             usize limit = 200;
             if (args.contains("limit")) {
                 if (!args["limit"].is_number_integer()) throw ToolError("\"limit\" must be an integer");
                 const f64 requested = args["limit"].get<f64>();
                 if (requested < 1 || requested > 1000) throw ToolError("\"limit\" must be between 1 and 1000");
                 limit = static_cast<usize>(requested);
             }
             usize offset = 0;
             if (args.contains("offset")) {
                 if (!args["offset"].is_number_integer()) throw ToolError("\"offset\" must be a non-negative integer");
                 if (args["offset"].is_number_unsigned()) {
                     const u64 requested = args["offset"].get<u64>();
                     if (requested > std::numeric_limits<usize>::max()) throw ToolError("\"offset\" is too large");
                     offset = static_cast<usize>(requested);
                 } else {
                     const i64 requested = args["offset"].get<i64>();
                     if (requested < 0) throw ToolError("\"offset\" must be a non-negative integer");
                     offset = static_cast<usize>(requested);
                 }
             }
             std::vector<Entity> candidates;
             if (args.contains("tag")) candidates = FindEntitiesWithTag(world, RequireString(args, "tag"));
             else candidates = AllEntities(world);
             Json list = Json::array();
             usize total = 0;
             for (Entity e : candidates) {
                 if (filter != kInvalidComponentId && !world.HasComponentRaw(e, filter)) continue;
                 ++total;
                 if (total <= offset || list.size() >= limit) continue;
                 Json names = Json::array();
                 for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
                     if (world.HasComponentRaw(e, id)) names.push_back(ComponentName(id));
                 }
                 Json tags = Json::array();
                 if (const Tags* component_tags = world.GetComponent<Tags>(e)) {
                     for (const std::string& tag : component_tags->names) tags.push_back(tag);
                 }
                 Json row = {{"entity", EntityKey(world, e)}, {"components", names}, {"tags", tags}};
                 if (const EntityName* n = world.GetComponent<EntityName>(e)) row["name"] = n->value;
                 list.push_back(row);
             }
             const usize remaining = total > offset ? total - offset : 0;
             return {{"total", total}, {"offset", offset}, {"limit", limit}, {"has_more", list.size() < remaining}, {"entities", list}};
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
    // -- Audio (the running game's mixer, cue player and AudioSource components) ---------------------------------
    server.AddTool({"game_audio_state",
                    "The running game's audio: voices and cues playing, the listener, reverb, every bus (volume, muted) and the cue problems found "
                    "so far (a missing or invalid cue is reported once).",
                    Schema(Json::object()), [host](const Json&) -> Json {
                        Game& game = host->Require();
                        audio::AudioSystem* audio = game.AudioSystem();
                        if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
                        audio::Mixer& mixer = audio->GetMixer();
                        Json buses = Json::array();
                        for (usize b = 0; b < mixer.BusCount(); ++b) {
                            buses.push_back({{"name", mixer.BusName(static_cast<audio::BusId>(b))},
                                             {"volume_db", mixer.BusVolume(static_cast<audio::BusId>(b))},
                                             {"muted", mixer.BusMuted(static_cast<audio::BusId>(b))}});
                        }
                        Json out = {{"voices", mixer.VoiceCount()},
                                    {"cues_playing", audio->Player().ActiveCount()},
                                    {"sample_rate", mixer.SampleRate()},
                                    {"buses", buses},
                                    {"problems", audio->Problems()}};
                        out["listener"] = audio->ListenerEntity().IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), audio->ListenerEntity()));
                        out["reverb"] = {{"active", audio->HasReverb()}, {"strength", audio->ReverbStrength()}, {"wet", audio->ReverbWet()}, {"room_size", audio->ReverbRoomSize()}};
                        return out;
                    }});

    server.AddTool(
        {"game_play_sound",
         "Play a sound cue in the running game, once: in 2D, or at a `location` [x,y,z] (a spatial cue is heard from there). Returns whether "
         "anything started (false: the cue doesn't exist or plays nothing). Step the game to let it run; game_audio_state shows it playing.",
         Schema({{"cue", {{"type", "string"}, {"description", "Cue asset path, e.g. Audio/hurt.acue"}}},
                 {"location", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                 {"volume_db", {{"type", "number"}}},
                 {"pitch", {{"type", "number"}}}},
                {"cue"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             audio::AudioSystem* audio = game.AudioSystem();
             if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
             const std::string cue = RequireString(args, "cue");
             const f32 volume = args.contains("volume_db") && args["volume_db"].is_number() ? args["volume_db"].get<f32>() : 0.0f;
             const f32 pitch = args.contains("pitch") && args["pitch"].is_number() ? args["pitch"].get<f32>() : 1.0f;
             if (!(pitch > 0.0f)) throw ToolError("\"pitch\" must be above 0");
             audio::CueHandle handle = 0;
             if (args.contains("location")) {
                 const Json& l = args["location"];
                 if (!l.is_array() || l.size() != 3 || !l[0].is_number() || !l[1].is_number() || !l[2].is_number()) throw ToolError("\"location\" must be [x, y, z]");
                 handle = audio->PlaySoundAtLocation(cue, Vec3(l[0].get<f32>(), l[1].get<f32>(), l[2].get<f32>()), volume, pitch);
             } else {
                 handle = audio->PlaySound2D(cue, volume, pitch);
             }
             return {{"played", handle != 0}, {"cue", cue}, {"problems", audio->Problems()}};
         }});

    server.AddTool({"game_set_bus",
                    "Change a mixer bus in the running game: its volume (dB, optionally faded over seconds) and/or mute. Buses: Master, Music, SFX, UI, Voice "
                    "(game_audio_state lists them).",
                    Schema({{"bus", {{"type", "string"}}}, {"volume_db", {{"type", "number"}}}, {"fade_seconds", {{"type", "number"}}}, {"muted", {{"type", "boolean"}}}}, {"bus"}),
                    [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        audio::AudioSystem* audio = game.AudioSystem();
                        if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
                        const std::string bus = RequireString(args, "bus");
                        if (!args.contains("volume_db") && !args.contains("muted")) throw ToolError("Give \"volume_db\" and/or \"muted\"");
                        const audio::BusId id = audio->GetMixer().FindBus(bus);
                        if (id == audio::Mixer::kInvalidBus) throw ToolError("No bus \"" + bus + "\"");
                        if (args.contains("volume_db")) {
                            if (!args["volume_db"].is_number()) throw ToolError("\"volume_db\" must be a number");
                            const f32 fade = args.contains("fade_seconds") && args["fade_seconds"].is_number() ? std::max(0.0f, args["fade_seconds"].get<f32>()) : 0.0f;
                            audio->SetBusVolume(bus, args["volume_db"].get<f32>(), fade);
                        }
                        if (args.contains("muted")) {
                            if (!args["muted"].is_boolean()) throw ToolError("\"muted\" must be true or false");
                            audio->GetMixer().SetBusMuted(id, args["muted"].get<bool>());
                        }
                        return {{"bus", bus}, {"volume_db", audio->GetMixer().BusVolume(id)}, {"muted", audio->GetMixer().BusMuted(id)}};
                    }});

    server.AddTool({"game_stop_sounds", "Stop every sound playing in the running game.", Schema(Json::object()), [host](const Json&) -> Json {
                        Game& game = host->Require();
                        audio::AudioSystem* audio = game.AudioSystem();
                        if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
                        audio->StopAll();
                        return {{"voices", audio->GetMixer().VoiceCount()}};
                    }});

    server.AddTool(
        {"game_audio_source",
         "Control an entity's AudioSource in the running game: play, stop, fade_in / fade_out (value = seconds), set_volume (value = dB) or set_cue "
         "(text = cue path). The request is applied by the next frame's audio update, so step the game; the reply shows the component's state.",
         Schema({{"entity", {{"type", "string"}}},
                 {"action", {{"type", "string"}, {"description", "play | stop | fade_in | fade_out | set_volume | set_cue"}}},
                 {"value", {{"type", "number"}}},
                 {"text", {{"type", "string"}}}},
                {"entity", "action"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             Entity e = RequireEntity(game, args);
             AudioSource* source = game.GetWorld().GetComponent<AudioSource>(e);
             if (source == nullptr) throw ToolError("The entity has no AudioSource");
             const std::string action = RequireString(args, "action");
             const f32 value = args.contains("value") && args["value"].is_number() ? args["value"].get<f32>() : 0.0f;
             if (action == "play") source->Play();
             else if (action == "stop") source->Stop();
             else if (action == "fade_in") source->FadeIn(value);
             else if (action == "fade_out") source->FadeOut(value);
             else if (action == "set_volume") source->SetVolume(value);
             else if (action == "set_cue") source->SetCue(RequireString(args, "text"));
             else throw ToolError("\"action\" must be play, stop, fade_in, fade_out, set_volume or set_cue");
             return {{"action", action}, {"cue", source->cue}, {"playing", source->playing}, {"volume_db", source->volume_db}, {"queued_commands", source->commands.size()}};
         }});

    // -- Sequences (the game's SequenceSystem and SequenceComponent) ----------------------------------------------
    server.AddTool(
        {"game_sequences",
         "The running game's sequences: every entity with a SequenceComponent (sequence, playing, time, loop, rate), the screen fade the sequences ask for, and the "
         "problems the sequence system found (a missing sequence, a track whose entity or field could not be resolved).",
         Schema(Json::object()), [host](const Json&) -> Json {
             Game& game = host->Require();
             seq::SequenceSystem* system = seq::SequenceSystem::Active();
             Json list = Json::array();
             game.GetWorld().ForEach<SequenceComponent>([&](SequenceComponent& c) {
                 list.push_back({{"sequence", c.sequence}, {"playing", c.playing}, {"time", c.time}, {"loop", c.loop}, {"rate", c.rate}});
             });
             Json out = {{"sequences", list}, {"has_sequence_system", system != nullptr}};
             if (system != nullptr) {
                 out["fade"] = {{"amount", system->Fade().amount}, {"color", Json::array({system->Fade().color.x, system->Fade().color.y, system->Fade().color.z, system->Fade().color.w})}};
                 out["problems"] = system->Problems();
             }
             return out;
         }});

    server.AddTool({"game_play_sequence",
                    "Play a level sequence in the running game, like the Blueprint node Play Sequence: it gets its own entity, which is removed when it ends. `sequence` is the "
                    "asset path (Sequences/Intro.asequence). Step the game to let it run; the entity comes back so game_sequence_control can drive it.",
                    Schema({{"sequence", {{"type", "string"}}}, {"loop", {{"type", "boolean"}}}}, {"sequence"}), [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        seq::SequenceSystem* system = seq::SequenceSystem::Active();
                        if (system == nullptr) throw ToolError("This game has no sequence system (load a scene first)");
                        const bool loop = args.contains("loop") && args["loop"].is_boolean() && args["loop"].get<bool>();
                        const Entity e = system->PlaySequence(RequireString(args, "sequence"), loop);
                        if (e.IsNull()) throw ToolError("Could not play \"" + RequireString(args, "sequence") + "\": it is not a sequence in the game (see game_sequences problems)");
                        return {{"entity", EntityKey(game.GetWorld(), e)}, {"sequence", RequireString(args, "sequence")}, {"loop", loop}, {"problems", system->Problems()}};
                    }});

    server.AddTool(
        {"game_sequence_control",
         "Control one SequenceComponent in the running game: play, pause, stop, skip (finish from here without firing the keys in between), set_time (value = seconds), set_rate "
         "(value; negative plays backward) or set_loop (value 1 or 0). Applied by the next frame's sequence update, so step the game.",
         Schema({{"entity", {{"type", "string"}}}, {"action", {{"type", "string"}, {"description", "play | pause | stop | skip | set_time | set_rate | set_loop"}}}, {"value", {{"type", "number"}}}},
                {"entity", "action"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             Entity e = RequireEntity(game, args);
             SequenceComponent* c = game.GetWorld().GetComponent<SequenceComponent>(e);
             if (c == nullptr) throw ToolError("The entity has no SequenceComponent");
             const std::string action = RequireString(args, "action");
             const f32 value = args.contains("value") && args["value"].is_number() ? args["value"].get<f32>() : 0.0f;
             if (action == "play") c->Play();
             else if (action == "pause") c->Pause();
             else if (action == "stop") c->Stop();
             else if (action == "skip") c->Skip();
             else if (action == "set_time") c->SetTime(value);
             else if (action == "set_rate") c->SetRate(value);
             else if (action == "set_loop") c->SetLoop(value != 0.0f);
             else throw ToolError("\"action\" must be play, pause, stop, skip, set_time, set_rate or set_loop");
             return {{"action", action}, {"sequence", c->sequence}, {"playing", c->playing}, {"time", c->time}, {"queued_commands", c->commands.size()}};
         }});

    server.AddTool({"game_stop_sequences", "Stop or skip every playing sequence in the running game (skip finishes each from where it is without firing the keys in between).",
                    Schema({{"skip", {{"type", "boolean"}, {"description", "Skip instead of stop"}}}}), [host](const Json& args) -> Json {
                        host->Require();
                        seq::SequenceSystem* system = seq::SequenceSystem::Active();
                        if (system == nullptr) throw ToolError("This game has no sequence system (load a scene first)");
                        const bool skip = args.contains("skip") && args["skip"].is_boolean() && args["skip"].get<bool>();
                        if (skip) system->SkipAll();
                        else system->StopAll();
                        return {{"done", skip ? "skipped" : "stopped"}};
                    }});

    server.AddTool({"game_destroy_entity", "Destroy an entity in the running game (its OnDestroy runs).",
                    Schema({{"entity", {{"type", "string"}}}}, {"entity"}), [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        Entity e = RequireEntity(game, args);
                        game.GetLifecycle().Destroy(e);
                        return {{"destroyed", args["entity"]}};
                    }});
}

} // namespace aether::mcp
