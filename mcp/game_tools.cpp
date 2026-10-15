#include "game_tools.h"

#include "aether/platform/window.h"

#include "screenshot.h"

#include "aether/audio/audio_system.h"
#include "aether/core/log.h"
#include "aether/sequencer/sequence_system.h"
#include "aether/gameplay/attribute_library.h"
#include "aether/gameplay/attribute_system.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/gameplay/ability_system.h"
#include "aether/gameplay/effect_system.h"
#include "aether/gameplay/gameplay_tag.h"
#if AETHER_MCP_INVENTORY
#include "aether/inventory/inventory_system.h"
#endif
#if AETHER_MCP_QUESTS
#include "aether/quests/quest_log.h"
#include "aether/quests/quest_system.h"
#endif
#include "aether/input/keys.h"
#if AETHER_MCP_INTERACTION
#include "aether/interaction/interaction_system.h"
#endif
#include "aether/player/game.h"
#include "aether/sprite2d/physics2d.h"
#if AETHER_MCP_PHYSICS
#include "aether/physics/physics_scene.h"
#endif
#include "aether/player/user_paths.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"
#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/viewport.h"
#include "aether/ui/widget_system.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_set>

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
        std::optional<u32> generation;
        try {
            const std::string value = key.substr(1);
            const usize separator = value.find(':');
            const std::string index_text = value.substr(0, separator);
            usize consumed = 0;
            index = std::stoull(index_text, &consumed);
            if (consumed != index_text.size()) throw std::invalid_argument("trailing index data");
            if (separator != std::string::npos) {
                const std::string generation_text = value.substr(separator + 1);
                consumed = 0;
                const unsigned long parsed_generation = std::stoul(generation_text, &consumed);
                if (generation_text.empty() || consumed != generation_text.size() || parsed_generation > std::numeric_limits<u32>::max()) {
                    throw std::invalid_argument("invalid entity generation");
                }
                generation = static_cast<u32>(parsed_generation);
            }
        } catch (...) {
            throw ToolError("Not a valid entity handle: " + key + " (expected #index or #index:generation)");
        }
        for (Entity e : AllEntities(world)) {
            if (e.index == index && (!generation.has_value() || e.generation == *generation)) return e;
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

Json EntityJson(Game& game, Entity e) {
    World& world = game.GetWorld();
    Json components = Json::object();
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (!world.HasComponentRaw(e, id)) continue;
        const ComponentInfo& info = GetComponentInfo(id);
        components[ComponentName(id)] = info.reflected != nullptr ? reflect::ToJson(*info.reflected, world.GetComponentRaw(e, id))
                                                                  : Json("<unreflected component>");
    }
    Json tags = Json::array();
    if (const Tags* entity_tags = world.GetComponent<Tags>(e)) {
        for (const std::string& tag : entity_tags->names) tags.push_back(tag);
    }
    const EntityName* entity_name = world.GetComponent<EntityName>(e);
    const Parent* parent_component = world.GetComponent<Parent>(e);
    const bool has_parent = parent_component != nullptr && !parent_component->parent.IsNull();
    const Active* active_component = world.GetComponent<Active>(e);
    const Vec3 world_position = WorldPosition(world, game.Guids(), e);
    const Json position = world.HasComponent<Transform>(e) && std::isfinite(world_position.x) && std::isfinite(world_position.y) &&
                                  std::isfinite(world_position.z)
                              ? Json::array({world_position.x, world_position.y, world_position.z})
                              : Json(nullptr);
    Json result = {{"entity", EntityKey(world, e)}, {"entity_index", e.index}, {"entity_generation", e.generation},
                   {"components", components}, {"tags", tags}, {"world_position", position},
                   {"active", active_component == nullptr || active_component->active},
                   {"active_in_hierarchy", IsActiveInHierarchy(world, game.Guids(), e)},
                   {"parent", has_parent ? Json(ToString(parent_component->parent)) : Json(nullptr)}};
    if (entity_name != nullptr) result["name"] = entity_name->value;
    return result;
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
    if (const auto* dropdown = dynamic_cast<const ui::Dropdown*>(&w)) {
        j["selected_index"] = dropdown->selected;
        j["options"] = dropdown->options;
        j["open"] = dropdown->IsOpen();
        j["selected_option"] = dropdown->selected >= 0 && static_cast<usize>(dropdown->selected) < dropdown->options.size()
            ? Json(dropdown->options[static_cast<usize>(dropdown->selected)]) : Json(nullptr);
    }
    if (const auto* stick = dynamic_cast<const ui::VirtualStick*>(&w)) {
        j["value"] = Json::array({stick->value.x, stick->value.y});
        j["active"] = stick->IsActive();
    }
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
             Json out = EntityJson(game, e);
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool(
        {"game_spawn_entities_many",
         "Create 1-64 live entities as one validated batch. Each entry supplies component fields and optional tags; Transform and GUID are managed automatically. If creation fails, entities created by this request are destroyed.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                                {"items", {{"type", "object"},
                                            {"properties", {{"components", {{"type", "object"}}},
                                                             {"tags", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}}},
                                            {"required", Json::array({"components"})}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 spawn specifications");
             }
             struct SpawnSpec {
                 ComponentMask mask;
                 std::vector<std::pair<ComponentId, Json>> components;
                 std::vector<std::string> tags;
                 bool has_tags = false;
             };
             std::vector<SpawnSpec> specs;
             specs.reserve(args["entities"].size());
             for (const Json& item : args["entities"]) {
                 if (!item.is_object() || !item.contains("components") || !item["components"].is_object()) {
                     throw ToolError("Every \"entities\" entry must contain an object \"components\"");
                 }
                 SpawnSpec spec;
                 spec.mask.set(GetComponentId<Transform>());
                 for (const auto& [name, values] : item["components"].items()) {
                     const ComponentId id = RequireComponent(name);
                     if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be spawned directly");
                     if (!values.is_object()) throw ToolError("Values for component \"" + name + "\" must be an object");
                     const ComponentInfo& info = GetComponentInfo(id);
                     reflect::Any candidate = reflect::Any::DefaultOf(*info.reflected);
                     if (!candidate.HasValue()) throw ToolError("Cannot default-construct " + name);
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, candidate.Data(), values, &report)) {
                         throw ToolError("Invalid values for component \"" + name + "\"");
                     }
                     spec.mask.set(id);
                     spec.components.emplace_back(id, values);
                 }
                 if (item.contains("tags")) {
                     if (!item["tags"].is_array() || item["tags"].size() > 64) throw ToolError("\"tags\" must be an array of at most 64 strings");
                     if (spec.mask.test(GetComponentId<Tags>())) {
                         throw ToolError("Supply tags either through the Tags component or the \"tags\" field, not both");
                     }
                     spec.has_tags = true;
                     spec.mask.set(GetComponentId<Tags>());
                     for (const Json& tag : item["tags"]) {
                         if (!tag.is_string() || tag.get<std::string>().empty()) throw ToolError("Every \"tags\" entry must be a non-empty string");
                         const std::string value = tag.get<std::string>();
                         if (std::find(spec.tags.begin(), spec.tags.end(), value) != spec.tags.end()) throw ToolError("\"tags\" must not contain duplicate values");
                         spec.tags.push_back(value);
                     }
                 }
                 specs.push_back(std::move(spec));
             }

             std::vector<Entity> created;
             std::vector<std::vector<std::string>> warnings;
             created.reserve(specs.size());
             warnings.reserve(specs.size());
             try {
                 for (const SpawnSpec& spec : specs) {
                     const Entity entity = world.CreateEntityRaw(spec.mask);
                     created.push_back(entity);
                     EnsureGuid(world, entity, &game.Guids());
                     warnings.emplace_back();
                     for (const auto& [id, values] : spec.components) {
                         const std::vector<std::string> component_warnings = ApplyComponentValues(world, entity, id, values);
                         warnings.back().insert(warnings.back().end(), component_warnings.begin(), component_warnings.end());
                     }
                     if (spec.has_tags) world.GetComponent<Tags>(entity)->names = spec.tags;
                 }
             } catch (...) {
                 for (auto it = created.rbegin(); it != created.rend(); ++it) game.GetLifecycle().Destroy(*it);
                 throw;
             }
             Json results = Json::array();
             for (usize i = 0; i < created.size(); ++i) {
                 Json result = EntityJson(game, created[i]);
                 if (!warnings[i].empty()) result["warnings"] = warnings[i];
                 results.push_back(std::move(result));
             }
             return {{"spawned_count", results.size()}, {"entities", results}};
         }});

    server.AddTool(
        {"game_add_component",
         "Add a component to a running entity (default values, then `values` merged on); if it already has one, `values` are merged onto it. Not undoable.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation]"}}},
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
             Json out = EntityJson(game, e);
             out["added"] = !had;
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool(
        {"game_remove_component", "Remove a component from a running entity. Not undoable.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation]"}}}, {"component", {{"type", "string"}}}}, {"entity", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             const Entity e = RequireEntity(game, args);
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             if (!world.HasComponentRaw(e, id)) throw ToolError("Entity has no " + ComponentName(id));
             if (id == GetComponentId<Transform>() || id == GetComponentId<IdComponent>()) throw ToolError(ComponentName(id) + " cannot be removed from a running entity");
             world.RemoveComponentRaw(e, id);
             return EntityJson(game, e);
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
        {"game_ui_set_widget",
         "Set a named game UI widget's text, numeric value, or checked state directly. Text inputs, sliders, and toggles invoke their change callbacks when their control API provides one.",
         Schema({{"widget", {{"type", "string"}}},
                 {"property", {{"type", "string"}, {"enum", Json::array({"text", "value", "checked"})}}},
                 {"text", {{"type", "string"}}},
                 {"value", {{"type", "number"}}},
                 {"checked", {{"type", "boolean"}}}}, {"widget", "property"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             const std::string name = RequireString(args, "widget");
             const std::string property = RequireString(args, "property");
             ui::Viewport* viewport = game.Ui();
             ui::Widget* widget = viewport != nullptr ? viewport->Find(name) : nullptr;
             if (widget == nullptr) throw ToolError("No widget named \"" + name + "\" in the game's UI");

             if (property == "text") {
                 const std::string text = RequireString(args, "text");
                 if (auto* label = dynamic_cast<ui::Text*>(widget)) {
                     label->text = text;
                     label->text_key.clear();
                 } else if (auto* input = dynamic_cast<ui::TextInput*>(widget)) {
                     input->SetText(text);
                 } else {
                     ui::Text* label = nullptr;
                     for (usize i = 0; i < widget->ChildCount() && label == nullptr; ++i) {
                         label = dynamic_cast<ui::Text*>(widget->Child(i));
                     }
                     if (label == nullptr) throw ToolError("Widget \"" + name + "\" does not contain editable text");
                     label->text = text;
                     label->text_key.clear();
                 }
             } else if (property == "value") {
                 if (!args.contains("value") || !args["value"].is_number()) throw ToolError("\"value\" must be a finite number");
                 const f64 requested = args["value"].get<f64>();
                 if (!std::isfinite(requested) || std::abs(requested) > std::numeric_limits<f32>::max()) throw ToolError("\"value\" must be a finite number in range");
                 const f32 value = static_cast<f32>(requested);
                 if (auto* slider = dynamic_cast<ui::Slider*>(widget)) slider->SetValue(value);
                 else if (auto* progress = dynamic_cast<ui::ProgressBar*>(widget)) {
                     if (value < 0.0f || value > 1.0f) throw ToolError("A ProgressBar value must be from 0 through 1");
                     progress->percent = value;
                 } else if (auto* dropdown = dynamic_cast<ui::Dropdown*>(widget)) {
                     if (std::trunc(requested) != requested || requested < -1.0 || requested >= static_cast<f64>(dropdown->options.size())) {
                         throw ToolError("A Dropdown value must be -1 or an existing option index");
                     }
                     dropdown->Select(static_cast<i32>(requested));
                 } else {
                     throw ToolError("Widget \"" + name + "\" does not accept a numeric value");
                 }
             } else if (property == "checked") {
                 if (!args.contains("checked") || !args["checked"].is_boolean()) throw ToolError("\"checked\" must be a boolean");
                 auto* toggle = dynamic_cast<ui::Toggle*>(widget);
                 if (toggle == nullptr) throw ToolError("Widget \"" + name + "\" is not a Toggle");
                 toggle->SetChecked(args["checked"].get<bool>());
             } else {
                 throw ToolError("\"property\" must be text, value or checked");
             }

             usize budget = 2000;
             return {{"widget", UiWidgetJson(*widget, budget)}};
         }});

    server.AddTool(
        {"game_ui_events",
         "Read UI control and animation events from the loaded game's most recent UI update, including the event kind, entity handle, widget name and any value or committed text.",
         Schema(Json::object()),
         [host](const Json&) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             ui::UISystem* system = game.Ui() != nullptr ? ui::UISystem::Active() : nullptr;
             Json events = Json::array();
             if (system != nullptr) {
                 for (const ui::WidgetEvent& event : system->Events()) {
                     const std::string entity = world.IsAlive(event.entity)
                         ? EntityKey(world, event.entity)
                         : "#" + std::to_string(event.entity.index) + ":" + std::to_string(event.entity.generation);
                     events.push_back({{"type", ui::UISystem::BlueprintEventName(event.kind)},
                                       {"entity", entity}, {"widget", event.widget},
                                       {"value", event.value}, {"text", event.text}});
                 }
             }
             return {{"count", events.size()}, {"events", std::move(events)}};
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
        {"game_update_tags_many",
         "Add and/or remove tags on up to 64 unique live entities in one validated batch; preserves tags not mentioned and creates Tags components when needed.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"add", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "string"}}}, {"description", "Tags to add to every selected entity"}}},
                 {"remove", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "string"}}}, {"description", "Tags to remove from every selected entity"}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args.contains("add") && !args.contains("remove")) throw ToolError("Supply at least one of \"add\" or \"remove\"");
             auto read_tags = [&args](const char* key) {
                 std::vector<std::string> tags;
                 if (!args.contains(key)) return tags;
                 const Json& values = args[key];
                 if (!values.is_array() || values.size() > 64) throw ToolError(std::string("\"") + key + "\" must be an array of at most 64 tags");
                 for (const Json& value : values) {
                     if (!value.is_string() || value.get<std::string>().empty()) {
                         throw ToolError(std::string("Every \"") + key + "\" entry must be a non-empty string");
                     }
                     const std::string tag = value.get<std::string>();
                     if (std::find(tags.begin(), tags.end(), tag) != tags.end()) throw ToolError(std::string("\"") + key + "\" must not contain duplicate tags");
                     tags.push_back(tag);
                 }
                 return tags;
             };
             const std::vector<std::string> to_add = read_tags("add");
             const std::vector<std::string> to_remove = read_tags("remove");
             for (const std::string& tag : to_add) {
                 if (std::find(to_remove.begin(), to_remove.end(), tag) != to_remove.end()) {
                     throw ToolError("A tag cannot appear in both \"add\" and \"remove\"");
                 }
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 entities.push_back(entity);
             }

             Json results = Json::array();
             usize added_count = 0;
             usize removed_count = 0;
             for (Entity entity : entities) {
                 Tags* state = world.GetComponent<Tags>(entity);
                 if (state == nullptr && !to_add.empty()) {
                     world.AddComponent<Tags>(entity);
                     state = world.GetComponent<Tags>(entity);
                 }
                 Json added = Json::array();
                 Json removed = Json::array();
                 if (state != nullptr) {
                     auto& names = state->names;
                     for (const std::string& tag : to_remove) {
                         const auto it = std::find(names.begin(), names.end(), tag);
                         if (it != names.end()) {
                             names.erase(it);
                             removed.push_back(tag);
                             ++removed_count;
                         }
                     }
                     for (const std::string& tag : to_add) {
                         if (std::find(names.begin(), names.end(), tag) == names.end()) {
                             names.push_back(tag);
                             added.push_back(tag);
                             ++added_count;
                         }
                     }
                 }
                 Json current_tags = Json::array();
                 if (state != nullptr) for (const std::string& tag : state->names) current_tags.push_back(tag);
                 results.push_back({{"entity", EntityKey(world, entity)}, {"added", added}, {"removed", removed}, {"tags", current_tags}});
             }
             return {{"added_count", added_count}, {"removed_count", removed_count}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_tags_many",
         "Replace the complete tag list on up to 64 unique live entities; an empty list clears all tags, and missing Tags components are created only when needed.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"tags", {{"type", "array"}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}},
                {"entities", "tags"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["tags"].is_array() || args["tags"].size() > 64) throw ToolError("\"tags\" must be an array of at most 64 tags");
             std::vector<std::string> requested_tags;
             requested_tags.reserve(args["tags"].size());
             for (const Json& value : args["tags"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"tags\" entry must be a non-empty string");
                 const std::string tag = value.get<std::string>();
                 if (std::find(requested_tags.begin(), requested_tags.end(), tag) != requested_tags.end()) {
                     throw ToolError("\"tags\" must not contain duplicate entries");
                 }
                 requested_tags.push_back(tag);
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 entities.push_back(entity);
             }
             Json results = Json::array();
             usize changed_count = 0;
             for (Entity entity : entities) {
                 Tags* current = world.GetComponent<Tags>(entity);
                 const std::vector<std::string> old_tags = current == nullptr ? std::vector<std::string>{} : current->names;
                 const bool changed = old_tags != requested_tags;
                 if (changed && current == nullptr && !requested_tags.empty()) {
                     world.AddComponent<Tags>(entity);
                     current = world.GetComponent<Tags>(entity);
                 }
                 if (current != nullptr && changed) current->names = requested_tags;
                 Json old_values = Json::array();
                 Json new_values = Json::array();
                 for (const std::string& tag : old_tags) old_values.push_back(tag);
                 for (const std::string& tag : requested_tags) new_values.push_back(tag);
                 results.push_back({{"entity", EntityKey(world, entity)}, {"changed", changed}, {"old_tags", old_values}, {"tags", new_values}});
                 if (changed) ++changed_count;
             }
             return {{"changed_count", changed_count}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_layer_many",
         "Set one collision layer index (0-31) on up to 64 unique live entities; missing Layer components mean layer 0 and are created only when needed.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", 31}}}},
                {"entities", "layer"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["layer"].is_number_integer()) throw ToolError("\"layer\" must be an integer from 0 to 31");
             const i64 layer_value = args["layer"].get<i64>();
             if (layer_value < 0 || layer_value >= static_cast<i64>(kMaxLayers)) throw ToolError("\"layer\" must be an integer from 0 to 31");
             const u8 requested_layer = static_cast<u8>(layer_value);
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 entities.push_back(entity);
             }
             Json results = Json::array();
             usize changed_count = 0;
             for (Entity entity : entities) {
                 Layer* state = world.GetComponent<Layer>(entity);
                 const u8 old_layer = state == nullptr ? 0 : state->index;
                 const bool changed = old_layer != requested_layer;
                 if (changed && state == nullptr) {
                     world.AddComponent<Layer>(entity, Layer{requested_layer});
                 } else if (changed) {
                     state->index = requested_layer;
                 }
                 results.push_back({{"entity", EntityKey(world, entity)}, {"old_layer", old_layer},
                                    {"layer", requested_layer}, {"changed", changed}});
                 if (changed) ++changed_count;
             }
             return {{"layer", requested_layer}, {"changed_count", changed_count}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_layers_many",
         "Set per-entity collision layer indices on up to 64 unique live entities in one validated batch; missing Layer components mean layer 0 and are created only when needed.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"}, {"properties", {{"entity", {{"type", "string"}}},
                                                                                 {"layer_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 31}}}}},
                                        {"required", Json::array({"entity", "layer_index"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity layer edits");
             }
             struct LayerEdit {
                 Entity entity;
                 u8 layer;
             };
             std::vector<LayerEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("layer_index") || !item["layer_index"].is_number_integer() || item["layer_index"].is_boolean()) {
                     throw ToolError("\"layer_index\" must be an integer from 0 to 31");
                 }
                 const i64 layer = item["layer_index"].get<i64>();
                 if (layer < 0 || layer >= static_cast<i64>(kMaxLayers)) throw ToolError("\"layer_index\" must be an integer from 0 to 31");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const LayerEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not contain the same entity more than once");
                 }
                 edits.push_back({entity, static_cast<u8>(layer)});
             }
             Json results = Json::array();
             usize changed_count = 0;
             for (const LayerEdit& edit : edits) {
                 Layer* state = world.GetComponent<Layer>(edit.entity);
                 const u8 old_layer = state == nullptr ? 0 : state->index;
                 const bool changed = old_layer != edit.layer;
                 if (changed && state == nullptr) world.AddComponent<Layer>(edit.entity, Layer{edit.layer});
                 else if (changed) state->index = edit.layer;
                 results.push_back({{"entity", EntityKey(world, edit.entity)}, {"old_layer", old_layer},
                                    {"layer_index", edit.layer}, {"changed", changed}});
                 if (changed) ++changed_count;
             }
             return {{"changed_count", changed_count}, {"results", results}};
         }});

    server.AddTool(
        {"game_rename_entities_many",
         "Rename up to 64 unique live entities in one validated batch; creates EntityName components when needed.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                                {"items", {{"type", "object"}, {"properties", {{"entity", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
                                            {"required", Json::array({"entity", "name"})}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity/name objects");
             }
             struct Rename {
                 Entity entity;
                 std::string name;
             };
             std::vector<Rename> renames;
             renames.reserve(args["entities"].size());
             for (const Json& item : args["entities"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty() ||
                     !item.contains("name") || !item["name"].is_string()) {
                     throw ToolError("Every \"entities\" entry must contain a non-empty string \"entity\" and string \"name\"");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(renames.begin(), renames.end(), [entity](const Rename& entry) { return entry.entity == entity; }) != renames.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 renames.push_back({entity, item["name"].get<std::string>()});
             }
             Json results = Json::array();
             for (const Rename& rename : renames) {
                 EntityName* current = world.GetComponent<EntityName>(rename.entity);
                 const Json old_name = current == nullptr ? Json(nullptr) : Json(current->value);
                 if (current == nullptr) world.AddComponent<EntityName>(rename.entity, EntityName{rename.name});
                 else current->value = rename.name;
                 results.push_back({{"entity", EntityKey(world, rename.entity)}, {"old_name", old_name}, {"name", rename.name}});
             }
             return {{"renamed_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_active_many",
         "Set the local active state on up to 64 unique running entities through the game's lifecycle, firing enable/disable behavior. Missing Active components mean active; unchanged entities are no-ops.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"active", {{"type", "boolean"}, {"description", "The requested local active state"}}}},
                {"entities", "active"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["active"].is_boolean()) throw ToolError("\"active\" must be a boolean");
             const bool requested_active = args["active"].get<bool>();
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 entities.push_back(entity);
             }
             usize changed_count = 0;
             for (Entity entity : entities) {
                 const Active* state = world.GetComponent<Active>(entity);
                 const bool was_active = state == nullptr || state->active;
                 if (was_active != requested_active) {
                     game.GetLifecycle().SetActive(entity, requested_active);
                     ++changed_count;
                 }
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 const Active* state = world.GetComponent<Active>(entity);
                 results.push_back({{"entity", EntityKey(world, entity)}, {"active", state == nullptr || state->active},
                                    {"active_in_hierarchy", IsActiveInHierarchy(world, game.Guids(), entity)}});
             }
             return {{"active", requested_active}, {"changed_count", changed_count}, {"results", results}};
         }});

    server.AddTool(
        {"game_toggle_active_many",
         "Toggle the local active state on up to 64 unique running entities through the game's lifecycle, firing enable/disable behavior.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 entities.push_back(entity);
             }
             for (Entity entity : entities) {
                 const Active* state = world.GetComponent<Active>(entity);
                 const bool was_active = state == nullptr || state->active;
                 game.GetLifecycle().SetActive(entity, !was_active);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 const Active* state = world.GetComponent<Active>(entity);
                 results.push_back({{"entity", EntityKey(world, entity)}, {"active", state == nullptr || state->active},
                                    {"active_in_hierarchy", IsActiveInHierarchy(world, game.Guids(), entity)}});
             }
             return {{"toggled_count", entities.size()}, {"results", results}};
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
         "Entities in the running game: handle (GUID, or #index when it has none), entity generation, name, world position, collision layer index, local and in-hierarchy active state, direct parent GUID, tags and component names. "
         "Optionally filter by exact name, case-insensitive ASCII name substring, one component or component groups, exact tag or tag groups, direct parent, root status, collision layer index or name, local active state or in-hierarchy active state. Results can be paged with a bounded limit and offset or entity-index cursor.",
         Schema({{"name", {{"type", "string"}, {"description", "Exact, case-sensitive entity name"}}},
                 {"name_contains", {{"type", "string"}, {"description", "Non-empty case-insensitive ASCII name substring"}}},
                 {"parent", {{"type", "string"}, {"description", "Direct parent entity GUID"}}},
                 {"root_only", {{"type", "boolean"}, {"description", "Only entities without a parent"}}},
                 {"component", {{"type", "string"}}}, {"tag", {{"type", "string"}}},
                 {"components_all", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}},
                                     {"description", "Require every listed component"}}},
                 {"components_any", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}},
                                     {"description", "Require at least one listed component"}}},
                 {"components_none", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}},
                                      {"description", "Exclude entities with any listed component"}}},
                 {"tags_all", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}},
                               {"description", "Require every listed exact tag"}}},
                 {"tags_any", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}},
                               {"description", "Require at least one listed exact tag"}}},
                 {"tags_none", {{"type", "array"}, {"maxItems", 32}, {"items", {{"type", "string"}}},
                                {"description", "Exclude entities with any listed exact tag"}}},
                 {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", 31}, {"description", "Filter by collision layer index (0-31; absent Layer means 0)"}}},
                 {"layer_name", {{"type", "string"}, {"description", "Exact, case-sensitive configured collision layer name"}}},
                 {"active", {{"type", "boolean"}, {"description", "Filter by local active state; entities without Active are active"}}},
                 {"active_in_hierarchy", {{"type", "boolean"}, {"description", "Filter by effective active state including ancestors"}}},
                 {"min_world_position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}},
                                          {"description", "Inclusive minimum world position [x, y, z]; requires Transform"}}},
                 {"max_world_position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}},
                                          {"description", "Inclusive maximum world position [x, y, z]; requires Transform"}}},
                 {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Page size, 1-1000 (default 200)"}}},
                 {"offset", {{"type", "integer"}, {"minimum", 0}, {"description", "Number of matching entities to skip; must be 0 with after_entity_index"}}},
                 {"after_entity_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295},
                                          {"description", "Continue after this entity index in stable index order"}}}}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             ComponentId filter = kInvalidComponentId;
             if (args.contains("component")) filter = RequireComponent(RequireString(args, "component"));
             auto read_component_filter = [&](const char* key) {
                 std::vector<ComponentId> components;
                 if (!args.contains(key)) return components;
                 if (!args[key].is_array() || args[key].size() > 32) {
                     throw ToolError(std::string("\"") + key + "\" must be an array of at most 32 component names");
                 }
                 for (const Json& value : args[key]) {
                     if (!value.is_string() || value.get<std::string>().empty()) {
                         throw ToolError(std::string("Every \"") + key + "\" entry must be a non-empty component name");
                     }
                     const ComponentId id = RequireComponent(value.get<std::string>());
                     if (std::find(components.begin(), components.end(), id) != components.end()) {
                         throw ToolError(std::string("Duplicate component in \"") + key + "\": " + value.get<std::string>());
                     }
                     components.push_back(id);
                 }
                 return components;
             };
             const std::vector<ComponentId> components_all = read_component_filter("components_all");
             const std::vector<ComponentId> components_any = read_component_filter("components_any");
             const std::vector<ComponentId> components_none = read_component_filter("components_none");
             auto read_tag_filter = [&](const char* key) {
                 std::vector<std::string> tags;
                 if (!args.contains(key)) return tags;
                 if (!args[key].is_array() || args[key].size() > 32) {
                     throw ToolError(std::string("\"") + key + "\" must be an array of at most 32 tags");
                 }
                 for (const Json& value : args[key]) {
                     if (!value.is_string() || value.get<std::string>().empty()) {
                         throw ToolError(std::string("Every \"") + key + "\" entry must be a non-empty tag");
                     }
                     const std::string tag = value.get<std::string>();
                     if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
                         throw ToolError(std::string("Duplicate tag in \"") + key + "\": " + tag);
                     }
                     tags.push_back(tag);
                 }
                 return tags;
             };
             const std::vector<std::string> tags_all = read_tag_filter("tags_all");
             const std::vector<std::string> tags_any = read_tag_filter("tags_any");
             const std::vector<std::string> tags_none = read_tag_filter("tags_none");
             const std::string name_filter = args.contains("name") ? RequireString(args, "name") : std::string{};
             const std::string name_contains_filter = args.contains("name_contains") ? RequireString(args, "name_contains") : std::string{};
             if (args.contains("name_contains") && name_contains_filter.empty()) {
                 throw ToolError("\"name_contains\" must be a non-empty string");
             }
             EntityGuid parent_filter;
             if (args.contains("parent") && !ParseEntityGuid(RequireString(args, "parent"), parent_filter)) {
                 throw ToolError("\"parent\" must be an entity GUID");
             }
             if (args.contains("root_only") && !args["root_only"].is_boolean()) throw ToolError("\"root_only\" must be a boolean");
             u8 layer_filter = 0;
             if (args.contains("layer")) {
                 if (!args["layer"].is_number_integer()) throw ToolError("\"layer\" must be an integer from 0 to 31");
                 if (args["layer"].is_number_unsigned()) {
                     const u64 requested = args["layer"].get<u64>();
                     if (requested > 31) throw ToolError("\"layer\" must be an integer from 0 to 31");
                     layer_filter = static_cast<u8>(requested);
                 } else {
                     const i64 requested = args["layer"].get<i64>();
                     if (requested < 0 || requested > 31) throw ToolError("\"layer\" must be an integer from 0 to 31");
                     layer_filter = static_cast<u8>(requested);
                 }
             }
             const auto& configured_layers = host->package->Manifest().layers;
             std::string layer_name_filter;
             if (args.contains("layer_name")) {
                 layer_name_filter = RequireString(args, "layer_name");
                 if (layer_name_filter.empty()) throw ToolError("\"layer_name\" must not be empty");
                 const bool implicit_default = configured_layers.empty() && layer_name_filter == "Default";
                 if (!implicit_default && std::find(configured_layers.begin(), configured_layers.end(), layer_name_filter) == configured_layers.end()) {
                     throw ToolError("Unknown collision layer \"" + layer_name_filter + "\"");
                 }
             }
             if (args.contains("active") && !args["active"].is_boolean()) throw ToolError("\"active\" must be a boolean");
             if (args.contains("active_in_hierarchy") && !args["active_in_hierarchy"].is_boolean()) {
                 throw ToolError("\"active_in_hierarchy\" must be a boolean");
             }
             auto read_position_bound = [&](const char* key) -> std::optional<Vec3> {
                 if (!args.contains(key)) return std::nullopt;
                 const Json& value = args[key];
                 if (!value.is_array() || value.size() != 3) {
                     throw ToolError(std::string("\"") + key + "\" must be an array of three finite numbers [x, y, z]");
                 }
                 f32 coordinates[3]{};
                 for (usize axis = 0; axis < 3; ++axis) {
                     if (!value[axis].is_number()) {
                         throw ToolError(std::string("\"") + key + "\" must be an array of three finite numbers [x, y, z]");
                     }
                     const f64 coordinate = value[axis].get<f64>();
                     if (!std::isfinite(coordinate) || coordinate < -std::numeric_limits<f32>::max() ||
                         coordinate > std::numeric_limits<f32>::max()) {
                         throw ToolError(std::string("\"") + key + "\" values must be finite and representable as floats");
                     }
                     coordinates[axis] = static_cast<f32>(coordinate);
                 }
                 return Vec3(coordinates[0], coordinates[1], coordinates[2]);
             };
             const std::optional<Vec3> min_world_position = read_position_bound("min_world_position");
             const std::optional<Vec3> max_world_position = read_position_bound("max_world_position");
             if (min_world_position.has_value() && max_world_position.has_value() &&
                 (min_world_position->x > max_world_position->x || min_world_position->y > max_world_position->y ||
                  min_world_position->z > max_world_position->z)) {
                 throw ToolError("Each min_world_position coordinate must be less than or equal to max_world_position");
             }
             usize limit = 200;
             if (args.contains("limit")) {
                 if (!args["limit"].is_number_integer()) throw ToolError("\"limit\" must be an integer");
                 const f64 requested = args["limit"].get<f64>();
                 if (requested < 1 || requested > 1000) throw ToolError("\"limit\" must be between 1 and 1000");
                 limit = static_cast<usize>(requested);
             }
             usize offset = 0;
             std::optional<u32> after_entity_index;
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
             if (args.contains("after_entity_index")) {
                 if (!args["after_entity_index"].is_number_integer()) {
                     throw ToolError("\"after_entity_index\" must be an integer from 0 to 4294967295");
                 }
                 if (args["after_entity_index"].is_number_unsigned()) {
                     const u64 requested = args["after_entity_index"].get<u64>();
                     if (requested > std::numeric_limits<u32>::max()) {
                         throw ToolError("\"after_entity_index\" must be an integer from 0 to 4294967295");
                     }
                     after_entity_index = static_cast<u32>(requested);
                 } else {
                     const i64 requested = args["after_entity_index"].get<i64>();
                     if (requested < 0 || static_cast<u64>(requested) > std::numeric_limits<u32>::max()) {
                         throw ToolError("\"after_entity_index\" must be an integer from 0 to 4294967295");
                     }
                     after_entity_index = static_cast<u32>(requested);
                 }
             }
             if (after_entity_index.has_value() && offset != 0) {
                 throw ToolError("\"offset\" must be 0 when using \"after_entity_index\"");
             }
             std::vector<Entity> candidates;
             if (args.contains("tag")) candidates = FindEntitiesWithTag(world, RequireString(args, "tag"));
             else candidates = AllEntities(world);
             std::sort(candidates.begin(), candidates.end(), [](Entity a, Entity b) { return a.index < b.index; });
             Json list = Json::array();
             usize total = 0;
             static const std::string implicit_default_layer = "Default";
             for (Entity e : candidates) {
                 if (after_entity_index.has_value() && e.index <= *after_entity_index) continue;
                 if (filter != kInvalidComponentId && !world.HasComponentRaw(e, filter)) continue;
                 bool has_all_components = true;
                 for (ComponentId id : components_all) {
                     has_all_components = has_all_components && world.HasComponentRaw(e, id);
                 }
                 if (!has_all_components) continue;
                 if (!components_any.empty()) {
                     bool has_any_component = false;
                     for (ComponentId id : components_any) {
                         has_any_component = has_any_component || world.HasComponentRaw(e, id);
                     }
                     if (!has_any_component) continue;
                 }
                 bool has_excluded_component = false;
                 for (ComponentId id : components_none) {
                     has_excluded_component = has_excluded_component || world.HasComponentRaw(e, id);
                 }
                 if (has_excluded_component) continue;
                 const Tags* entity_tags = world.GetComponent<Tags>(e);
                 auto has_tag = [&](const std::string& tag) {
                     return entity_tags != nullptr &&
                            std::find(entity_tags->names.begin(), entity_tags->names.end(), tag) != entity_tags->names.end();
                 };
                 bool has_all_tags = true;
                 for (const std::string& tag : tags_all) has_all_tags = has_all_tags && has_tag(tag);
                 if (!has_all_tags) continue;
                 if (!tags_any.empty()) {
                     bool has_any_tag = false;
                     for (const std::string& tag : tags_any) has_any_tag = has_any_tag || has_tag(tag);
                     if (!has_any_tag) continue;
                 }
                 bool has_excluded_tag = false;
                 for (const std::string& tag : tags_none) has_excluded_tag = has_excluded_tag || has_tag(tag);
                 if (has_excluded_tag) continue;
                 const EntityName* entity_name = world.GetComponent<EntityName>(e);
                 if (args.contains("name") && (entity_name == nullptr || entity_name->value != name_filter)) continue;
                 if (args.contains("name_contains") &&
                     (entity_name == nullptr || std::search(entity_name->value.begin(), entity_name->value.end(),
                                                            name_contains_filter.begin(), name_contains_filter.end(),
                                                            [](unsigned char lhs, unsigned char rhs) {
                                                                const unsigned char folded_lhs = lhs >= 'A' && lhs <= 'Z'
                                                                                                   ? static_cast<unsigned char>(lhs + ('a' - 'A'))
                                                                                                   : lhs;
                                                                const unsigned char folded_rhs = rhs >= 'A' && rhs <= 'Z'
                                                                                                   ? static_cast<unsigned char>(rhs + ('a' - 'A'))
                                                                                                   : rhs;
                                                                return folded_lhs == folded_rhs;
                                                            }) == entity_name->value.end())) continue;
                 const Parent* parent_component = world.GetComponent<Parent>(e);
                 const bool has_parent = parent_component != nullptr && !parent_component->parent.IsNull();
                 if (args.contains("parent") && (!has_parent || parent_component->parent != parent_filter)) continue;
                 if (args.contains("root_only") && args["root_only"].get<bool>() == has_parent) continue;
                 const u8 entity_layer = LayerOf(world, e);
                 if (args.contains("layer") && entity_layer != layer_filter) continue;
                 const std::string* entity_layer_name = entity_layer < configured_layers.size() ? &configured_layers[entity_layer] : nullptr;
                 if (entity_layer_name == nullptr && entity_layer == 0 && configured_layers.empty()) entity_layer_name = &implicit_default_layer;
                 if (args.contains("layer_name") && (entity_layer_name == nullptr || *entity_layer_name != layer_name_filter)) continue;
                 const Active* active_component = world.GetComponent<Active>(e);
                 const bool active = active_component == nullptr || active_component->active;
                 if (args.contains("active") && active != args["active"].get<bool>()) continue;
                 const bool active_in_hierarchy = args.contains("active_in_hierarchy")
                                                      ? IsActiveInHierarchy(world, game.Guids(), e)
                                                      : active;
                 if (args.contains("active_in_hierarchy") && active_in_hierarchy != args["active_in_hierarchy"].get<bool>()) continue;
                 Vec3 world_position;
                 bool world_position_computed = false;
                 if (min_world_position.has_value() || max_world_position.has_value()) {
                     world_position = WorldPosition(world, game.Guids(), e);
                     world_position_computed = true;
                     if (!world.HasComponent<Transform>(e) || !std::isfinite(world_position.x) || !std::isfinite(world_position.y) ||
                         !std::isfinite(world_position.z)) {
                         continue;
                     }
                     if (min_world_position.has_value() &&
                         (world_position.x < min_world_position->x || world_position.y < min_world_position->y ||
                          world_position.z < min_world_position->z)) {
                         continue;
                     }
                     if (max_world_position.has_value() &&
                         (world_position.x > max_world_position->x || world_position.y > max_world_position->y ||
                          world_position.z > max_world_position->z)) {
                         continue;
                     }
                 }
                 ++total;
                 if (total <= offset || list.size() >= limit) continue;
                 Json names = Json::array();
                 for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
                     if (world.HasComponentRaw(e, id)) names.push_back(ComponentName(id));
                 }
                 Json tags = Json::array();
                 if (entity_tags != nullptr) {
                     for (const std::string& tag : entity_tags->names) tags.push_back(tag);
                 }
                 Json parent = has_parent ? Json(ToString(parent_component->parent)) : Json(nullptr);
                 Json row = {{"entity", EntityKey(world, e)}, {"entity_index", e.index}, {"entity_generation", e.generation},
                             {"components", names}, {"tags", tags},
                             {"layer", entity_layer},
                             {"active", active_component == nullptr || active_component->active},
                             {"active_in_hierarchy", args.contains("active_in_hierarchy") ? active_in_hierarchy
                                                                                            : IsActiveInHierarchy(world, game.Guids(), e)},
                             {"parent", parent}};
                 if (!world_position_computed) world_position = WorldPosition(world, game.Guids(), e);
                 row["world_position"] = std::isfinite(world_position.x) && std::isfinite(world_position.y) && std::isfinite(world_position.z)
                                              ? Json::array({world_position.x, world_position.y, world_position.z})
                                              : Json(nullptr);
                 row["layer_name"] = entity_layer_name != nullptr ? Json(*entity_layer_name) : Json(nullptr);
                 if (entity_name != nullptr) row["name"] = entity_name->value;
                 list.push_back(row);
             }
             const usize remaining = total > offset ? total - offset : 0;
             const Json next_entity_index = list.empty() ? Json(nullptr) : list.back()["entity_index"];
             return {{"total", total}, {"offset", offset}, {"after_entity_index", after_entity_index.has_value() ? Json(*after_entity_index) : Json(nullptr)},
                     {"next_entity_index", next_entity_index}, {"limit", limit}, {"has_more", list.size() < remaining}, {"entities", list}};
         }});

    server.AddTool({"game_get_entity", "Every component of one entity in the running game, as JSON.",
                    Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation] from game_list_entities"}}}}, {"entity"}),
                    [host](const Json& args) -> Json {
                        Game& game = host->Require();
                        return EntityJson(game, RequireEntity(game, args));
                    }});

    server.AddTool(
        {"game_get_component",
         "Read one reflected component from an entity in the running game, as JSON.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation]"}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}}},
                {"entity", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             const Entity entity = RequireEntity(game, args);
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             const void* data = world.GetComponentRaw(entity, id);
             if (data == nullptr) throw ToolError("Entity has no " + ComponentName(id));
             const ComponentInfo& info = GetComponentInfo(id);
             return {{"entity", EntityKey(world, entity)}, {"component", ComponentName(id)},
                     {"values", reflect::ToJson(*info.reflected, data)}};
         }});

    server.AddTool(
        {"game_get_components_many",
         "Read one reflected component across up to 64 unique running entities in one bounded response. Entities without the component are returned with present=false.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}}},
                {"entities", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             const ComponentInfo& info = GetComponentInfo(id);
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 const void* data = world.GetComponentRaw(entity, id);
                 Json result = {{"entity", EntityKey(world, entity)}, {"entity_index", entity.index},
                                {"entity_generation", entity.generation}, {"present", data != nullptr}};
                 if (data != nullptr) result["values"] = reflect::ToJson(*info.reflected, data);
                 results.push_back(std::move(result));
             }
             return {{"component", ComponentName(id)}, {"results", results}};
         }});

    server.AddTool(
        {"game_get_entity_components_many",
         "Read different reflected components from up to 64 live entity/component pairs in request order; missing components are reported with present=false.",
         Schema({{"reads", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}}, {"component", {{"type", "string"}}}}},
                                         {"required", Json::array({"entity", "component"})}}}}}},
                {"reads"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["reads"].is_array() || args["reads"].empty() || args["reads"].size() > 64) {
                 throw ToolError("\"reads\" must be an array of 1 to 64 entity/component pairs");
             }
             struct ReadRequest {
                 Entity entity;
                 ComponentId id;
             };
             std::vector<ReadRequest> requests;
             requests.reserve(args["reads"].size());
             for (const Json& item : args["reads"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"reads\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"reads\" entry must contain a string \"component\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (std::find_if(requests.begin(), requests.end(), [entity, id](const ReadRequest& request) {
                         return request.entity == entity && request.id == id;
                     }) != requests.end()) {
                     throw ToolError("\"reads\" must not repeat the same entity/component pair");
                 }
                 requests.push_back({entity, id});
             }
             Json results = Json::array();
             for (const ReadRequest& request : requests) {
                 const void* data = world.GetComponentRaw(request.entity, request.id);
                 Json result = {{"entity", EntityKey(world, request.entity)}, {"entity_index", request.entity.index},
                                {"entity_generation", request.entity.generation}, {"component", ComponentName(request.id)},
                                {"present", data != nullptr}};
                 if (data != nullptr) result["values"] = reflect::ToJson(*GetComponentInfo(request.id).reflected, data);
                 results.push_back(std::move(result));
             }
             return {{"results", results}};
         }});

    server.AddTool(
        {"game_remove_components_many",
         "Remove one component from up to 64 unique running entities as one validated batch. Transform and IdComponent are protected; no entity is changed unless the entire selection is valid.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}}},
                {"entities", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             if (id == GetComponentId<Transform>() || id == GetComponentId<IdComponent>()) {
                 throw ToolError(ComponentName(id) + " cannot be removed from a running entity");
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 if (!world.HasComponentRaw(entity, id)) throw ToolError("Entity " + EntityKey(world, entity) + " has no " + ComponentName(id));
                 entities.push_back(entity);
             }
             for (Entity entity : entities) world.RemoveComponentRaw(entity, id);
             Json results = Json::array();
             for (Entity entity : entities) results.push_back({{"entity", EntityKey(world, entity)}, {"removed", true}});
             return {{"component", ComponentName(id)}, {"removed_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_remove_entity_components_many",
         "Remove different components from up to 64 live entity/component pairs. Validates every target first; Transform and game-managed IdComponent are protected.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}}, {"component", {{"type", "string"}}}}},
                                         {"required", Json::array({"entity", "component"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity/component removals");
             }
             struct Removal {
                 Entity entity;
                 ComponentId id;
             };
             std::vector<Removal> removals;
             removals.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"component\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (id == GetComponentId<Transform>() || id == GetComponentId<IdComponent>()) {
                     throw ToolError(ComponentName(id) + " cannot be removed from a running entity");
                 }
                 if (std::find_if(removals.begin(), removals.end(), [entity, id](const Removal& removal) {
                         return removal.entity == entity && removal.id == id;
                     }) != removals.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity/component pair");
                 }
                 if (!world.HasComponentRaw(entity, id)) throw ToolError("Entity " + EntityKey(world, entity) + " has no " + ComponentName(id));
                 removals.push_back({entity, id});
             }
             for (const Removal& removal : removals) world.RemoveComponentRaw(removal.entity, removal.id);
             Json results = Json::array();
             for (const Removal& removal : removals) {
                 results.push_back({{"entity", EntityKey(world, removal.entity)}, {"component", ComponentName(removal.id)}, {"removed", true}});
             }
             return {{"removed_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_add_components_many",
         "Add one default-constructed component to up to 64 unique running entities as one validated batch. The batch is refused if any entity already has the component.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}}},
                {"entities", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be added directly");
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 if (world.HasComponentRaw(entity, id)) {
                     throw ToolError("Entity " + EntityKey(world, entity) + " already has " + ComponentName(id));
                 }
                 entities.push_back(entity);
             }
             for (Entity entity : entities) world.AddComponentRaw(entity, id);
             Json results = Json::array();
             for (Entity entity : entities) results.push_back({{"entity", EntityKey(world, entity)}, {"added", true}});
             return {{"component", ComponentName(id)}, {"added_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_add_entity_components_many",
         "Add different default-constructed components to up to 64 live entity/component pairs. Validates every target first and protects game-managed IdComponent.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}}, {"component", {{"type", "string"}}}}},
                                         {"required", Json::array({"entity", "component"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity/component additions");
             }
             struct Addition {
                 Entity entity;
                 ComponentId id;
             };
             std::vector<Addition> additions;
             additions.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"component\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be added directly");
                 if (std::find_if(additions.begin(), additions.end(), [entity, id](const Addition& addition) {
                         return addition.entity == entity && addition.id == id;
                     }) != additions.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity/component pair");
                 }
                 if (world.HasComponentRaw(entity, id)) throw ToolError("Entity " + EntityKey(world, entity) + " already has " + ComponentName(id));
                 additions.push_back({entity, id});
             }
             for (const Addition& addition : additions) world.AddComponentRaw(addition.entity, addition.id);
             Json results = Json::array();
             for (const Addition& addition : additions) {
                 results.push_back({{"entity", EntityKey(world, addition.entity)}, {"component", ComponentName(addition.id)}, {"added", true}});
             }
             return {{"added_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_upsert_entity_components_many",
         "Add or update different components on up to 64 live entity/component pairs. Missing components start with reflected defaults; IdComponent is protected and the full request is validated first.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}}, {"component", {{"type", "string"}}},
                                                          {"values", {{"type", "object"}}}}},
                                         {"required", Json::array({"entity", "component", "values"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity/component upserts");
             }
             struct PreparedUpsert {
                 Entity entity;
                 ComponentId id;
                 bool added;
                 Json values;
                 std::vector<std::string> warnings;
             };
             std::vector<PreparedUpsert> upserts;
             upserts.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"component\"");
                 if (!item.contains("values") || !item["values"].is_object()) throw ToolError("Every \"edits\" entry must contain object \"values\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be changed directly");
                 if (std::find_if(upserts.begin(), upserts.end(), [entity, id](const PreparedUpsert& upsert) {
                         return upsert.entity == entity && upsert.id == id;
                     }) != upserts.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity/component pair");
                 }
                 const ComponentInfo& info = GetComponentInfo(id);
                 const void* data = world.GetComponentRaw(entity, id);
                 reflect::Any candidate = reflect::Any::DefaultOf(*info.reflected);
                 if (!candidate.HasValue()) throw ToolError("Cannot construct reflected defaults for " + ComponentName(id));
                 Json merged = data != nullptr ? reflect::ToJson(*info.reflected, data) : reflect::ToJson(*info.reflected, candidate.Data());
                 merged.merge_patch(item["values"]);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, candidate.Data(), merged, &report)) {
                     throw ToolError("Value does not match the shape of " + ComponentName(id) + " on entity " + EntityKey(world, entity));
                 }
                 upserts.push_back({entity, id, data == nullptr, std::move(merged), std::move(report.warnings)});
             }
             Json results = Json::array();
             for (PreparedUpsert& upsert : upserts) {
                 const ComponentInfo& info = GetComponentInfo(upsert.id);
                 if (upsert.added) world.AddComponentRaw(upsert.entity, upsert.id);
                 void* data = world.GetComponentRaw(upsert.entity, upsert.id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, upsert.values, &report)) {
                     throw ToolError("Validated component upsert could not be applied to entity " + EntityKey(world, upsert.entity));
                 }
                 Json result = {{"entity", EntityKey(world, upsert.entity)}, {"component", ComponentName(upsert.id)},
                                {"added", upsert.added}, {"values", reflect::ToJson(*info.reflected, data)}};
                 if (!upsert.warnings.empty()) result["warnings"] = upsert.warnings;
                 results.push_back(std::move(result));
             }
             return {{"upserted_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_components_many",
         "Apply the same partial field update to one component on up to 64 unique running entities. The complete selection and every merged value are validated before any component changes.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}},
                 {"values", {{"type", "object"}, {"description", "Fields to merge onto the component on each entity"}}}},
                {"entities", "component", "values"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["values"].is_object()) throw ToolError("\"values\" must be an object of component fields");
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             const ComponentInfo& info = GetComponentInfo(id);
             struct PreparedUpdate {
                 Entity entity;
                 Json merged;
                 std::vector<std::string> warnings;
             };
             std::vector<Entity> seen;
             std::vector<PreparedUpdate> updates;
             seen.reserve(args["entities"].size());
             updates.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(seen.begin(), seen.end(), entity) != seen.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 seen.push_back(entity);
                 const void* data = world.GetComponentRaw(entity, id);
                 if (data == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no " + ComponentName(id));
                 Json merged = reflect::ToJson(*info.reflected, data);
                 merged.merge_patch(args["values"]);
                 reflect::Any candidate = reflect::Any::DefaultOf(*info.reflected);
                 if (!candidate.HasValue()) throw ToolError("Cannot validate default values for " + ComponentName(id));
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, candidate.Data(), merged, &report)) {
                     throw ToolError("Value does not match the shape of " + ComponentName(id) + " on entity " + EntityKey(world, entity));
                 }
                 updates.push_back({entity, std::move(merged), std::move(report.warnings)});
             }
             Json results = Json::array();
             for (PreparedUpdate& update : updates) {
                 void* data = world.GetComponentRaw(update.entity, id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, update.merged, &report)) {
                     throw ToolError("Validated component update could not be applied to entity " + EntityKey(world, update.entity));
                 }
                 Json result = {{"entity", EntityKey(world, update.entity)},
                                {"values", reflect::ToJson(*info.reflected, data)}};
                 if (!update.warnings.empty()) result["warnings"] = update.warnings;
                 results.push_back(std::move(result));
             }
             return {{"component", ComponentName(id)}, {"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_entity_components_many",
         "Apply different partial component patches to up to 64 live entity/component pairs. Every entity, component and merged value is validated before any field changes.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}},
                                                          {"component", {{"type", "string"}}},
                                                          {"values", {{"type", "object"}}}}},
                                         {"required", Json::array({"entity", "component", "values"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity/component patches");
             }
             struct PreparedUpdate {
                 Entity entity;
                 ComponentId id;
                 Json merged;
                 std::vector<std::string> warnings;
             };
             std::vector<PreparedUpdate> updates;
             updates.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"component\"");
                 if (!item.contains("values") || !item["values"].is_object()) throw ToolError("Every \"edits\" entry must contain object \"values\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be changed directly");
                 if (std::find_if(updates.begin(), updates.end(), [entity, id](const PreparedUpdate& update) {
                         return update.entity == entity && update.id == id;
                     }) != updates.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity/component pair");
                 }
                 const void* data = world.GetComponentRaw(entity, id);
                 if (data == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no " + ComponentName(id));
                 const ComponentInfo& info = GetComponentInfo(id);
                 Json merged = reflect::ToJson(*info.reflected, data);
                 merged.merge_patch(item["values"]);
                 reflect::Any candidate = reflect::Any::DefaultOf(*info.reflected);
                 if (!candidate.HasValue()) throw ToolError("Cannot validate default values for " + ComponentName(id));
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, candidate.Data(), merged, &report)) {
                     throw ToolError("Value does not match the shape of " + ComponentName(id) + " on entity " + EntityKey(world, entity));
                 }
                 updates.push_back({entity, id, std::move(merged), std::move(report.warnings)});
             }
             Json results = Json::array();
             for (PreparedUpdate& update : updates) {
                 const ComponentInfo& info = GetComponentInfo(update.id);
                 void* data = world.GetComponentRaw(update.entity, update.id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, update.merged, &report)) {
                     throw ToolError("Validated component update could not be applied to entity " + EntityKey(world, update.entity));
                 }
                 Json result = {{"entity", EntityKey(world, update.entity)}, {"component", ComponentName(update.id)},
                                {"values", reflect::ToJson(*info.reflected, data)}};
                 if (!update.warnings.empty()) result["warnings"] = update.warnings;
                 results.push_back(std::move(result));
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_reset_entity_components_many",
         "Reset different reflected components on up to 64 live entity/component pairs. Validates every target and default first, keeps components attached, and protects game-managed IdComponent.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}}, {"component", {{"type", "string"}}}}},
                                         {"required", Json::array({"entity", "component"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity/component resets");
             }
             struct PreparedReset {
                 Entity entity;
                 ComponentId id;
                 Json before;
                 Json defaults;
             };
             std::vector<PreparedReset> resets;
             resets.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"component\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be reset directly");
                 if (std::find_if(resets.begin(), resets.end(), [entity, id](const PreparedReset& reset) {
                         return reset.entity == entity && reset.id == id;
                     }) != resets.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity/component pair");
                 }
                 const void* data = world.GetComponentRaw(entity, id);
                 if (data == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no " + ComponentName(id));
                 const ComponentInfo& info = GetComponentInfo(id);
                 reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                 if (!defaults.HasValue()) throw ToolError("Cannot construct reflected defaults for " + ComponentName(id));
                 const Json default_values = reflect::ToJson(*info.reflected, defaults.Data());
                 resets.push_back({entity, id, reflect::ToJson(*info.reflected, data), default_values});
             }
             Json results = Json::array();
             usize changed_count = 0;
             for (const PreparedReset& reset : resets) {
                 const ComponentInfo& info = GetComponentInfo(reset.id);
                 void* data = world.GetComponentRaw(reset.entity, reset.id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, reset.defaults, &report)) {
                     throw ToolError("Default component values could not be applied to entity " + EntityKey(world, reset.entity));
                 }
                 const bool changed = reset.before != reset.defaults;
                 results.push_back({{"entity", EntityKey(world, reset.entity)}, {"component", ComponentName(reset.id)},
                                    {"changed", changed}, {"old_values", reset.before}, {"values", reflect::ToJson(*info.reflected, data)}});
                 if (changed) ++changed_count;
             }
             return {{"reset_count", results.size()}, {"changed_count", changed_count}, {"results", results}};
         }});

    server.AddTool(
        {"game_reset_components_many",
         "Reset reflected fields of one component to its defaults on up to 64 unique running entities. The component stays attached; the whole selection is validated before any reset.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}}},
                {"entities", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be reset directly");
             const ComponentInfo& info = GetComponentInfo(id);
             reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
             if (!defaults.HasValue()) throw ToolError("Cannot construct reflected defaults for " + ComponentName(id));
             const Json default_values = reflect::ToJson(*info.reflected, defaults.Data());
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 if (!world.HasComponentRaw(entity, id)) throw ToolError("Entity " + EntityKey(world, entity) + " has no " + ComponentName(id));
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 void* data = world.GetComponentRaw(entity, id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, default_values, &report)) {
                     throw ToolError("Default component values could not be applied to entity " + EntityKey(world, entity));
                 }
                 Json result = {{"entity", EntityKey(world, entity)}, {"values", reflect::ToJson(*info.reflected, data)}};
                 if (!report.warnings.empty()) result["warnings"] = report.warnings;
                 results.push_back(std::move(result));
             }
             return {{"component", ComponentName(id)}, {"reset_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_copy_entity_components_many",
         "Copy reflected component values using a different source and target for each of up to 64 edits. Every source, target, component and value is validated before any target changes; IdComponent is protected.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"source", {{"type", "string"}}}, {"target", {{"type", "string"}}},
                                                          {"component", {{"type", "string"}}}}},
                                         {"required", Json::array({"source", "target", "component"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 source/target/component copies");
             }
             struct PreparedCopy {
                 Entity source;
                 Entity target;
                 ComponentId id;
                 Json values;
             };
             std::vector<PreparedCopy> copies;
             copies.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object()) throw ToolError("Every \"edits\" entry must be an object");
                 for (const char* field : {"source", "target"}) {
                     if (!item.contains(field) || !item[field].is_string() || item[field].get<std::string>().empty()) {
                         throw ToolError(std::string("Every \"edits\" entry must contain a non-empty string \"") + field + "\"");
                     }
                 }
                 if (!item.contains("component") || !item["component"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"component\"");
                 const Entity source = FindEntity(game, item["source"].get<std::string>());
                 const Entity target = FindEntity(game, item["target"].get<std::string>());
                 const ComponentId id = RequireComponent(item["component"].get<std::string>());
                 if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be copied directly");
                 if (std::find_if(copies.begin(), copies.end(), [target, id](const PreparedCopy& copy) {
                         return copy.target == target && copy.id == id;
                     }) != copies.end()) {
                     throw ToolError("\"edits\" must not copy to the same target/component pair more than once");
                 }
                 const void* source_data = world.GetComponentRaw(source, id);
                 if (source_data == nullptr) throw ToolError("Source " + EntityKey(world, source) + " has no " + ComponentName(id));
                 const void* target_data = world.GetComponentRaw(target, id);
                 if (target_data == nullptr) throw ToolError("Target " + EntityKey(world, target) + " has no " + ComponentName(id));
                 const ComponentInfo& info = GetComponentInfo(id);
                 Json values = reflect::ToJson(*info.reflected, source_data);
                 reflect::Any candidate = reflect::Any::DefaultOf(*info.reflected);
                 if (!candidate.HasValue()) throw ToolError("Cannot validate values for " + ComponentName(id));
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, candidate.Data(), values, &report)) {
                     throw ToolError("Source " + ComponentName(id) + " values cannot be copied");
                 }
                 copies.push_back({source, target, id, std::move(values)});
             }
             Json results = Json::array();
             for (const PreparedCopy& copy : copies) {
                 const ComponentInfo& info = GetComponentInfo(copy.id);
                 void* data = world.GetComponentRaw(copy.target, copy.id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, copy.values, &report)) {
                     throw ToolError("Validated component values could not be copied to " + EntityKey(world, copy.target));
                 }
                 Json result = {{"source", EntityKey(world, copy.source)}, {"target", EntityKey(world, copy.target)},
                                {"component", ComponentName(copy.id)}, {"values", reflect::ToJson(*info.reflected, data)}};
                 if (!report.warnings.empty()) result["warnings"] = report.warnings;
                 results.push_back(std::move(result));
             }
             return {{"copied_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_copy_component_many",
         "Copy all reflected values of one component from a live source entity to up to 64 unique target entities. Every target must already have the component; IdComponent is protected.",
         Schema({{"source", {{"type", "string"}, {"description", "Source GUID or #index[:generation]"}}},
                 {"targets", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"component", {{"type", "string"}, {"description", "Component name from list_component_types"}}}},
                {"source", "targets", "component"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["targets"].is_array() || args["targets"].empty() || args["targets"].size() > 64) {
                 throw ToolError("\"targets\" must be an array of 1 to 64 entity handles");
             }
             const ComponentId id = RequireComponent(RequireString(args, "component"));
             if (id == GetComponentId<IdComponent>()) throw ToolError("IdComponent is managed by the running game and cannot be copied directly");
             const ComponentInfo& info = GetComponentInfo(id);
             const Entity source = FindEntity(game, RequireString(args, "source"));
             const void* source_data = world.GetComponentRaw(source, id);
             if (source_data == nullptr) throw ToolError("Source entity has no " + ComponentName(id));
             const Json source_values = reflect::ToJson(*info.reflected, source_data);
             reflect::Any candidate = reflect::Any::DefaultOf(*info.reflected);
             if (!candidate.HasValue()) throw ToolError("Cannot validate values for " + ComponentName(id));
             reflect::LoadReport validation;
             if (!reflect::FromJson(*info.reflected, candidate.Data(), source_values, &validation)) {
                 throw ToolError("Source " + ComponentName(id) + " values cannot be copied");
             }
             std::vector<Entity> targets;
             targets.reserve(args["targets"].size());
             for (const Json& handle : args["targets"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"targets\" entry must be a non-empty entity handle");
                 }
                 const Entity target = FindEntity(game, handle.get<std::string>());
                 if (std::find(targets.begin(), targets.end(), target) != targets.end()) {
                     throw ToolError("\"targets\" must not contain the same entity more than once");
                 }
                 if (!world.HasComponentRaw(target, id)) throw ToolError("Target entity " + EntityKey(world, target) + " has no " + ComponentName(id));
                 targets.push_back(target);
             }
             Json results = Json::array();
             for (Entity target : targets) {
                 void* data = world.GetComponentRaw(target, id);
                 reflect::LoadReport report;
                 if (!reflect::FromJson(*info.reflected, data, source_values, &report)) {
                     throw ToolError("Validated component values could not be copied to " + EntityKey(world, target));
                 }
                 Json result = {{"entity", EntityKey(world, target)}, {"values", reflect::ToJson(*info.reflected, data)}};
                 if (!report.warnings.empty()) result["warnings"] = report.warnings;
                 results.push_back(std::move(result));
             }
             return {{"source", EntityKey(world, source)}, {"component", ComponentName(id)},
                     {"copied_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_get_hierarchy",
         "Return an entity's live descendant tree in stable entity-index order. The root is included; depth and node caps bound the response.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation]"}}},
                 {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxHierarchyDepth}, {"description", "Maximum descendant depth (default 8)"}}},
                 {"max_nodes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}, {"description", "Maximum total nodes including the root (default 200)"}}}},
                {"entity"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             const Entity root = RequireEntity(game, args);
             usize max_depth = 8;
             if (args.contains("max_depth")) {
                 if (!args["max_depth"].is_number_integer()) throw ToolError("\"max_depth\" must be an integer from 1 to 64");
                 const i64 requested = args["max_depth"].get<i64>();
                 if (requested < 1 || requested > kMaxHierarchyDepth) throw ToolError("\"max_depth\" must be an integer from 1 to 64");
                 max_depth = static_cast<usize>(requested);
             }
             usize max_nodes = 200;
             if (args.contains("max_nodes")) {
                 if (!args["max_nodes"].is_number_integer()) throw ToolError("\"max_nodes\" must be an integer from 1 to 1000");
                 const i64 requested = args["max_nodes"].get<i64>();
                 if (requested < 1 || requested > 1000) throw ToolError("\"max_nodes\" must be an integer from 1 to 1000");
                 max_nodes = static_cast<usize>(requested);
             }
             usize remaining_nodes = max_nodes;
             auto metadata = [&](Entity entity) -> Json {
                 Json tags = Json::array();
                 if (const Tags* entity_tags = world.GetComponent<Tags>(entity)) {
                     for (const std::string& tag : entity_tags->names) tags.push_back(tag);
                 }
                 const EntityName* name = world.GetComponent<EntityName>(entity);
                 const Active* active = world.GetComponent<Active>(entity);
                 Json node = {{"entity", EntityKey(world, entity)}, {"entity_index", entity.index},
                              {"entity_generation", entity.generation}, {"tags", tags},
                              {"active", active == nullptr || active->active},
                              {"active_in_hierarchy", IsActiveInHierarchy(world, game.Guids(), entity)}};
                 if (name != nullptr) node["name"] = name->value;
                 return node;
             };
             std::vector<Entity> path{root};
             auto build = [&](auto&& self, Entity parent, usize depth) -> Json {
                 Json node = metadata(parent);
                 Json children_json = Json::array();
                 const std::vector<Entity> children = ChildrenOf(world, game.Guids(), parent);
                 if (depth >= max_depth) {
                     if (!children.empty()) node["truncated"] = true;
                     node["children"] = std::move(children_json);
                     return node;
                 }
                 for (Entity child : children) {
                     if (remaining_nodes == 0) {
                         node["truncated"] = true;
                         break;
                     }
                     if (std::find(path.begin(), path.end(), child) != path.end()) {
                         --remaining_nodes;
                         Json cycle = metadata(child);
                         cycle["cycle"] = true;
                         children_json.push_back(std::move(cycle));
                         continue;
                     }
                     --remaining_nodes;
                     path.push_back(child);
                     children_json.push_back(self(self, child, depth + 1));
                     path.pop_back();
                 }
                 node["children"] = std::move(children_json);
                 return node;
             };
             // The root consumes one slot, even though it is always returned.
             --remaining_nodes;
             Json tree = build(build, root, 0);
             return {{"max_depth", max_depth}, {"max_nodes", max_nodes}, {"tree", std::move(tree)}};
         }});

    server.AddTool(
        {"game_get_ancestors",
         "Return an entity's live parent chain from its immediate parent toward the root. The chain is bounded and reports truncation or cycles.",
         Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation]"}}},
                 {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxHierarchyDepth},
                                {"description", "Maximum number of ancestors to return (default 16)"}}}},
                {"entity"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             const Entity entity = RequireEntity(game, args);
             usize max_depth = 16;
             if (args.contains("max_depth")) {
                 if (!args["max_depth"].is_number_integer()) throw ToolError("\"max_depth\" must be an integer from 1 to 64");
                 const i64 requested = args["max_depth"].get<i64>();
                 if (requested < 1 || requested > kMaxHierarchyDepth) throw ToolError("\"max_depth\" must be an integer from 1 to 64");
                 max_depth = static_cast<usize>(requested);
             }
             auto summary = [&](Entity current) {
                 Json tags = Json::array();
                 if (const Tags* entity_tags = world.GetComponent<Tags>(current)) {
                     for (const std::string& tag : entity_tags->names) tags.push_back(tag);
                 }
                 const EntityName* name = world.GetComponent<EntityName>(current);
                 const Active* active = world.GetComponent<Active>(current);
                 Json node = {{"entity", EntityKey(world, current)}, {"entity_index", current.index},
                              {"entity_generation", current.generation}, {"tags", tags},
                              {"active", active == nullptr || active->active},
                              {"active_in_hierarchy", IsActiveInHierarchy(world, game.Guids(), current)}};
                 if (name != nullptr) node["name"] = name->value;
                 return node;
             };
             Json ancestors = Json::array();
             std::vector<Entity> visited{entity};
             Entity current = entity;
             bool root_reached = false;
             bool truncated = false;
             bool cycle = false;
             for (usize depth = 0; depth < max_depth; ++depth) {
                 const Entity parent = GetParent(world, game.Guids(), current);
                 if (parent.IsNull()) {
                     root_reached = true;
                     break;
                 }
                 if (std::find(visited.begin(), visited.end(), parent) != visited.end()) {
                     cycle = true;
                     break;
                 }
                 visited.push_back(parent);
                 ancestors.push_back(summary(parent));
                 current = parent;
             }
             if (!root_reached && !cycle) {
                 if (GetParent(world, game.Guids(), current).IsNull()) root_reached = true;
                 else if (ancestors.size() >= max_depth) truncated = true;
             }
             return {{"entity", EntityKey(world, entity)}, {"entity_index", entity.index},
                     {"entity_generation", entity.generation}, {"ancestors", ancestors},
                     {"root_reached", root_reached}, {"truncated", truncated}, {"cycle", cycle}};
         }});

    server.AddTool(
        {"game_get_world_transforms_many",
         "Read world transforms for 1 to 64 unique live entities in request order. Returns each 4x4 matrix as column vectors plus world position and basis-length world scale. Rejects missing Transform components, cycles, excessive hierarchy depth and non-finite results.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) throw ToolError("\"entities\" must not repeat an entity");
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 std::vector<Entity> visited;
                 Entity current = entity;
                 while (!current.IsNull()) {
                     if (visited.size() >= kMaxHierarchyDepth) throw ToolError("World-transform query encountered excessive hierarchy depth");
                     if (std::find(visited.begin(), visited.end(), current) != visited.end()) throw ToolError("World-transform query encountered a hierarchy cycle");
                     visited.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
                 const Mat4 matrix = ComputeWorldTransform(world, game.Guids(), entity);
                 Json columns = Json::array();
                 f32 scales[3]{};
                 for (int axis = 0; axis < 4; ++axis) {
                     const Vec4& column = matrix.cols[axis];
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("World transform has a non-finite matrix for " + EntityKey(world, entity));
                     }
                     columns.push_back(Json::array({column.x, column.y, column.z, column.w}));
                     if (axis < 3) {
                         scales[axis] = Vec3(column.x, column.y, column.z).Length();
                         if (!std::isfinite(scales[axis])) throw ToolError("World transform has a non-finite basis for " + EntityKey(world, entity));
                     }
                 }
                 const Vec4& position = matrix.cols[3];
                 results.push_back({{"entity", EntityKey(world, entity)},
                                    {"world_position", Json::array({position.x, position.y, position.z})},
                                    {"world_scale", Json::array({scales[0], scales[1], scales[2]})},
                                    {"world_matrix_columns", std::move(columns)}});
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

#if AETHER_MCP_PHYSICS
    server.AddTool(
        {"game_physics_query",
         "Query the loaded game's 3D physics scene with a raycast, sphere cast, or sphere overlap. Supports a 32-bit collision layer mask, an ignored entity and optional trigger inclusion.",
         Schema({{"kind", {{"type", "string"}, {"enum", Json::array({"raycast", "spherecast", "overlap_sphere"})}}},
                 {"from", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                 {"to", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                 {"center", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                 {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                 {"layer_mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}},
                 {"ignore_entity", {{"type", "string"}}},
                 {"include_triggers", {{"type", "boolean"}}}},
                {"kind"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             PhysicsScene* scene = game.PhysicsQueries();
             if (scene == nullptr) throw ToolError("The loaded game has no 3D physics scene");
             const std::string kind = RequireString(args, "kind");
             auto read_vec3 = [&](const char* field) {
                 if (!args.contains(field) || !args[field].is_array() || args[field].size() != 3 ||
                     !args[field][0].is_number() || !args[field][1].is_number() || !args[field][2].is_number()) {
                     throw ToolError(std::string("\"") + field + "\" must be [x, y, z]");
                 }
                 const Vec3 value(args[field][0].get<f32>(), args[field][1].get<f32>(), args[field][2].get<f32>());
                 if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
                     throw ToolError(std::string("\"") + field + "\" values must be finite");
                 }
                 return value;
             };
             LayerMask layers = kAllLayers;
             if (args.contains("layer_mask")) {
                 if (!args["layer_mask"].is_number_integer()) throw ToolError("\"layer_mask\" must be an integer from 0 through 4294967295");
                 const i64 value = args["layer_mask"].get<i64>();
                 if (value < 0 || static_cast<u64>(value) > std::numeric_limits<u32>::max()) throw ToolError("\"layer_mask\" must be an integer from 0 through 4294967295");
                 layers = static_cast<LayerMask>(value);
             }
             Entity ignore = kNullEntity;
             if (args.contains("ignore_entity")) {
                 if (!args["ignore_entity"].is_string() || args["ignore_entity"].get<std::string>().empty()) throw ToolError("\"ignore_entity\" must be a non-empty entity handle");
                 ignore = FindEntity(game, args["ignore_entity"].get<std::string>());
             }
             bool include_triggers = false;
             if (args.contains("include_triggers")) {
                 if (!args["include_triggers"].is_boolean()) throw ToolError("\"include_triggers\" must be a boolean");
                 include_triggers = args["include_triggers"].get<bool>();
             }
             if (kind == "raycast" || kind == "spherecast") {
                 const Vec3 from = read_vec3("from");
                 const Vec3 to = read_vec3("to");
                 f32 radius = 0.0f;
                 if (kind == "spherecast") {
                     if (!args.contains("radius") || !args["radius"].is_number()) throw ToolError("\"radius\" must be a finite positive number for spherecast");
                     radius = args["radius"].get<f32>();
                     if (!std::isfinite(radius) || radius <= 0.0f) throw ToolError("\"radius\" must be a finite positive number for spherecast");
                 }
                 const SceneHit hit = kind == "raycast" ? scene->RayCast(from, to, layers, ignore, include_triggers)
                                                         : scene->SphereCast(radius, from, to, layers, ignore, include_triggers);
                 Json result = {{"kind", kind}, {"hit", hit.hit}};
                 if (hit.hit) {
                     result["entity"] = hit.entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), hit.entity));
                     result["point"] = Json::array({hit.point.x, hit.point.y, hit.point.z});
                     result["normal"] = Json::array({hit.normal.x, hit.normal.y, hit.normal.z});
                     result["distance"] = hit.distance;
                 }
                 return result;
             }
             if (kind == "overlap_sphere") {
                 const Vec3 center = read_vec3("center");
                 if (!args.contains("radius") || !args["radius"].is_number()) throw ToolError("\"radius\" must be a finite positive number for overlap_sphere");
                 const f32 radius = args["radius"].get<f32>();
                 if (!std::isfinite(radius) || radius <= 0.0f) throw ToolError("\"radius\" must be a finite positive number for overlap_sphere");
                 Json entities = Json::array();
                 for (Entity entity : scene->OverlapSphere(center, radius, layers, ignore, include_triggers)) {
                     entities.push_back(entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), entity)));
                 }
                 return {{"kind", kind}, {"count", entities.size()}, {"entities", std::move(entities)}};
             }
             throw ToolError("\"kind\" must be raycast, spherecast or overlap_sphere");
         }});

    server.AddTool(
        {"game_physics_queries_many",
         "Run 1 to 64 3D raycast, spherecast, or sphere-overlap queries in one request. Each query can set its own layer mask, ignored entity, and trigger policy. Validates every query before running any of them.",
         Schema({{"queries", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                               {"items", {{"type", "object"},
                                          {"properties", {{"kind", {{"type", "string"}, {"enum", Json::array({"raycast", "spherecast", "overlap_sphere"})}},
                                                          {"from", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                                                          {"to", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                                                          {"center", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                                                          {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                                                          {"layer_mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}},
                                                          {"ignore_entity", {{"type", "string"}}}, {"include_triggers", {{"type", "boolean"}}}},
                                          {"required", Json::array({"kind"})}}}}}},
                {"queries"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             PhysicsScene* scene = game.PhysicsQueries();
             if (scene == nullptr) throw ToolError("The loaded game has no 3D physics scene");
             if (!args.contains("queries") || !args["queries"].is_array() || args["queries"].empty() || args["queries"].size() > 64) {
                 throw ToolError("\"queries\" must be an array of 1 to 64 physics queries");
             }
             struct PhysicsQuery {
                 std::string kind;
                 Vec3 from{};
                 Vec3 to{};
                 Vec3 center{};
                 f32 radius = 0.0f;
                 LayerMask layers = kAllLayers;
                 Entity ignore = kNullEntity;
                 bool include_triggers = false;
             };
             auto read_vec3 = [](const Json& query, const char* field) {
                 if (!query.contains(field) || !query[field].is_array() || query[field].size() != 3 ||
                     !query[field][0].is_number() || !query[field][1].is_number() || !query[field][2].is_number()) {
                     throw ToolError(std::string("\"") + field + "\" must be [x, y, z]");
                 }
                 const Vec3 value(query[field][0].get<f32>(), query[field][1].get<f32>(), query[field][2].get<f32>());
                 if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
                     throw ToolError(std::string("\"") + field + "\" values must be finite");
                 }
                 return value;
             };
             std::vector<PhysicsQuery> parsed;
             parsed.reserve(args["queries"].size());
             for (const Json& raw : args["queries"]) {
                 if (!raw.is_object() || !raw.contains("kind") || !raw["kind"].is_string()) throw ToolError("Every query must contain a string \"kind\"");
                 PhysicsQuery query;
                 query.kind = raw["kind"].get<std::string>();
                 if (query.kind != "raycast" && query.kind != "spherecast" && query.kind != "overlap_sphere") {
                     throw ToolError("Query kind must be raycast, spherecast or overlap_sphere");
                 }
                 if (query.kind == "raycast" || query.kind == "spherecast") {
                     query.from = read_vec3(raw, "from");
                     query.to = read_vec3(raw, "to");
                 } else {
                     query.center = read_vec3(raw, "center");
                 }
                 if (query.kind == "spherecast" || query.kind == "overlap_sphere") {
                     if (!raw.contains("radius") || !raw["radius"].is_number()) throw ToolError("Sphere queries require a finite positive \"radius\"");
                     query.radius = raw["radius"].get<f32>();
                     if (!std::isfinite(query.radius) || query.radius <= 0.0f) throw ToolError("Sphere queries require a finite positive \"radius\"");
                 }
                 if (raw.contains("layer_mask")) {
                     if (!raw["layer_mask"].is_number_integer()) throw ToolError("\"layer_mask\" must be an integer from 0 through 4294967295");
                     u64 value = 0;
                     if (raw["layer_mask"].is_number_unsigned()) value = raw["layer_mask"].get<u64>();
                     else {
                         const i64 signed_value = raw["layer_mask"].get<i64>();
                         if (signed_value < 0) throw ToolError("\"layer_mask\" must be an integer from 0 through 4294967295");
                         value = static_cast<u64>(signed_value);
                     }
                     if (value > std::numeric_limits<u32>::max()) throw ToolError("\"layer_mask\" must be an integer from 0 through 4294967295");
                     query.layers = static_cast<LayerMask>(value);
                 }
                 if (raw.contains("ignore_entity")) {
                     if (!raw["ignore_entity"].is_string() || raw["ignore_entity"].get<std::string>().empty()) throw ToolError("\"ignore_entity\" must be a non-empty entity handle");
                     query.ignore = FindEntity(game, raw["ignore_entity"].get<std::string>());
                 }
                 if (raw.contains("include_triggers")) {
                     if (!raw["include_triggers"].is_boolean()) throw ToolError("\"include_triggers\" must be a boolean");
                     query.include_triggers = raw["include_triggers"].get<bool>();
                 }
                 parsed.push_back(query);
             }
             Json results = Json::array();
             for (usize i = 0; i < parsed.size(); ++i) {
                 const PhysicsQuery& query = parsed[i];
                 if (query.kind == "raycast" || query.kind == "spherecast") {
                     const SceneHit hit = query.kind == "raycast" ? scene->RayCast(query.from, query.to, query.layers, query.ignore, query.include_triggers)
                                                                   : scene->SphereCast(query.radius, query.from, query.to, query.layers, query.ignore, query.include_triggers);
                     Json result = {{"index", i}, {"kind", query.kind}, {"hit", hit.hit}};
                     if (hit.hit) {
                         result["entity"] = hit.entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), hit.entity));
                         result["point"] = Json::array({hit.point.x, hit.point.y, hit.point.z});
                         result["normal"] = Json::array({hit.normal.x, hit.normal.y, hit.normal.z});
                         result["distance"] = hit.distance;
                     }
                     results.push_back(std::move(result));
                 } else {
                     Json entities = Json::array();
                     for (Entity entity : scene->OverlapSphere(query.center, query.radius, query.layers, query.ignore, query.include_triggers)) {
                         entities.push_back(entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), entity)));
                     }
                     results.push_back({{"index", i}, {"kind", query.kind}, {"count", entities.size()}, {"entities", std::move(entities)}});
                 }
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});
#endif

    server.AddTool(
        {"game_physics2d_query",
         "Query the loaded game's 2D physics scene with a raycast or axis-aligned box overlap. Supports a 32-bit collision-layer mask; raycasts report entity or solid-tile hits.",
         Schema({{"kind", {{"type", "string"}, {"enum", Json::array({"raycast", "overlap_box"})}}},
                 {"origin", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"direction", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"max_distance", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                 {"center", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"half_extents", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}}},
                {"kind"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             sprite2d::Physics2D* physics = game.Physics2D();
             if (physics == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             const std::string kind = RequireString(args, "kind");
             auto read_vec2 = [&](const char* field) {
                 if (!args.contains(field) || !args[field].is_array() || args[field].size() != 2 ||
                     !args[field][0].is_number() || !args[field][1].is_number()) throw ToolError(std::string("\"") + field + "\" must be [x, y]");
                 const sprite2d::Vec2 value{args[field][0].get<f32>(), args[field][1].get<f32>()};
                 if (!std::isfinite(value.x) || !std::isfinite(value.y)) throw ToolError(std::string("\"") + field + "\" values must be finite");
                 return value;
             };
             u32 mask = 0xFFFFFFFFu;
             if (args.contains("mask")) {
                 if (!args["mask"].is_number_integer()) throw ToolError("\"mask\" must be an integer from 0 through 4294967295");
                 const i64 value = args["mask"].get<i64>();
                 if (value < 0 || static_cast<u64>(value) > std::numeric_limits<u32>::max()) throw ToolError("\"mask\" must be an integer from 0 through 4294967295");
                 mask = static_cast<u32>(value);
             }
             if (kind == "raycast") {
                 const sprite2d::Vec2 origin = read_vec2("origin");
                 const sprite2d::Vec2 direction = read_vec2("direction");
                 if (!args.contains("max_distance") || !args["max_distance"].is_number()) throw ToolError("\"max_distance\" must be a finite positive number");
                 const f32 max_distance = args["max_distance"].get<f32>();
                 if (!std::isfinite(max_distance) || max_distance <= 0.0f) throw ToolError("\"max_distance\" must be a finite positive number");
                 sprite2d::RayHit2D hit;
                 const bool did_hit = physics->Raycast(origin, direction, max_distance, hit, mask);
                 Json result = {{"kind", kind}, {"hit", did_hit}};
                 if (did_hit) {
                     result["entity"] = hit.entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), hit.entity));
                     result["tile"] = hit.tile;
                     result["point"] = Json::array({hit.point.x, hit.point.y});
                     result["normal"] = Json::array({hit.normal.x, hit.normal.y});
                     result["distance"] = hit.distance;
                     if (hit.tile) result["tile_coordinates"] = Json::array({hit.tile_x, hit.tile_y});
                 }
                 return result;
             }
             if (kind == "overlap_box") {
                 const sprite2d::Vec2 center = read_vec2("center");
                 const sprite2d::Vec2 half_extents = read_vec2("half_extents");
                 if (half_extents.x <= 0.0f || half_extents.y <= 0.0f) throw ToolError("\"half_extents\" must contain positive values");
                 Json entities = Json::array();
                 for (Entity entity : physics->OverlapBox(center, half_extents, mask)) {
                     entities.push_back(entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), entity)));
                 }
                 return {{"kind", kind}, {"count", entities.size()}, {"entities", std::move(entities)}};
             }
             throw ToolError("\"kind\" must be raycast or overlap_box");
         }});

    server.AddTool(
        {"game_physics2d_queries_many",
         "Run 1 to 64 validated 2D raycast or axis-aligned box-overlap queries in one request. Each query can set its own 32-bit collision-layer mask; raycasts include solid-tile hit details.",
         Schema({{"queries", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                               {"items", {{"type", "object"},
                                          {"properties", {{"kind", {{"type", "string"}, {"enum", Json::array({"raycast", "overlap_box"})}},
                                                          {"origin", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                                                          {"direction", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                                                          {"max_distance", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                                                          {"center", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                                                          {"half_extents", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                                                          {"mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}}},
                                          {"required", Json::array({"kind"})}}}}}},
                {"queries"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             sprite2d::Physics2D* physics = game.Physics2D();
             if (physics == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (!args.contains("queries") || !args["queries"].is_array() || args["queries"].empty() || args["queries"].size() > 64) {
                 throw ToolError("\"queries\" must be an array of 1 to 64 2D physics queries");
             }
             struct Physics2DQuery {
                 std::string kind;
                 sprite2d::Vec2 origin{};
                 sprite2d::Vec2 direction{};
                 f32 max_distance = 0.0f;
                 sprite2d::Vec2 center{};
                 sprite2d::Vec2 half_extents{};
                 u32 mask = 0xFFFFFFFFu;
             };
             auto read_vec2 = [](const Json& query, const char* field) {
                 if (!query.contains(field) || !query[field].is_array() || query[field].size() != 2 ||
                     !query[field][0].is_number() || !query[field][1].is_number()) throw ToolError(std::string("\"") + field + "\" must be [x, y]");
                 const sprite2d::Vec2 value{query[field][0].get<f32>(), query[field][1].get<f32>()};
                 if (!std::isfinite(value.x) || !std::isfinite(value.y)) throw ToolError(std::string("\"") + field + "\" values must be finite");
                 return value;
             };
             std::vector<Physics2DQuery> parsed;
             parsed.reserve(args["queries"].size());
             for (const Json& raw : args["queries"]) {
                 if (!raw.is_object() || !raw.contains("kind") || !raw["kind"].is_string()) throw ToolError("Every query must contain a string \"kind\"");
                 Physics2DQuery query;
                 query.kind = raw["kind"].get<std::string>();
                 if (query.kind == "raycast") {
                     query.origin = read_vec2(raw, "origin");
                     query.direction = read_vec2(raw, "direction");
                     if (!raw.contains("max_distance") || !raw["max_distance"].is_number()) throw ToolError("Raycasts require a finite positive \"max_distance\"");
                     query.max_distance = raw["max_distance"].get<f32>();
                     if (!std::isfinite(query.max_distance) || query.max_distance <= 0.0f) throw ToolError("Raycasts require a finite positive \"max_distance\"");
                 } else if (query.kind == "overlap_box") {
                     query.center = read_vec2(raw, "center");
                     query.half_extents = read_vec2(raw, "half_extents");
                     if (query.half_extents.x <= 0.0f || query.half_extents.y <= 0.0f) throw ToolError("\"half_extents\" must contain positive values");
                 } else {
                     throw ToolError("Query kind must be raycast or overlap_box");
                 }
                 if (raw.contains("mask")) {
                     if (!raw["mask"].is_number_integer()) throw ToolError("\"mask\" must be an integer from 0 through 4294967295");
                     u64 value = 0;
                     if (raw["mask"].is_number_unsigned()) value = raw["mask"].get<u64>();
                     else {
                         const i64 signed_value = raw["mask"].get<i64>();
                         if (signed_value < 0) throw ToolError("\"mask\" must be an integer from 0 through 4294967295");
                         value = static_cast<u64>(signed_value);
                     }
                     if (value > std::numeric_limits<u32>::max()) throw ToolError("\"mask\" must be an integer from 0 through 4294967295");
                     query.mask = static_cast<u32>(value);
                 }
                 parsed.push_back(query);
             }
             Json results = Json::array();
             for (usize i = 0; i < parsed.size(); ++i) {
                 const Physics2DQuery& query = parsed[i];
                 if (query.kind == "raycast") {
                     sprite2d::RayHit2D hit;
                     const bool did_hit = physics->Raycast(query.origin, query.direction, query.max_distance, hit, query.mask);
                     Json result = {{"index", i}, {"kind", query.kind}, {"hit", did_hit}};
                     if (did_hit) {
                         result["entity"] = hit.entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), hit.entity));
                         result["tile"] = hit.tile;
                         result["point"] = Json::array({hit.point.x, hit.point.y});
                         result["normal"] = Json::array({hit.normal.x, hit.normal.y});
                         result["distance"] = hit.distance;
                         if (hit.tile) result["tile_coordinates"] = Json::array({hit.tile_x, hit.tile_y});
                     }
                     results.push_back(std::move(result));
                 } else {
                     Json entities = Json::array();
                     for (Entity entity : physics->OverlapBox(query.center, query.half_extents, query.mask)) {
                         entities.push_back(entity.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), entity)));
                     }
                     results.push_back({{"index", i}, {"kind", query.kind}, {"count", entities.size()}, {"entities", std::move(entities)}});
                 }
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});

    server.AddTool(
        {"game_physics_events",
         "Read the most recent 2D and 3D collision and trigger events from the loaded game. Events represent transitions from its latest physics step and include entity handles and contact details where available.",
         Schema({{"dimension", {{"type", "string"}, {"enum", Json::array({"all", "2d", "3d"})}, {"default", "all"}}}),
                {}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             const std::string dimension = args.contains("dimension")
                 ? (args["dimension"].is_string() ? args["dimension"].get<std::string>() : throw ToolError("\"dimension\" must be all, 2d or 3d"))
                 : "all";
             if (dimension != "all" && dimension != "2d" && dimension != "3d") {
                 throw ToolError("\"dimension\" must be all, 2d or 3d");
             }

             Json events = Json::array();
             bool has_2d = false;
             bool has_3d = false;
             if (dimension != "3d") {
                 if (sprite2d::Physics2D* physics = game.Physics2D()) {
                     has_2d = true;
                     for (const sprite2d::Event2D& event : physics->Events()) {
                         const char* type = "begin";
                         switch (event.kind) {
                         case sprite2d::Event2D::Kind::Begin: type = "begin"; break;
                         case sprite2d::Event2D::Kind::End: type = "end"; break;
                         case sprite2d::Event2D::Kind::TriggerEnter: type = "trigger_enter"; break;
                         case sprite2d::Event2D::Kind::TriggerExit: type = "trigger_exit"; break;
                         }
                         events.push_back({{"dimension", "2d"}, {"type", type},
                                           {"entity_a", event.a.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), event.a))},
                                           {"entity_b", event.b.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), event.b))},
                                           {"normal", Json::array({event.normal.x, event.normal.y})}});
                     }
                 }
             }
#if AETHER_MCP_PHYSICS
             if (dimension != "2d") {
                 if (PhysicsScene* scene = game.PhysicsQueries()) {
                     has_3d = true;
                     for (const ContactEvent& event : scene->Physics().Contacts()) {
                         const char* type = "begin";
                         switch (event.type) {
                         case ContactType::Begin: type = "begin"; break;
                         case ContactType::Stay: type = "stay"; break;
                         case ContactType::End: type = "end"; break;
                         }
                         const Entity entity_a = scene->EntityOf(event.body1);
                         const Entity entity_b = scene->EntityOf(event.body2);
                         events.push_back({{"dimension", "3d"}, {"type", type}, {"trigger", event.trigger},
                                           {"entity_a", entity_a.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), entity_a))},
                                           {"entity_b", entity_b.IsNull() ? Json(nullptr) : Json(EntityKey(game.GetWorld(), entity_b))},
                                           {"point", Json::array({event.point.x, event.point.y, event.point.z})},
                                           {"normal", Json::array({event.normal.x, event.normal.y, event.normal.z})},
                                           {"approach_speed", event.approach_speed}});
                     }
                 }
             }
#endif
             return {{"dimension", dimension}, {"count", events.size()}, {"events", std::move(events)},
                     {"available", {{"2d", has_2d}, {"3d", has_3d}}}};
         }});

    server.AddTool(
        {"game_physics2d_state_many",
         "Read platformer movement state for up to 64 live entities: whether each is grounded or touching a wall, its wall side, and its 2D rigidbody velocity when present.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}}, {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             sprite2d::Physics2D* physics = game.Physics2D();
             if (physics == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }

             World& world = game.GetWorld();
             std::vector<Entity> requested;
             requested.reserve(args["entities"].size());
             Json results = Json::array();
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(requested.begin(), requested.end(), entity) != requested.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 requested.push_back(entity);

                 const sprite2d::Rigidbody2D* body = world.GetComponent<sprite2d::Rigidbody2D>(entity);
                 const bool has_collider = world.GetComponent<sprite2d::Collider2D>(entity) != nullptr;
                 const int wall_side = physics->WallSide(entity);
                 Json result = {{"entity", EntityKey(world, entity)},
                                {"has_rigidbody", body != nullptr},
                                {"has_collider", has_collider},
                                {"grounded", physics->IsGrounded(entity)},
                                {"wall_side", wall_side},
                                {"touching_wall", wall_side != 0}};
                 if (body != nullptr) result["velocity"] = Json::array({body->velocity.x, body->velocity.y});
                 else result["velocity"] = nullptr;
                 results.push_back(std::move(result));
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"game_physics2d_settings",
         "Read gravity and solver settings for the loaded game's 2D physics scene.",
         Schema(Json::object()),
         [host](const Json&) -> Json {
             Game& game = host->Require();
             sprite2d::Physics2D* physics = game.Physics2D();
             if (physics == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             const sprite2d::Physics2DSettings& settings = physics->settings;
             return {{"gravity", Json::array({settings.gravity.x, settings.gravity.y})},
                     {"iterations", settings.iterations}, {"slop", settings.slop},
                     {"correction", settings.correction}, {"max_step", settings.max_step}};
         }});

    server.AddTool(
        {"game_set_physics2d_settings",
         "Update selected 2D physics scene settings. Validates the complete resulting settings before applying; iterations must be 1-64, slop non-negative, correction 0-1 and max_step positive.",
         Schema({{"gravity", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"iterations", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                 {"slop", {{"type", "number"}, {"minimum", 0}}},
                 {"correction", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                 {"max_step", {{"type", "number"}, {"exclusiveMinimum", 0}}}},
                {}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             sprite2d::Physics2D* physics = game.Physics2D();
             if (physics == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (args.empty()) throw ToolError("Provide at least one 2D physics setting to update");
             static const std::unordered_set<std::string> supported = {"gravity", "iterations", "slop", "correction", "max_step"};
             for (auto it = args.begin(); it != args.end(); ++it) {
                 if (!supported.contains(it.key())) throw ToolError("Unsupported 2D physics setting: " + it.key());
             }
             sprite2d::Physics2DSettings updated = physics->settings;
             if (args.contains("gravity")) {
                 const Json& value = args["gravity"];
                 if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
                     throw ToolError("\"gravity\" must be [x, y]");
                 }
                 updated.gravity = {value[0].get<f32>(), value[1].get<f32>()};
                 if (!std::isfinite(updated.gravity.x) || !std::isfinite(updated.gravity.y)) throw ToolError("Gravity components must be finite");
             }
             if (args.contains("iterations")) {
                 const Json& value = args["iterations"];
                 if (!value.is_number_integer()) throw ToolError("\"iterations\" must be an integer from 1 through 64");
                 u64 iterations = 0;
                 if (value.is_number_unsigned()) iterations = value.get<u64>();
                 else {
                     const i64 signed_iterations = value.get<i64>();
                     if (signed_iterations < 1) throw ToolError("\"iterations\" must be an integer from 1 through 64");
                     iterations = static_cast<u64>(signed_iterations);
                 }
                 if (iterations < 1 || iterations > 64) throw ToolError("\"iterations\" must be an integer from 1 through 64");
                 updated.iterations = static_cast<u32>(iterations);
             }
             if (args.contains("slop")) {
                 if (!args["slop"].is_number()) throw ToolError("\"slop\" must be a finite non-negative number");
                 updated.slop = args["slop"].get<f32>();
                 if (!std::isfinite(updated.slop) || updated.slop < 0.0f) throw ToolError("\"slop\" must be a finite non-negative number");
             }
             if (args.contains("correction")) {
                 if (!args["correction"].is_number()) throw ToolError("\"correction\" must be a finite number from 0 through 1");
                 updated.correction = args["correction"].get<f32>();
                 if (!std::isfinite(updated.correction) || updated.correction < 0.0f || updated.correction > 1.0f) {
                     throw ToolError("\"correction\" must be a finite number from 0 through 1");
                 }
             }
             if (args.contains("max_step")) {
                 if (!args["max_step"].is_number()) throw ToolError("\"max_step\" must be a finite positive number");
                 updated.max_step = args["max_step"].get<f32>();
                 if (!std::isfinite(updated.max_step) || updated.max_step <= 0.0f) throw ToolError("\"max_step\" must be a finite positive number");
             }
             physics->settings = updated;
             return {{"gravity", Json::array({updated.gravity.x, updated.gravity.y})},
                     {"iterations", updated.iterations}, {"slop", updated.slop},
                     {"correction", updated.correction}, {"max_step", updated.max_step}};
         }});

    server.AddTool(
        {"game_set_physics2d_velocities_many",
         "Set the 2D rigidbody velocity of up to 64 unique live entities. Validates every entity, component and finite velocity before applying any changes; preserves all other rigidbody settings.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", {{"type", "string"}}},
                                                        {"velocity", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}}}},
                                        {"required", Json::array({"entity", "velocity"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             if (game.Physics2D() == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (!args.contains("edits") || !args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 2D velocity edits");
             }
             World& world = game.GetWorld();
             struct VelocityEdit { Entity entity; sprite2d::Vec2 velocity; };
             std::vector<VelocityEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& raw : args["edits"]) {
                 if (!raw.is_object() || !raw.contains("entity") || !raw["entity"].is_string() || raw["entity"].get<std::string>().empty()) {
                     throw ToolError("Every edit requires a non-empty entity handle");
                 }
                 if (!raw.contains("velocity") || !raw["velocity"].is_array() || raw["velocity"].size() != 2 ||
                     !raw["velocity"][0].is_number() || !raw["velocity"][1].is_number()) {
                     throw ToolError("Every \"velocity\" must be [x, y]");
                 }
                 const sprite2d::Vec2 velocity{raw["velocity"][0].get<f32>(), raw["velocity"][1].get<f32>()};
                 if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y)) throw ToolError("Velocity components must be finite");
                 const Entity entity = FindEntity(game, raw["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const VelocityEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<sprite2d::Rigidbody2D>(entity) == nullptr) throw ToolError("Every entity must have a Rigidbody2D component");
                 edits.push_back({entity, velocity});
             }
             Json results = Json::array();
             for (const VelocityEdit& edit : edits) {
                 sprite2d::Rigidbody2D* body = world.GetComponent<sprite2d::Rigidbody2D>(edit.entity);
                 body->velocity = edit.velocity;
                 results.push_back({{"entity", EntityKey(world, edit.entity)}, {"velocity", Json::array({edit.velocity.x, edit.velocity.y})}});
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"game_set_physics2d_velocity_shared_many",
         "Set one shared 2D velocity on up to 64 unique live Rigidbody2D entities. Validates every entity and component before applying; preserves all other body settings.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"velocity", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}}},
                {"entities", "velocity"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             if (game.Physics2D() == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["velocity"].is_array() || args["velocity"].size() != 2 ||
                 !args["velocity"][0].is_number() || !args["velocity"][1].is_number()) {
                 throw ToolError("\"velocity\" must be [x, y]");
             }
             const sprite2d::Vec2 velocity{args["velocity"][0].get<f32>(), args["velocity"][1].get<f32>()};
             if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y)) throw ToolError("Velocity components must be finite");
             World& world = game.GetWorld();
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) throw ToolError("\"entities\" must not repeat an entity");
                 if (world.GetComponent<sprite2d::Rigidbody2D>(entity) == nullptr) throw ToolError("Every entity must have a Rigidbody2D component");
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 world.GetComponent<sprite2d::Rigidbody2D>(entity)->velocity = velocity;
                 results.push_back({{"entity", EntityKey(world, entity)}, {"velocity", Json::array({velocity.x, velocity.y})}});
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"game_set_physics2d_colliders_many",
         "Apply partial Collider2D edits to up to 64 unique entities. Validates every resulting collider before applying any edit; unspecified settings are preserved.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", {{"type", "string"}}},
                                                        {"shape", {{"type", "string"}, {"enum", Json::array({"box", "circle"})}}},
                                                        {"half_extents", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                                                        {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                                                        {"offset", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                                                        {"friction", {{"type", "number"}, {"minimum", 0}, {"maximum", 2}}},
                                                        {"restitution", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                                                        {"trigger", {{"type", "boolean"}}},
                                                        {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}},
                                                        {"mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}}}},
                                        {"required", Json::array({"entity"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             if (game.Physics2D() == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (!args.contains("edits") || !args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 collider edits");
             }
             World& world = game.GetWorld();
             struct ColliderEdit { Entity entity; sprite2d::Collider2D collider; };
             static const std::unordered_set<std::string> supported = {"entity", "shape", "half_extents", "radius", "offset", "friction", "restitution", "trigger", "layer", "mask"};
             auto read_vec2 = [](const Json& value, const char* field) {
                 if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
                     throw ToolError(std::string("\"") + field + "\" must be [x, y]");
                 }
                 const sprite2d::Vec2 result{value[0].get<f32>(), value[1].get<f32>()};
                 if (!std::isfinite(result.x) || !std::isfinite(result.y)) throw ToolError(std::string("\"") + field + "\" values must be finite");
                 return result;
             };
             auto read_u32 = [](const Json& value, const char* field) {
                 if (!value.is_number_integer()) throw ToolError(std::string("\"") + field + "\" must be an integer from 0 through 4294967295");
                 u64 parsed_value = 0;
                 if (value.is_number_unsigned()) parsed_value = value.get<u64>();
                 else {
                     const i64 signed_value = value.get<i64>();
                     if (signed_value < 0) throw ToolError(std::string("\"") + field + "\" must be an integer from 0 through 4294967295");
                     parsed_value = static_cast<u64>(signed_value);
                 }
                 if (parsed_value > std::numeric_limits<u32>::max()) throw ToolError(std::string("\"") + field + "\" must be an integer from 0 through 4294967295");
                 return static_cast<u32>(parsed_value);
             };
             std::vector<ColliderEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& raw : args["edits"]) {
                 if (!raw.is_object() || !raw.contains("entity") || !raw["entity"].is_string() || raw["entity"].get<std::string>().empty()) {
                     throw ToolError("Every collider edit requires a non-empty entity handle");
                 }
                 for (auto it = raw.begin(); it != raw.end(); ++it) {
                     if (!supported.contains(it.key())) throw ToolError("Unsupported Collider2D field: " + it.key());
                 }
                 const Entity entity = FindEntity(game, raw["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ColliderEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 const sprite2d::Collider2D* current = world.GetComponent<sprite2d::Collider2D>(entity);
                 if (current == nullptr) throw ToolError("Every entity must have a Collider2D component");
                 sprite2d::Collider2D updated = *current;
                 if (raw.contains("shape")) {
                     if (!raw["shape"].is_string()) throw ToolError("\"shape\" must be box or circle");
                     const std::string shape = raw["shape"].get<std::string>();
                     if (shape == "box") updated.shape = sprite2d::Shape2D::Box;
                     else if (shape == "circle") updated.shape = sprite2d::Shape2D::Circle;
                     else throw ToolError("\"shape\" must be box or circle");
                 }
                 if (raw.contains("half_extents")) updated.half_extents = read_vec2(raw["half_extents"], "half_extents");
                 if (raw.contains("radius")) {
                     if (!raw["radius"].is_number()) throw ToolError("\"radius\" must be a finite positive number");
                     updated.radius = raw["radius"].get<f32>();
                     if (!std::isfinite(updated.radius) || updated.radius <= 0.0f) throw ToolError("\"radius\" must be a finite positive number");
                 }
                 if (raw.contains("offset")) updated.offset = read_vec2(raw["offset"], "offset");
                 if (raw.contains("friction")) {
                     if (!raw["friction"].is_number()) throw ToolError("\"friction\" must be a finite number from 0 through 2");
                     updated.friction = raw["friction"].get<f32>();
                 }
                 if (raw.contains("restitution")) {
                     if (!raw["restitution"].is_number()) throw ToolError("\"restitution\" must be a finite number from 0 through 1");
                     updated.restitution = raw["restitution"].get<f32>();
                 }
                 if (raw.contains("trigger")) {
                     if (!raw["trigger"].is_boolean()) throw ToolError("\"trigger\" must be a boolean");
                     updated.trigger = raw["trigger"].get<bool>();
                 }
                 if (raw.contains("layer")) updated.layer = read_u32(raw["layer"], "layer");
                 if (raw.contains("mask")) updated.mask = read_u32(raw["mask"], "mask");
                 if (!std::isfinite(updated.half_extents.x) || !std::isfinite(updated.half_extents.y) ||
                     !std::isfinite(updated.offset.x) || !std::isfinite(updated.offset.y) ||
                     !std::isfinite(updated.radius) || !std::isfinite(updated.friction) || !std::isfinite(updated.restitution) ||
                     updated.friction < 0.0f || updated.friction > 2.0f || updated.restitution < 0.0f || updated.restitution > 1.0f ||
                     (updated.shape != sprite2d::Shape2D::Box && updated.shape != sprite2d::Shape2D::Circle) ||
                     (updated.shape == sprite2d::Shape2D::Box && (updated.half_extents.x <= 0.0f || updated.half_extents.y <= 0.0f)) ||
                     (updated.shape == sprite2d::Shape2D::Circle && updated.radius <= 0.0f)) {
                     throw ToolError("The resulting collider has invalid geometry or material values");
                 }
                 edits.push_back({entity, updated});
             }
             Json results = Json::array();
             for (const ColliderEdit& edit : edits) {
                 *world.GetComponent<sprite2d::Collider2D>(edit.entity) = edit.collider;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"shape", edit.collider.shape == sprite2d::Shape2D::Box ? "box" : "circle"},
                                    {"half_extents", Json::array({edit.collider.half_extents.x, edit.collider.half_extents.y})},
                                    {"radius", edit.collider.radius},
                                    {"offset", Json::array({edit.collider.offset.x, edit.collider.offset.y})},
                                    {"friction", edit.collider.friction}, {"restitution", edit.collider.restitution},
                                    {"trigger", edit.collider.trigger}, {"layer", edit.collider.layer}, {"mask", edit.collider.mask}});
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"game_set_physics2d_colliders_shared_many",
         "Apply the same partial Collider2D patch to up to 64 unique entities. Validates every resulting collider before applying any change; unspecified fields retain their per-entity values.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"shape", {{"type", "string"}, {"enum", Json::array({"box", "circle"})}},
                 {"half_extents", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"radius", {{"type", "number"}, {"exclusiveMinimum", 0}}},
                 {"offset", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", {{"type", "number"}}}}},
                 {"friction", {{"type", "number"}, {"minimum", 0}, {"maximum", 2}}},
                 {"restitution", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                 {"trigger", {{"type", "boolean"}}},
                 {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}},
                 {"mask", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295.0}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             if (game.Physics2D() == nullptr) throw ToolError("The loaded game has no 2D physics scene");
             if (!args.contains("entities") || !args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             static const std::unordered_set<std::string> supported = {"entities", "shape", "half_extents", "radius", "offset", "friction", "restitution", "trigger", "layer", "mask"};
             for (auto it = args.begin(); it != args.end(); ++it) {
                 if (!supported.contains(it.key())) throw ToolError("Unsupported Collider2D field: " + it.key());
             }
             bool has_patch = false;
             for (const char* field : {"shape", "half_extents", "radius", "offset", "friction", "restitution", "trigger", "layer", "mask"}) {
                 has_patch = has_patch || args.contains(field);
             }
             if (!has_patch) throw ToolError("Provide at least one Collider2D field to update");
             struct ColliderPatch {
                 std::optional<sprite2d::Shape2D> shape;
                 std::optional<sprite2d::Vec2> half_extents, offset;
                 std::optional<f32> radius, friction, restitution;
                 std::optional<bool> trigger;
                 std::optional<u32> layer, mask;
             } patch;
             auto read_vec2 = [](const Json& value, const char* field) {
                 if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
                     throw ToolError(std::string("\"") + field + "\" must be [x, y]");
                 }
                 const sprite2d::Vec2 result{value[0].get<f32>(), value[1].get<f32>()};
                 if (!std::isfinite(result.x) || !std::isfinite(result.y)) throw ToolError(std::string("\"") + field + "\" values must be finite");
                 return result;
             };
             auto read_number = [](const Json& value, const char* field) {
                 if (!value.is_number()) throw ToolError(std::string("\"") + field + "\" must be a finite number");
                 const f32 result = value.get<f32>();
                 if (!std::isfinite(result)) throw ToolError(std::string("\"") + field + "\" must be a finite number");
                 return result;
             };
             auto read_u32 = [](const Json& value, const char* field) {
                 if (!value.is_number_integer()) throw ToolError(std::string("\"") + field + "\" must be an integer from 0 through 4294967295");
                 u64 parsed_value = 0;
                 if (value.is_number_unsigned()) parsed_value = value.get<u64>();
                 else {
                     const i64 signed_value = value.get<i64>();
                     if (signed_value < 0) throw ToolError(std::string("\"") + field + "\" must be an integer from 0 through 4294967295");
                     parsed_value = static_cast<u64>(signed_value);
                 }
                 if (parsed_value > std::numeric_limits<u32>::max()) throw ToolError(std::string("\"") + field + "\" must be an integer from 0 through 4294967295");
                 return static_cast<u32>(parsed_value);
             };
             if (args.contains("shape")) {
                 if (!args["shape"].is_string()) throw ToolError("\"shape\" must be box or circle");
                 const std::string shape = args["shape"].get<std::string>();
                 if (shape == "box") patch.shape = sprite2d::Shape2D::Box;
                 else if (shape == "circle") patch.shape = sprite2d::Shape2D::Circle;
                 else throw ToolError("\"shape\" must be box or circle");
             }
             if (args.contains("half_extents")) patch.half_extents = read_vec2(args["half_extents"], "half_extents");
             if (args.contains("radius")) {
                 patch.radius = read_number(args["radius"], "radius");
                 if (*patch.radius <= 0.0f) throw ToolError("\"radius\" must be positive");
             }
             if (args.contains("offset")) patch.offset = read_vec2(args["offset"], "offset");
             if (args.contains("friction")) patch.friction = read_number(args["friction"], "friction");
             if (args.contains("restitution")) patch.restitution = read_number(args["restitution"], "restitution");
             if (args.contains("trigger")) {
                 if (!args["trigger"].is_boolean()) throw ToolError("\"trigger\" must be a boolean");
                 patch.trigger = args["trigger"].get<bool>();
             }
             if (args.contains("layer")) patch.layer = read_u32(args["layer"], "layer");
             if (args.contains("mask")) patch.mask = read_u32(args["mask"], "mask");

             World& world = game.GetWorld();
             struct ColliderEdit { Entity entity; sprite2d::Collider2D collider; };
             std::vector<ColliderEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ColliderEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 const sprite2d::Collider2D* current = world.GetComponent<sprite2d::Collider2D>(entity);
                 if (current == nullptr) throw ToolError("Every entity must have a Collider2D component");
                 sprite2d::Collider2D updated = *current;
                 if (patch.shape) updated.shape = *patch.shape;
                 if (patch.half_extents) updated.half_extents = *patch.half_extents;
                 if (patch.radius) updated.radius = *patch.radius;
                 if (patch.offset) updated.offset = *patch.offset;
                 if (patch.friction) updated.friction = *patch.friction;
                 if (patch.restitution) updated.restitution = *patch.restitution;
                 if (patch.trigger) updated.trigger = *patch.trigger;
                 if (patch.layer) updated.layer = *patch.layer;
                 if (patch.mask) updated.mask = *patch.mask;
                 if (!std::isfinite(updated.half_extents.x) || !std::isfinite(updated.half_extents.y) ||
                     !std::isfinite(updated.offset.x) || !std::isfinite(updated.offset.y) ||
                     !std::isfinite(updated.radius) || !std::isfinite(updated.friction) || !std::isfinite(updated.restitution) ||
                     updated.friction < 0.0f || updated.friction > 2.0f || updated.restitution < 0.0f || updated.restitution > 1.0f ||
                     (updated.shape != sprite2d::Shape2D::Box && updated.shape != sprite2d::Shape2D::Circle) ||
                     (updated.shape == sprite2d::Shape2D::Box && (updated.half_extents.x <= 0.0f || updated.half_extents.y <= 0.0f)) ||
                     (updated.shape == sprite2d::Shape2D::Circle && updated.radius <= 0.0f)) {
                     throw ToolError("The resulting collider has invalid geometry or material values");
                 }
                 edits.push_back({entity, updated});
             }
             Json results = Json::array();
             for (const ColliderEdit& edit : edits) {
                 *world.GetComponent<sprite2d::Collider2D>(edit.entity) = edit.collider;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"shape", edit.collider.shape == sprite2d::Shape2D::Box ? "box" : "circle"},
                                    {"half_extents", Json::array({edit.collider.half_extents.x, edit.collider.half_extents.y})},
                                    {"radius", edit.collider.radius}, {"offset", Json::array({edit.collider.offset.x, edit.collider.offset.y})},
                                    {"friction", edit.collider.friction}, {"restitution", edit.collider.restitution},
                                    {"trigger", edit.collider.trigger}, {"layer", edit.collider.layer}, {"mask", edit.collider.mask}});
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"game_reparent_entities_many",
         "Set or clear the parent of up to 64 unique live entities as one batch. By default parent changes retain local transforms; set preserve_world=true to retain world poses by updating local transforms. The batch validates stable parent GUIDs and the complete resulting hierarchy, and rejects cycles, excessive depth, singular transforms, or unrepresentable shear before applying.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}}, {"parent", {{"type", "string"}}}}},
                                          {"required", Json::array({"entity", "parent"})}}}},
                 {"preserve_world", {{"type", "boolean"}, {"default", false}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 reparent edits");
             }
             const bool preserve_world = args.contains("preserve_world") ?
                 (args["preserve_world"].is_boolean() ? args["preserve_world"].get<bool>() : throw ToolError("\"preserve_world\" must be a boolean")) : false;
             struct ParentEdit { Entity entity; Entity parent; EntityGuid parent_guid; Transform preserved_local; bool has_preserved_local = false; };
             std::vector<ParentEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("parent") || !item["parent"].is_string()) throw ToolError("Every \"parent\" must be a GUID string or an empty string for a root");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ParentEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 Entity parent = kNullEntity;
                 EntityGuid parent_guid{};
                 const std::string parent_key = item["parent"].get<std::string>();
                 if (!parent_key.empty()) {
                     parent = FindEntity(game, parent_key);
                     const IdComponent* id = world.GetComponent<IdComponent>(parent);
                     if (id == nullptr || id->guid.IsNull() || game.Guids().Find(world, id->guid) != parent) {
                         throw ToolError("New parent " + parent_key + " must have an indexed stable GUID");
                     }
                     parent_guid = id->guid;
                 }
                 edits.push_back({entity, parent, parent_guid, {}, false});
             }
             for (const ParentEdit& edit : edits) {
                 std::vector<Entity> visited{edit.entity};
                 Entity current = edit.parent;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Resulting hierarchy exceeds the supported depth");
                     if (std::find(visited.begin(), visited.end(), current) != visited.end()) throw ToolError("Reparenting would create a hierarchy cycle");
                     visited.push_back(current);
                     const auto changed = std::find_if(edits.begin(), edits.end(), [current](const ParentEdit& candidate) { return candidate.entity == current; });
                     current = changed != edits.end() ? changed->parent : GetParent(world, game.Guids(), current);
                 }
             }
             if (preserve_world) {
                 auto finite_matrix = [](const Mat4& matrix) {
                     for (const Vec4& column : matrix.cols) {
                         if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) return false;
                     }
                     return true;
                 };
                 auto rotation_from_basis = [](const Vec3& x, const Vec3& y, const Vec3& z) {
                     const f32 m00 = x.x, m01 = y.x, m02 = z.x;
                     const f32 m10 = x.y, m11 = y.y, m12 = z.y;
                     const f32 m20 = x.z, m21 = y.z, m22 = z.z;
                     Quaternion q;
                     const f32 trace = m00 + m11 + m22;
                     if (trace > 0.0f) {
                         const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
                         q = Quaternion((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
                     } else if (m00 > m11 && m00 > m22) {
                         const f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                         q = Quaternion(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
                     } else if (m11 > m22) {
                         const f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                         q = Quaternion((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
                     } else {
                         const f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                         q = Quaternion((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
                     }
                     return q.Normalized();
                 };
                 for (ParentEdit& edit : edits) {
                     if (world.GetComponent<Transform>(edit.entity) == nullptr) continue;
                     for (Entity ancestor = edit.parent; !ancestor.IsNull(); ancestor = GetParent(world, game.Guids(), ancestor)) {
                         if (std::find_if(edits.begin(), edits.end(), [ancestor](const ParentEdit& candidate) { return candidate.entity == ancestor; }) != edits.end()) {
                             throw ToolError("preserve_world cannot target a parent whose hierarchy is also being reparented in this batch");
                         }
                     }
                     const Mat4 old_world = ComputeWorldTransform(world, game.Guids(), edit.entity);
                     Mat4 parent_world;
                     if (!edit.parent.IsNull() && world.GetComponent<Transform>(edit.parent) != nullptr) {
                         parent_world = ComputeWorldTransform(world, game.Guids(), edit.parent);
                     }
                     if (!finite_matrix(old_world) || !finite_matrix(parent_world)) throw ToolError("Cannot preserve a non-finite world transform");
                     Mat4 inverse_parent;
                     if (!parent_world.TryInverse(inverse_parent)) throw ToolError("Cannot preserve world pose through a singular parent transform");
                     const Mat4 local = inverse_parent * old_world;
                     if (!finite_matrix(local) || std::abs(local.cols[0].w) > 1.0e-4f || std::abs(local.cols[1].w) > 1.0e-4f ||
                         std::abs(local.cols[2].w) > 1.0e-4f || std::abs(local.cols[3].w - 1.0f) > 1.0e-4f) {
                         throw ToolError("Cannot preserve world pose because the resulting local transform is not affine");
                     }
                     Vec3 x(local.cols[0].x, local.cols[0].y, local.cols[0].z);
                     Vec3 y(local.cols[1].x, local.cols[1].y, local.cols[1].z);
                     Vec3 z(local.cols[2].x, local.cols[2].y, local.cols[2].z);
                     Vec3 scale(x.Length(), y.Length(), z.Length());
                     if (scale.x <= 1.0e-8f || scale.y <= 1.0e-8f || scale.z <= 1.0e-8f) throw ToolError("Cannot preserve world pose with a zero-scale local transform");
                     x = x * (1.0f / scale.x);
                     y = y * (1.0f / scale.y);
                     z = z * (1.0f / scale.z);
                     if (std::abs(x.Dot(y)) > 1.0e-4f || std::abs(x.Dot(z)) > 1.0e-4f || std::abs(y.Dot(z)) > 1.0e-4f) {
                         throw ToolError("Cannot preserve world pose because the reparenting would require shear");
                     }
                     if (x.Cross(y).Dot(z) < 0.0f) { x = x * -1.0f; scale.x = -scale.x; }
                     Transform local_transform;
                     local_transform.position = Vec3(local.cols[3].x, local.cols[3].y, local.cols[3].z);
                     local_transform.rotation = rotation_from_basis(x, y, z);
                     local_transform.scale = scale;
                     edit.preserved_local = local_transform;
                     edit.has_preserved_local = true;
                 }
             }
             Json results = Json::array();
             for (ParentEdit& edit : edits) {
                 if (edit.parent.IsNull()) {
                     if (world.GetComponent<Parent>(edit.entity) != nullptr) world.RemoveComponent<Parent>(edit.entity);
                 } else if (Parent* component = world.GetComponent<Parent>(edit.entity)) {
                     component->parent = edit.parent_guid;
                 } else {
                     world.AddComponent<Parent>(edit.entity, Parent{edit.parent_guid});
                 }
                 if (edit.has_preserved_local) *world.GetComponent<Transform>(edit.entity) = edit.preserved_local;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"parent", edit.parent.IsNull() ? Json(nullptr) : Json(EntityKey(world, edit.parent))}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
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
             Json out = EntityJson(game, e);
             if (!report.warnings.empty()) out["warnings"] = report.warnings;
             return out;
         }});

    server.AddTool({"game_attributes", "The attributes (name, base, current, min, max) of an entity in the running game.",
                    Schema({{"entity", {{"type", "string"}, {"description", "GUID or #index[:generation]"}}}}, {"entity"}), [host](const Json& args) -> Json {
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
        {"game_attributes_many",
         "Read attributes (name, base, current, min, max) for up to 64 unique live entities. Entities without an AttributeSet return an empty attribute list.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}}, {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) throw ToolError("\"entities\" must not contain the same entity more than once");
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 Json attributes = Json::array();
                 const gas::AttributeSet* set = world.GetComponent<gas::AttributeSet>(entity);
                 if (set != nullptr) {
                     for (const gas::Attribute& attribute : set->attributes) {
                         attributes.push_back({{"name", attribute.name}, {"base", attribute.base}, {"current", attribute.current},
                                               {"min", attribute.min}, {"max", attribute.max}});
                     }
                 }
                 results.push_back({{"entity", EntityKey(world, entity)}, {"entity_index", entity.index}, {"attributes", attributes}});
             }
             return {{"count", results.size()}, {"results", results}};
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

    server.AddTool(
        {"game_set_attributes_many",
         "Apply different base, delta or define operations to up to 64 unique live entity/attribute pairs through the gameplay attribute system, preserving bounds and change events. Validates the whole request first.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}}, {"name", {{"type", "string"}}},
                                                          {"base", {{"type", "number"}}}, {"delta", {{"type", "number"}}},
                                                          {"define", {{"type", "object"}}}}},
                                         {"required", Json::array({"entity", "name"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             if (gas::AttributeSystem::Active() == nullptr) throw ToolError("This game has no attribute system (load a scene first)");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 entity/attribute changes");
             }
             struct AttributeEdit {
                 Entity entity;
                 std::string name;
                 std::string mode;
                 f32 value = 0.0f;
                 f32 min = -3.0e38f;
                 f32 max = 3.0e38f;
             };
             auto number = [](const Json& object, const char* key, f32 fallback) {
                 if (!object.contains(key)) return fallback;
                 if (!object[key].is_number()) throw ToolError(std::string("\"") + key + "\" must be a number");
                 return object[key].get<f32>();
             };
             std::vector<AttributeEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("name") || !item["name"].is_string() || item["name"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"name\"");
                 }
                 const int modes = (item.contains("base") ? 1 : 0) + (item.contains("delta") ? 1 : 0) + (item.contains("define") ? 1 : 0);
                 if (modes != 1) throw ToolError("Every \"edits\" entry must give exactly one of \"base\", \"delta\" or \"define\"");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 const std::string name = item["name"].get<std::string>();
                 if (std::find_if(edits.begin(), edits.end(), [&entity, &name](const AttributeEdit& edit) {
                         return edit.entity == entity && edit.name == name;
                     }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity/attribute pair");
                 }
                 AttributeEdit edit{entity, name};
                 if (item.contains("define")) {
                     if (!item["define"].is_object()) throw ToolError("\"define\" must be an object");
                     edit.mode = "define";
                     edit.value = number(item["define"], "base", 0.0f);
                     edit.min = number(item["define"], "min", -3.0e38f);
                     edit.max = number(item["define"], "max", 3.0e38f);
                 } else if (item.contains("base")) {
                     edit.mode = "base";
                     edit.value = number(item, "base", 0.0f);
                     if (!gas::Attributes::HasAttribute(entity, name)) throw ToolError("The entity has no attribute \"" + name + "\" (use define to create it)");
                 } else {
                     edit.mode = "delta";
                     edit.value = number(item, "delta", 0.0f);
                     if (!gas::Attributes::HasAttribute(entity, name)) throw ToolError("The entity has no attribute \"" + name + "\" (use define to create it)");
                 }
                 if (std::isnan(edit.value) || std::isnan(edit.min) || std::isnan(edit.max)) throw ToolError("Attribute changes cannot contain NaN");
                 edits.push_back(std::move(edit));
             }
             Json results = Json::array();
             for (const AttributeEdit& edit : edits) {
                 bool ok = false;
                 if (edit.mode == "define") ok = gas::Attributes::DefineAttribute(edit.entity, edit.name, edit.value, edit.min, edit.max);
                 else if (edit.mode == "base") ok = gas::Attributes::SetAttributeBase(edit.entity, edit.name, edit.value);
                 else ok = gas::Attributes::AddAttributeBase(edit.entity, edit.name, edit.value);
                 if (!ok) throw ToolError("Attribute change was refused for \"" + edit.name + "\"");
                 results.push_back({{"entity", EntityKey(game.GetWorld(), edit.entity)}, {"name", edit.name}, {"mode", edit.mode},
                                    {"base", gas::Attributes::GetAttributeBase(edit.entity, edit.name, 0.0f)},
                                    {"current", gas::Attributes::GetAttribute(edit.entity, edit.name, 0.0f)},
                                    {"min", gas::Attributes::GetAttributeMin(edit.entity, edit.name, 0.0f)},
                                    {"max", gas::Attributes::GetAttributeMax(edit.entity, edit.name, 0.0f)}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_ability",
         "Inspect or control a gameplay ability in the running game: check eligibility, grant or revoke it, activate it, or commit/end/cancel an active ability by handle.",
         Schema({{"entity", {{"type", "string"}}},
                 {"ability", {{"type", "string"}}},
                 {"action", {{"type", "string"}, {"enum", Json::array({"state", "can_activate", "grant", "revoke", "activate", "commit", "end", "cancel"})}},
                 {"handle", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<u32>::max()}}}},
                {"action"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             gas::AbilitySystem* system = gas::AbilitySystem::Active();
             if (system == nullptr) throw ToolError("This game has no ability system (load a scene first)");
             const std::string action = RequireString(args, "action");
             if (action != "state" && action != "can_activate" && action != "grant" && action != "revoke" && action != "activate" &&
                 action != "commit" && action != "end" && action != "cancel") {
                 throw ToolError("\"action\" must be state, can_activate, grant, revoke, activate, commit, end or cancel");
             }
             auto require_entity = [&]() { return FindEntity(game, RequireString(args, "entity")); };
             auto require_ability = [&]() { return RequireString(args, "ability"); };
             if (action == "commit" || action == "end" || action == "cancel") {
                 if (!args.contains("handle") || (!args["handle"].is_number_unsigned() && !args["handle"].is_number_integer())) {
                     throw ToolError("This action requires a positive integer \"handle\"");
                 }
                 u64 raw = 0;
                 if (args["handle"].is_number_unsigned()) raw = args["handle"].get<u64>();
                 else {
                     const i64 signed_raw = args["handle"].get<i64>();
                     if (signed_raw <= 0) throw ToolError("\"handle\" must be between 1 and 4294967295");
                     raw = static_cast<u64>(signed_raw);
                 }
                 if (raw == 0 || raw > std::numeric_limits<u32>::max()) throw ToolError("\"handle\" must be between 1 and 4294967295");
                 const gas::AbilityHandle handle = static_cast<gas::AbilityHandle>(raw);
                 bool ok = false;
                 if (action == "commit") ok = system->Commit(handle);
                 else if (action == "end") ok = system->End(handle);
                 else ok = system->Cancel(handle);
                 return {{"action", action}, {"handle", handle}, {"ok", ok}};
             }
             const Entity entity = require_entity();
             const std::string ability = require_ability();
             if (action == "state") {
                 return {{"entity", EntityKey(game.GetWorld(), entity)}, {"ability", ability},
                         {"granted", system->IsGranted(entity, ability)}, {"active", system->IsActive(entity, ability)},
                         {"active_count", system->ActiveCount(entity)}};
             }
             if (action == "can_activate") {
                 const gas::FailReason reason = system->CanActivate(entity, ability);
                 return {{"entity", EntityKey(game.GetWorld(), entity)}, {"ability", ability},
                         {"can_activate", reason == gas::FailReason::None}, {"reason", gas::FailReasonName(reason)}};
             }
             if (action == "grant" || action == "revoke") {
                 const bool changed = action == "grant" ? system->Grant(entity, ability) : system->Revoke(entity, ability);
                 return {{"entity", EntityKey(game.GetWorld(), entity)}, {"ability", ability}, {"action", action}, {"changed", changed},
                         {"granted", system->IsGranted(entity, ability)}, {"active", system->IsActive(entity, ability)}};
             }
             if (action == "activate") {
                 const gas::AbilitySystem::ActivateResult result = system->TryActivate(entity, ability);
                 return {{"entity", EntityKey(game.GetWorld(), entity)}, {"ability", ability},
                         {"activated", result.handle != 0}, {"handle", result.handle}, {"reason", gas::FailReasonName(result.reason)}};
             }
             throw ToolError("Unsupported ability action");
         }});

    server.AddTool(
        {"game_abilities_many",
         "Check or control up to 64 distinct entity/ability pairs, or commit/end/cancel up to 64 distinct ability handles. Validates the complete request before applying commands; each ability can still fail its normal gameplay checks.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}}, {"ability", {{"type", "string"}}},
                                                           {"action", {{"type", "string"}, {"enum", Json::array({"state", "can_activate", "grant", "revoke", "activate", "commit", "end", "cancel"})}},
                                                           {"handle", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<u32>::max()}}}}},
                                          {"required", Json::array({"action"})}}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             gas::AbilitySystem* system = gas::AbilitySystem::Active();
             if (system == nullptr) throw ToolError("This game has no ability system (load a scene first)");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must contain 1 to 64 ability commands");
             }
             struct AbilityEdit {
                 std::string action;
                 Entity entity;
                 std::string ability;
                 gas::AbilityHandle handle = 0;
             };
             std::vector<AbilityEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("action") || !item["action"].is_string()) throw ToolError("Every edit needs a string \"action\"");
                 AbilityEdit edit;
                 edit.action = item["action"].get<std::string>();
                 const bool by_handle = edit.action == "commit" || edit.action == "end" || edit.action == "cancel";
                 if (edit.action != "state" && edit.action != "can_activate" && edit.action != "grant" && edit.action != "revoke" &&
                     edit.action != "activate" && !by_handle) throw ToolError("Unsupported ability action in \"edits\"");
                 if (by_handle) {
                     if (!item.contains("handle") || (!item["handle"].is_number_unsigned() && !item["handle"].is_number_integer())) {
                         throw ToolError("Handle actions need a positive integer \"handle\"");
                     }
                     u64 raw = 0;
                     if (item["handle"].is_number_unsigned()) raw = item["handle"].get<u64>();
                     else {
                         const i64 value = item["handle"].get<i64>();
                         if (value <= 0) throw ToolError("\"handle\" must be between 1 and 4294967295");
                         raw = static_cast<u64>(value);
                     }
                     if (raw == 0 || raw > std::numeric_limits<u32>::max()) throw ToolError("\"handle\" must be between 1 and 4294967295");
                     edit.handle = static_cast<gas::AbilityHandle>(raw);
                     if (std::find_if(edits.begin(), edits.end(), [&](const AbilityEdit& previous) { return previous.handle == edit.handle && previous.handle != 0; }) != edits.end()) {
                         throw ToolError("Handle actions must not repeat a handle");
                     }
                 } else {
                     if (!item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) throw ToolError("Entity actions need a non-empty string \"entity\"");
                     if (!item.contains("ability") || !item["ability"].is_string() || item["ability"].get<std::string>().empty()) throw ToolError("Entity actions need a non-empty string \"ability\"");
                     edit.entity = FindEntity(game, item["entity"].get<std::string>());
                     edit.ability = item["ability"].get<std::string>();
                     if (std::find_if(edits.begin(), edits.end(), [&](const AbilityEdit& previous) {
                             return previous.handle == 0 && previous.entity == edit.entity && previous.ability == edit.ability;
                         }) != edits.end()) throw ToolError("Entity actions must not repeat an entity/ability pair");
                 }
                 edits.push_back(std::move(edit));
             }
             Json results = Json::array();
             for (const AbilityEdit& edit : edits) {
                 if (edit.handle != 0) {
                     bool ok = false;
                     if (edit.action == "commit") ok = system->Commit(edit.handle);
                     else if (edit.action == "end") ok = system->End(edit.handle);
                     else ok = system->Cancel(edit.handle);
                     results.push_back({{"action", edit.action}, {"handle", edit.handle}, {"ok", ok}});
                     continue;
                 }
                 const std::string entity_key = EntityKey(game.GetWorld(), edit.entity);
                 if (edit.action == "state") {
                     results.push_back({{"action", edit.action}, {"entity", entity_key}, {"ability", edit.ability},
                                        {"granted", system->IsGranted(edit.entity, edit.ability)}, {"active", system->IsActive(edit.entity, edit.ability)},
                                        {"active_count", system->ActiveCount(edit.entity)}});
                 } else if (edit.action == "can_activate") {
                     const gas::FailReason reason = system->CanActivate(edit.entity, edit.ability);
                     results.push_back({{"action", edit.action}, {"entity", entity_key}, {"ability", edit.ability},
                                        {"can_activate", reason == gas::FailReason::None}, {"reason", gas::FailReasonName(reason)}});
                 } else if (edit.action == "grant" || edit.action == "revoke") {
                     const bool changed = edit.action == "grant" ? system->Grant(edit.entity, edit.ability) : system->Revoke(edit.entity, edit.ability);
                     results.push_back({{"action", edit.action}, {"entity", entity_key}, {"ability", edit.ability}, {"changed", changed},
                                        {"granted", system->IsGranted(edit.entity, edit.ability)}, {"active", system->IsActive(edit.entity, edit.ability)}});
                 } else {
                     const gas::AbilitySystem::ActivateResult activated = system->TryActivate(edit.entity, edit.ability);
                     results.push_back({{"action", edit.action}, {"entity", entity_key}, {"ability", edit.ability},
                                        {"activated", activated.handle != 0}, {"handle", activated.handle}, {"reason", gas::FailReasonName(activated.reason)}});
                 }
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});

    server.AddTool(
        {"game_effect",
         "Inspect active gameplay effects, apply an effect to an entity, remove one by runtime handle, or remove effects by granted tag. Effect callbacks are processed on the next game step.",
         Schema({{"action", {{"type", "string"}, {"enum", Json::array({"state", "apply", "remove", "remove_by_tag"})}},
                 {"entity", {{"type", "string"}}},
                 {"source", {{"type", "string"}}},
                 {"effect", {{"type", "string"}}},
                 {"handle", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<u32>::max()}}},
                 {"tag", {{"type", "string"}}}},
                {"action"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             gas::EffectSystem* system = gas::EffectSystem::Active();
             if (system == nullptr) throw ToolError("This game has no effect system (load a scene first)");
             const std::string action = RequireString(args, "action");
             if (action != "state" && action != "apply" && action != "remove" && action != "remove_by_tag") {
                 throw ToolError("\"action\" must be state, apply, remove or remove_by_tag");
             }
             World& world = game.GetWorld();
             auto entity_json = [&](Entity entity) -> Json {
                 if (entity.IsNull()) return nullptr;
                 if (world.IsAlive(entity)) return EntityKey(world, entity);
                 return "#" + std::to_string(entity.index) + ":" + std::to_string(entity.generation);
             };
             if (action == "state") {
                 const Entity entity = FindEntity(game, RequireString(args, "entity"));
                 if (args.contains("effect") && !args["effect"].is_string()) throw ToolError("\"effect\" must be a string");
                 const gas::EffectContainer* container = world.GetComponent<gas::EffectContainer>(entity);
                 Json effects = Json::array();
                 if (container != nullptr) {
                     for (const gas::ActiveEffect& active : container->active) {
                         if (args.contains("effect") && args["effect"].get<std::string>() != active.effect) continue;
                         effects.push_back({{"effect", active.effect}, {"handle", active.handle}, {"source", entity_json(active.source)},
                                            {"remaining", active.remaining}, {"stacks", active.stacks}, {"period_timer", active.period_timer}});
                     }
                 }
                 return {{"entity", EntityKey(world, entity)}, {"count", effects.size()}, {"effects", std::move(effects)}};
             }
             if (action == "apply") {
                 const Entity target = FindEntity(game, Json{{"entity", RequireString(args, "entity")}});
                 Entity source{};
                 if (args.contains("source")) source = FindEntity(game, Json{{"entity", RequireString(args, "source")}});
                 const std::string effect = RequireString(args, "effect");
                 const gas::EffectSystem::ApplyResult result = system->Apply(target, effect, source);
                 const char* status = result.status == gas::EffectSystem::Status::Applied ? "applied" :
                                      result.status == gas::EffectSystem::Status::Instant ? "instant" : "rejected";
                 return {{"entity", EntityKey(world, target)}, {"source", entity_json(source)}, {"effect", effect},
                         {"status", status}, {"handle", result.handle}};
             }
             if (action == "remove") {
                 if (!args.contains("handle") || (!args["handle"].is_number_unsigned() && !args["handle"].is_number_integer())) {
                     throw ToolError("remove requires a positive integer \"handle\"");
                 }
                 u64 raw = 0;
                 if (args["handle"].is_number_unsigned()) raw = args["handle"].get<u64>();
                 else {
                     const i64 signed_raw = args["handle"].get<i64>();
                     if (signed_raw <= 0) throw ToolError("\"handle\" must be between 1 and 4294967295");
                     raw = static_cast<u64>(signed_raw);
                 }
                 if (raw == 0 || raw > std::numeric_limits<u32>::max()) throw ToolError("\"handle\" must be between 1 and 4294967295");
                 const gas::EffectHandle handle = static_cast<gas::EffectHandle>(raw);
                 return {{"handle", handle}, {"removed", system->Remove(handle)}};
             }
             const Entity target = FindEntity(game, Json{{"entity", RequireString(args, "entity")}});
             const std::string tag_name = RequireString(args, "tag");
             const gas::GameplayTag tag = gas::GameplayTag::Make(tag_name);
             if (!tag.IsValid()) throw ToolError("\"tag\" must be a valid dotted gameplay tag");
             const i32 removed = system->RemoveByTag(target, tag);
             return {{"entity", EntityKey(world, target)}, {"tag", tag_name}, {"removed_count", removed}};
         }});

    server.AddTool(
        {"game_effects_many",
         "Apply, remove by handle, or remove by gameplay tag across up to 64 effect operations. Validates every target, handle and tag before applying any operation; callbacks run on the next game step.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"action", {{"type", "string"}, {"enum", Json::array({"apply", "remove", "remove_by_tag"})}},
                                                           {"entity", {{"type", "string"}}}, {"source", {{"type", "string"}}},
                                                           {"effect", {{"type", "string"}}},
                                                           {"handle", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<u32>::max()}}},
                                                           {"tag", {{"type", "string"}}}}},
                                          {"required", Json::array({"action"})}}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             gas::EffectSystem* system = gas::EffectSystem::Active();
             if (system == nullptr) throw ToolError("This game has no effect system (load a scene first)");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must contain 1 to 64 effect operations");
             }
             struct EffectEdit {
                 std::string action;
                 Entity entity;
                 Entity source;
                 std::string effect;
                 gas::EffectHandle handle = 0;
                 std::string tag;
             };
             std::vector<EffectEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("action") || !item["action"].is_string()) throw ToolError("Every edit needs a string \"action\"");
                 EffectEdit edit;
                 edit.action = item["action"].get<std::string>();
                 if (edit.action != "apply" && edit.action != "remove" && edit.action != "remove_by_tag") throw ToolError("Unsupported effect action in \"edits\"");
                 if (edit.action == "remove") {
                     if (!item.contains("handle") || (!item["handle"].is_number_unsigned() && !item["handle"].is_number_integer())) {
                         throw ToolError("remove edits need a positive integer \"handle\"");
                     }
                     u64 raw = 0;
                     if (item["handle"].is_number_unsigned()) raw = item["handle"].get<u64>();
                     else {
                         const i64 value = item["handle"].get<i64>();
                         if (value <= 0) throw ToolError("\"handle\" must be between 1 and 4294967295");
                         raw = static_cast<u64>(value);
                     }
                     if (raw == 0 || raw > std::numeric_limits<u32>::max()) throw ToolError("\"handle\" must be between 1 and 4294967295");
                     edit.handle = static_cast<gas::EffectHandle>(raw);
                     if (std::find_if(edits.begin(), edits.end(), [&](const EffectEdit& previous) { return previous.handle == edit.handle && previous.handle != 0; }) != edits.end()) {
                         throw ToolError("remove edits must not repeat a handle");
                     }
                 } else {
                     if (!item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                         throw ToolError("Effect edits need a non-empty string \"entity\"");
                     }
                     edit.entity = FindEntity(game, item["entity"].get<std::string>());
                     if (edit.action == "apply") {
                         if (!item.contains("effect") || !item["effect"].is_string() || item["effect"].get<std::string>().empty()) {
                             throw ToolError("apply edits need a non-empty string \"effect\"");
                         }
                         edit.effect = item["effect"].get<std::string>();
                         if (item.contains("source")) {
                             if (!item["source"].is_string() || item["source"].get<std::string>().empty()) throw ToolError("\"source\" must be a non-empty entity handle");
                             edit.source = FindEntity(game, item["source"].get<std::string>());
                         }
                     } else {
                         if (!item.contains("tag") || !item["tag"].is_string()) throw ToolError("remove_by_tag edits need string \"tag\"");
                         edit.tag = item["tag"].get<std::string>();
                         if (!gas::GameplayTag::Make(edit.tag).IsValid()) throw ToolError("\"tag\" must be a valid dotted gameplay tag");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [&](const EffectEdit& previous) {
                             return previous.handle == 0 && previous.action == edit.action && previous.entity == edit.entity &&
                                    previous.effect == edit.effect && previous.source == edit.source && previous.tag == edit.tag;
                         }) != edits.end()) throw ToolError("\"edits\" must not repeat the same operation");
                 }
                 edits.push_back(std::move(edit));
             }
             World& world = game.GetWorld();
             auto entity_json = [&](Entity entity) -> Json {
                 if (entity.IsNull()) return nullptr;
                 if (world.IsAlive(entity)) return EntityKey(world, entity);
                 return "#" + std::to_string(entity.index) + ":" + std::to_string(entity.generation);
             };
             Json results = Json::array();
             for (const EffectEdit& edit : edits) {
                 if (edit.action == "apply") {
                     const gas::EffectSystem::ApplyResult result = system->Apply(edit.entity, edit.effect, edit.source);
                     const char* status = result.status == gas::EffectSystem::Status::Applied ? "applied" :
                                          result.status == gas::EffectSystem::Status::Instant ? "instant" : "rejected";
                     results.push_back({{"action", edit.action}, {"entity", EntityKey(world, edit.entity)}, {"source", entity_json(edit.source)},
                                        {"effect", edit.effect}, {"status", status}, {"handle", result.handle}});
                 } else if (edit.action == "remove") {
                     results.push_back({{"action", edit.action}, {"handle", edit.handle}, {"removed", system->Remove(edit.handle)}});
                 } else {
                     const i32 removed = system->RemoveByTag(edit.entity, gas::GameplayTag::Make(edit.tag));
                     results.push_back({{"action", edit.action}, {"entity", EntityKey(world, edit.entity)}, {"tag", edit.tag}, {"removed_count", removed}});
                 }
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});

#if AETHER_MCP_INVENTORY
    server.AddTool(
        {"game_inventory",
         "Inspect or manage a running entity's inventory: configure capacity, add/remove items, move/split stacks, equip/unequip, use items, or count an item. Slot indices are zero-based. Item events and quest notifications are processed on the next game step.",
         Schema({{"entity", {{"type", "string"}}},
                 {"action", {{"type", "string"}, {"enum", Json::array({"state", "count", "configure", "add", "remove", "move", "split", "equip", "unequip", "use"})}},
                 {"item", {{"type", "string"}}},
                 {"count", {{"type", "integer"}}},
                 {"slot", {{"type", "integer"}}},
                 {"to_slot", {{"type", "integer"}}},
                 {"equipment_slot", {{"type", "string"}}},
                 {"capacity", {{"type", "integer"}}},
                 {"max_weight", {{"type", "number"}}}},
                {"entity", "action"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             inv::InventorySystem* system = inv::InventorySystem::Active();
             if (system == nullptr) throw ToolError("This game has no inventory system (load a scene with the Inventory kit)");
             const std::string action = RequireString(args, "action");
             if (action != "state" && action != "count" && action != "configure" && action != "add" && action != "remove" &&
                 action != "move" && action != "split" && action != "equip" && action != "unequip" && action != "use") {
                 throw ToolError("\"action\" must be state, count, configure, add, remove, move, split, equip, unequip or use");
             }
             const Entity entity = FindEntity(game, RequireString(args, "entity"));
             const World& world = game.GetWorld();
             auto integer = [&](const char* key, i32 fallback) -> i32 {
                 if (!args.contains(key)) return fallback;
                 if (!args[key].is_number_integer()) throw ToolError(std::string("\"") + key + "\" must be an integer");
                 if (args[key].is_number_unsigned()) {
                     const u64 value = args[key].get<u64>();
                     if (value > static_cast<u64>(std::numeric_limits<i32>::max())) throw ToolError(std::string("\"") + key + "\" is out of range");
                     return static_cast<i32>(value);
                 }
                 const i64 value = args[key].get<i64>();
                 if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) throw ToolError(std::string("\"") + key + "\" is out of range");
                 return static_cast<i32>(value);
             };
             auto required_integer = [&](const char* key) -> i32 {
                 if (!args.contains(key)) throw ToolError(std::string("Missing integer argument \"") + key + "\"");
                 return integer(key, 0);
             };
             auto required_item = [&]() { return RequireString(args, "item"); };
             if (action == "state") {
                 const inv::Inventory* inventory = world.GetComponent<inv::Inventory>(entity);
                 Json slots = Json::array();
                 Json equipment = Json::array();
                 i32 capacity = 0;
                 f32 max_weight = 0.0f;
                 if (inventory != nullptr) {
                     capacity = inventory->capacity;
                     max_weight = inventory->max_weight;
                     for (usize i = 0; i < inventory->slots.size(); ++i) {
                         const inv::ItemStack& stack = inventory->slots[i];
                         slots.push_back({{"slot", i}, {"item", stack.count > 0 ? Json(stack.item) : Json(nullptr)}, {"count", stack.count}});
                     }
                     for (const inv::EquipSlot& slot : inventory->equipment) equipment.push_back({{"slot", slot.name}, {"item", slot.item}});
                 }
                 return {{"entity", EntityKey(world, entity)}, {"configured", inventory != nullptr}, {"capacity", capacity},
                         {"max_weight", max_weight}, {"weight", system->TotalWeight(entity)}, {"slots", std::move(slots)}, {"equipment", std::move(equipment)}};
             }
             if (action == "count") {
                 const std::string item = required_item();
                 return {{"entity", EntityKey(world, entity)}, {"item", item}, {"count", system->Count(entity, item)}};
             }
             if (action == "configure") {
                 if (!args.contains("capacity")) throw ToolError("configure requires integer \"capacity\"");
                 f32 max_weight = 0.0f;
                 if (args.contains("max_weight")) {
                     if (!args["max_weight"].is_number()) throw ToolError("\"max_weight\" must be a number");
                     max_weight = args["max_weight"].get<f32>();
                     if (!std::isfinite(max_weight) || max_weight < 0.0f) throw ToolError("\"max_weight\" must be finite and non-negative");
                 }
                 const i32 capacity = required_integer("capacity");
                 const bool ok = system->Configure(entity, capacity, max_weight);
                 return {{"entity", EntityKey(world, entity)}, {"configured", ok}, {"capacity", capacity}, {"max_weight", max_weight}};
             }
             if (action == "add") {
                 const std::string item = required_item();
                 const i32 requested = integer("count", 1);
                 const i32 added = system->Add(entity, item, requested);
                 return {{"entity", EntityKey(world, entity)}, {"item", item}, {"requested", requested}, {"added", added}, {"count", system->Count(entity, item)}};
             }
             if (action == "remove") {
                 const std::string item = required_item();
                 const i32 count = integer("count", 1);
                 const bool ok = system->Remove(entity, item, count);
                 return {{"entity", EntityKey(world, entity)}, {"item", item}, {"requested", count}, {"removed", ok}, {"count", system->Count(entity, item)}};
             }
             if (action == "move" || action == "split") {
                 const i32 slot = required_integer("slot");
                 const i32 to_slot = required_integer("to_slot");
                 const bool ok = action == "move" ? system->Move(entity, slot, to_slot) : system->Split(entity, slot, integer("count", 1), to_slot);
                 return {{"entity", EntityKey(world, entity)}, {"action", action}, {"slot", slot}, {"to_slot", to_slot}, {"success", ok}};
             }
             if (action == "equip" || action == "use") {
                 const i32 slot = required_integer("slot");
                 const bool ok = action == "equip" ? system->Equip(entity, slot) : system->Use(entity, slot);
                 return {{"entity", EntityKey(world, entity)}, {"action", action}, {"slot", slot}, {"success", ok}};
             }
             if (action == "unequip") {
                 const std::string slot = RequireString(args, "equipment_slot");
                 const bool ok = system->Unequip(entity, slot);
                 return {{"entity", EntityKey(world, entity)}, {"equipment_slot", slot}, {"success", ok}};
             }
             throw ToolError("\"action\" must be state, count, configure, add, remove, move, split, equip, unequip or use");
         }});

    server.AddTool(
        {"game_inventory_many",
         "Apply 1 to 64 inventory operations after validating every entity and argument. Operations run in order and report their individual result; inventory rules can reject an operation without stopping later operations. Item events and quest notifications are processed on the next game step.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"}, {"properties", {
                                 {"entity", {{"type", "string"}}},
                                 {"action", {{"type", "string"}, {"enum", Json::array({"configure", "add", "remove", "move", "split", "equip", "unequip", "use"})}},
                                 {"item", {{"type", "string"}}}, {"count", {{"type", "integer"}}},
                                 {"slot", {{"type", "integer"}}}, {"to_slot", {{"type", "integer"}}},
                                 {"equipment_slot", {{"type", "string"}}}, {"capacity", {{"type", "integer"}}},
                                 {"max_weight", {{"type", "number"}}}}}, {"required", Json::array({"entity", "action"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             inv::InventorySystem* system = inv::InventorySystem::Active();
             if (system == nullptr) throw ToolError("This game has no inventory system (load a scene with the Inventory kit)");
             if (!args.contains("edits") || !args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 inventory operations");
             }
             struct InventoryEdit {
                 Entity entity;
                 std::string entity_key;
                 std::string action;
                 std::string item;
                 std::string equipment_slot;
                 i32 count = 1;
                 i32 slot = 0;
                 i32 to_slot = 0;
                 i32 capacity = 0;
                 f32 max_weight = 0.0f;
             };
             const World& world = game.GetWorld();
             auto integer = [](const Json& edit, const char* key, i32 fallback, bool required) -> i32 {
                 if (!edit.contains(key)) {
                     if (required) throw ToolError(std::string("Missing integer argument \"") + key + "\"");
                     return fallback;
                 }
                 if (!edit[key].is_number_integer()) throw ToolError(std::string("\"") + key + "\" must be an integer");
                 if (edit[key].is_number_unsigned()) {
                     const u64 value = edit[key].get<u64>();
                     if (value > static_cast<u64>(std::numeric_limits<i32>::max())) throw ToolError(std::string("\"") + key + "\" is out of range");
                     return static_cast<i32>(value);
                 }
                 const i64 value = edit[key].get<i64>();
                 if (value < std::numeric_limits<i32>::min() || value > std::numeric_limits<i32>::max()) throw ToolError(std::string("\"") + key + "\" is out of range");
                 return static_cast<i32>(value);
             };
             auto string_field = [](const Json& edit, const char* key, bool required) -> std::string {
                 if (!edit.contains(key)) {
                     if (required) throw ToolError(std::string("Missing string argument \"") + key + "\"");
                     return {};
                 }
                 if (!edit[key].is_string()) throw ToolError(std::string("\"") + key + "\" must be a string");
                 return edit[key].get<std::string>();
             };
             std::vector<InventoryEdit> parsed;
             parsed.reserve(args["edits"].size());
             for (const Json& raw : args["edits"]) {
                 if (!raw.is_object()) throw ToolError("Every inventory edit must be an object");
                 InventoryEdit edit;
                 edit.entity_key = string_field(raw, "entity", true);
                 edit.entity = FindEntity(game, edit.entity_key);
                 edit.entity_key = EntityKey(world, edit.entity);
                 edit.action = string_field(raw, "action", true);
                 if (edit.action != "configure" && edit.action != "add" && edit.action != "remove" && edit.action != "move" &&
                     edit.action != "split" && edit.action != "equip" && edit.action != "unequip" && edit.action != "use") {
                     throw ToolError("Inventory action must be configure, add, remove, move, split, equip, unequip or use");
                 }
                 if (edit.action == "configure") {
                     edit.capacity = integer(raw, "capacity", 0, true);
                     if (raw.contains("max_weight")) {
                         if (!raw["max_weight"].is_number()) throw ToolError("\"max_weight\" must be a number");
                         edit.max_weight = raw["max_weight"].get<f32>();
                         if (!std::isfinite(edit.max_weight) || edit.max_weight < 0.0f) throw ToolError("\"max_weight\" must be finite and non-negative");
                     }
                 } else if (edit.action == "add" || edit.action == "remove") {
                     edit.item = string_field(raw, "item", true);
                     edit.count = integer(raw, "count", 1, false);
                 } else if (edit.action == "move" || edit.action == "split") {
                     edit.slot = integer(raw, "slot", 0, true);
                     edit.to_slot = integer(raw, "to_slot", 0, true);
                     if (edit.action == "split") edit.count = integer(raw, "count", 1, false);
                 } else if (edit.action == "equip" || edit.action == "use") {
                     edit.slot = integer(raw, "slot", 0, true);
                 } else {
                     edit.equipment_slot = string_field(raw, "equipment_slot", true);
                 }
                 parsed.push_back(std::move(edit));
             }

             Json results = Json::array();
             for (const InventoryEdit& edit : parsed) {
                 if (edit.action == "configure") {
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action},
                                        {"configured", system->Configure(edit.entity, edit.capacity, edit.max_weight)},
                                        {"capacity", edit.capacity}, {"max_weight", edit.max_weight}});
                 } else if (edit.action == "add") {
                     const i32 added = system->Add(edit.entity, edit.item, edit.count);
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"item", edit.item},
                                        {"requested", edit.count}, {"added", added}, {"count", system->Count(edit.entity, edit.item)}});
                 } else if (edit.action == "remove") {
                     const bool removed = system->Remove(edit.entity, edit.item, edit.count);
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"item", edit.item},
                                        {"requested", edit.count}, {"removed", removed}, {"count", system->Count(edit.entity, edit.item)}});
                 } else if (edit.action == "move" || edit.action == "split") {
                     const bool ok = edit.action == "move" ? system->Move(edit.entity, edit.slot, edit.to_slot)
                                                             : system->Split(edit.entity, edit.slot, edit.count, edit.to_slot);
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"slot", edit.slot},
                                        {"to_slot", edit.to_slot}, {"success", ok}});
                 } else if (edit.action == "equip" || edit.action == "use") {
                     const bool ok = edit.action == "equip" ? system->Equip(edit.entity, edit.slot) : system->Use(edit.entity, edit.slot);
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"slot", edit.slot}, {"success", ok}});
                 } else {
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"equipment_slot", edit.equipment_slot},
                                        {"success", system->Unequip(edit.entity, edit.equipment_slot)}});
                 }
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});

    server.AddTool(
        {"game_inventories_many",
         "Inspect the full inventory state of 1 to 64 distinct entities in one read-only request, including slots, equipment and total weight. Entities without an Inventory component are reported as unconfigured.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}}, {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             inv::InventorySystem* system = inv::InventorySystem::Active();
             if (system == nullptr) throw ToolError("This game has no inventory system (load a scene with the Inventory kit)");
             if (!args.contains("entities") || !args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             const World& world = game.GetWorld();
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty entity handle string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) throw ToolError("\"entities\" must not contain duplicate entities");
                 entities.push_back(entity);
             }
             Json inventories = Json::array();
             for (Entity entity : entities) {
                 const inv::Inventory* inventory = world.GetComponent<inv::Inventory>(entity);
                 Json slots = Json::array();
                 Json equipment = Json::array();
                 i32 capacity = 0;
                 f32 max_weight = 0.0f;
                 if (inventory != nullptr) {
                     capacity = inventory->capacity;
                     max_weight = inventory->max_weight;
                     for (usize i = 0; i < inventory->slots.size(); ++i) {
                         const inv::ItemStack& stack = inventory->slots[i];
                         slots.push_back({{"slot", i}, {"item", stack.count > 0 ? Json(stack.item) : Json(nullptr)}, {"count", stack.count}});
                     }
                     for (const inv::EquipSlot& slot : inventory->equipment) equipment.push_back({{"slot", slot.name}, {"item", slot.item}});
                 }
                 inventories.push_back({{"entity", EntityKey(world, entity)}, {"configured", inventory != nullptr}, {"capacity", capacity},
                                        {"max_weight", max_weight}, {"weight", system->TotalWeight(entity)},
                                        {"slots", std::move(slots)}, {"equipment", std::move(equipment)}});
             }
             return {{"count", inventories.size()}, {"inventories", std::move(inventories)}};
         }});

    server.AddTool(
        {"game_inventory_counts_many",
         "Read item counts for 1 to 64 distinct entity/item pairs in one request. Each result reports the entity, item key, and current count; this does not change inventory state.",
         Schema({{"queries", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                               {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}}, {"item", {{"type", "string"}}}}},
                                          {"required", Json::array({"entity", "item"})}}}}}},
                {"queries"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             inv::InventorySystem* system = inv::InventorySystem::Active();
             if (system == nullptr) throw ToolError("This game has no inventory system (load a scene with the Inventory kit)");
             if (!args.contains("queries") || !args["queries"].is_array() || args["queries"].empty() || args["queries"].size() > 64) {
                 throw ToolError("\"queries\" must be an array of 1 to 64 entity/item pairs");
             }
             struct InventoryCountQuery {
                 Entity entity;
                 std::string entity_key;
                 std::string item;
             };
             std::vector<InventoryCountQuery> queries;
             queries.reserve(args["queries"].size());
             for (const Json& query : args["queries"]) {
                 if (!query.is_object() || !query.contains("entity") || !query["entity"].is_string() || query["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"queries\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!query.contains("item") || !query["item"].is_string() || query["item"].get<std::string>().empty()) {
                     throw ToolError("Every \"queries\" entry must contain a non-empty string \"item\"");
                 }
                 const Entity entity = FindEntity(game, query["entity"].get<std::string>());
                 const std::string item = query["item"].get<std::string>();
                 if (std::find_if(queries.begin(), queries.end(), [&](const InventoryCountQuery& existing) {
                         return existing.entity == entity && existing.item == item;
                     }) != queries.end()) {
                     throw ToolError("\"queries\" must not repeat an entity/item pair");
                 }
                 queries.push_back({entity, EntityKey(game.GetWorld(), entity), item});
             }
             Json results = Json::array();
             for (const InventoryCountQuery& query : queries) {
                 results.push_back({{"entity", query.entity_key}, {"item", query.item}, {"count", system->Count(query.entity, query.item)}});
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});

#endif

#if AETHER_MCP_QUESTS
    server.AddTool(
        {"game_quest",
         "Inspect a running entity's quest log, start/abandon/fail a quest, advance one objective, or notify matching objectives. Quest callbacks and rewards are processed on the next game step.",
         Schema({{"entity", {{"type", "string"}}},
                 {"action", {{"type", "string"}, {"enum", Json::array({"state", "start", "abandon", "fail", "progress", "notify"})}},
                 {"quest", {{"type", "string"}}},
                 {"objective", {{"type", "string"}}},
                 {"kind", {{"type", "string"}, {"enum", Json::array({"count", "tag", "flag"})}},
                 {"target", {{"type", "string"}}},
                 {"amount", {{"type", "integer"}}}},
                {"entity", "action"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             quest::QuestSystem* system = quest::QuestSystem::Active();
             if (system == nullptr) throw ToolError("This game has no quest system (load a scene with the Quests kit)");
             const std::string action = RequireString(args, "action");
             if (action != "state" && action != "start" && action != "abandon" && action != "fail" && action != "progress" && action != "notify") {
                 throw ToolError("\"action\" must be state, start, abandon, fail, progress or notify");
             }
             const Entity entity = FindEntity(game, RequireString(args, "entity"));
             const World& world = game.GetWorld();
             auto status_name = [](i32 state) {
                 switch (state) {
                 case quest::kActive: return "active";
                 case quest::kCompleted: return "completed";
                 case quest::kFailed: return "failed";
                 default: return "inactive";
                 }
             };
             auto read_amount = [&](i32 fallback) {
                 if (!args.contains("amount")) return fallback;
                 if (!args["amount"].is_number_integer()) throw ToolError("\"amount\" must be an integer");
                 if (args["amount"].is_number_unsigned()) {
                     const u64 amount = args["amount"].get<u64>();
                     if (amount > static_cast<u64>(std::numeric_limits<i32>::max())) throw ToolError("\"amount\" is out of range");
                     return static_cast<i32>(amount);
                 }
                 const i64 amount = args["amount"].get<i64>();
                 if (amount < std::numeric_limits<i32>::min() || amount > std::numeric_limits<i32>::max()) throw ToolError("\"amount\" is out of range");
                 return static_cast<i32>(amount);
             };
             if (action == "state") {
                 Json quests = Json::array();
                 const quest::QuestLog* log = world.GetComponent<quest::QuestLog>(entity);
                 const bool has_filter = args.contains("quest");
                 const std::string filter = has_filter ? RequireString(args, "quest") : std::string{};
                 auto append = [&](const std::string& name, i32 state, const std::vector<quest::ObjectiveState>& objectives) {
                     Json objective_rows = Json::array();
                     for (const quest::ObjectiveState& objective : objectives) {
                         objective_rows.push_back({{"id", objective.id}, {"text", system->ObjectiveText(name, objective.id)},
                                                   {"progress", objective.progress}, {"done", objective.done}});
                     }
                     quests.push_back({{"quest", name}, {"title", system->Title(name)}, {"state", status_name(state)}, {"objectives", std::move(objective_rows)}});
                 };
                 if (has_filter) {
                     const quest::QuestState* saved = log == nullptr ? nullptr : log->Find(filter);
                     static const std::vector<quest::ObjectiveState> no_objectives;
                     append(filter, saved == nullptr ? quest::kInactive : saved->state, saved == nullptr ? no_objectives : saved->objectives);
                 } else if (log != nullptr) {
                     for (const quest::QuestState& saved : log->quests) append(saved.name, saved.state, saved.objectives);
                 }
                 return {{"entity", EntityKey(world, entity)}, {"count", quests.size()}, {"quests", std::move(quests)}};
             }
             if (action == "notify") {
                 const std::string kind = RequireString(args, "kind");
                 quest::Objective::Kind objective_kind;
                 if (kind == "count") objective_kind = quest::Objective::Kind::Count;
                 else if (kind == "tag") objective_kind = quest::Objective::Kind::Tag;
                 else if (kind == "flag") objective_kind = quest::Objective::Kind::Flag;
                 else throw ToolError("\"kind\" must be count, tag or flag");
                 const std::string target = RequireString(args, "target");
                 const i32 moved = system->Notify(entity, objective_kind, target, read_amount(1));
                 return {{"entity", EntityKey(world, entity)}, {"action", action}, {"kind", kind}, {"target", target}, {"advanced", moved}};
             }
             const std::string quest_name = RequireString(args, "quest");
             quest::Result result = quest::Result::Ok;
             if (action == "start") result = system->Start(entity, quest_name);
             else if (action == "abandon") result = system->Abandon(entity, quest_name);
             else if (action == "fail") result = system->Fail(entity, quest_name);
             else if (action == "progress") {
                 const std::string objective = RequireString(args, "objective");
                 result = system->Progress(entity, quest_name, objective, read_amount(1));
                 return {{"entity", EntityKey(world, entity)}, {"quest", quest_name}, {"objective", objective},
                         {"result", quest::ResultName(result)}, {"progress", system->GetProgress(entity, quest_name, objective)},
                         {"state", status_name(system->State(entity, quest_name))}};
             }
             return {{"entity", EntityKey(world, entity)}, {"quest", quest_name}, {"action", action},
                     {"result", quest::ResultName(result)}, {"state", status_name(system->State(entity, quest_name))}};
         }});

    server.AddTool(
        {"game_quests_many",
         "Apply 1 to 64 ordered quest operations across entities: start, abandon, fail, progress an objective, or notify matching objectives. Validates every entity and argument before applying; returns each operation's result. Quest callbacks and rewards are processed on the next game step.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", {{"type", "string"}}},
                                                        {"action", {{"type", "string"}, {"enum", Json::array({"start", "abandon", "fail", "progress", "notify"})}},
                                                        {"quest", {{"type", "string"}}}, {"objective", {{"type", "string"}}},
                                                        {"kind", {{"type", "string"}, {"enum", Json::array({"count", "tag", "flag"})}},
                                                        {"target", {{"type", "string"}}}, {"amount", {{"type", "integer"}}}},
                                        {"required", Json::array({"entity", "action"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             quest::QuestSystem* system = quest::QuestSystem::Active();
             if (system == nullptr) throw ToolError("This game has no quest system (load a scene with the Quests kit)");
             if (!args.contains("edits") || !args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 quest operations");
             }
             struct QuestEdit {
                 Entity entity;
                 std::string entity_key;
                 std::string action;
                 std::string quest_name;
                 std::string objective;
                 std::string kind;
                 std::string target;
                 i32 amount = 1;
             };
             const World& world = game.GetWorld();
             auto string_field = [](const Json& edit, const char* key, bool required) -> std::string {
                 if (!edit.contains(key)) {
                     if (required) throw ToolError(std::string("Missing string argument \"") + key + "\"");
                     return {};
                 }
                 if (!edit[key].is_string()) throw ToolError(std::string("\"") + key + "\" must be a string");
                 return edit[key].get<std::string>();
             };
             auto amount_field = [](const Json& edit) -> i32 {
                 if (!edit.contains("amount")) return 1;
                 if (!edit["amount"].is_number_integer()) throw ToolError("\"amount\" must be an integer");
                 if (edit["amount"].is_number_unsigned()) {
                     const u64 amount = edit["amount"].get<u64>();
                     if (amount > static_cast<u64>(std::numeric_limits<i32>::max())) throw ToolError("\"amount\" is out of range");
                     return static_cast<i32>(amount);
                 }
                 const i64 amount = edit["amount"].get<i64>();
                 if (amount < std::numeric_limits<i32>::min() || amount > std::numeric_limits<i32>::max()) throw ToolError("\"amount\" is out of range");
                 return static_cast<i32>(amount);
             };
             std::vector<QuestEdit> parsed;
             parsed.reserve(args["edits"].size());
             for (const Json& raw : args["edits"]) {
                 if (!raw.is_object()) throw ToolError("Every quest edit must be an object");
                 QuestEdit edit;
                 edit.entity_key = string_field(raw, "entity", true);
                 edit.entity = FindEntity(game, edit.entity_key);
                 edit.entity_key = EntityKey(world, edit.entity);
                 edit.action = string_field(raw, "action", true);
                 if (edit.action != "start" && edit.action != "abandon" && edit.action != "fail" && edit.action != "progress" && edit.action != "notify") {
                     throw ToolError("Quest action must be start, abandon, fail, progress or notify");
                 }
                 if (edit.action == "notify") {
                     edit.kind = string_field(raw, "kind", true);
                     if (edit.kind != "count" && edit.kind != "tag" && edit.kind != "flag") throw ToolError("\"kind\" must be count, tag or flag");
                     edit.target = string_field(raw, "target", true);
                     edit.amount = amount_field(raw);
                 } else {
                     edit.quest_name = string_field(raw, "quest", true);
                     if (edit.action == "progress") {
                         edit.objective = string_field(raw, "objective", true);
                         edit.amount = amount_field(raw);
                     }
                 }
                 parsed.push_back(std::move(edit));
             }

             auto status_name = [](i32 state) {
                 switch (state) {
                 case quest::kActive: return "active";
                 case quest::kCompleted: return "completed";
                 case quest::kFailed: return "failed";
                 default: return "inactive";
                 }
             };
             Json results = Json::array();
             for (const QuestEdit& edit : parsed) {
                 if (edit.action == "notify") {
                     const quest::Objective::Kind kind = edit.kind == "count" ? quest::Objective::Kind::Count :
                                                         edit.kind == "tag" ? quest::Objective::Kind::Tag : quest::Objective::Kind::Flag;
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"kind", edit.kind},
                                        {"target", edit.target}, {"advanced", system->Notify(edit.entity, kind, edit.target, edit.amount)}});
                 } else if (edit.action == "progress") {
                     const quest::Result result = system->Progress(edit.entity, edit.quest_name, edit.objective, edit.amount);
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"quest", edit.quest_name},
                                        {"objective", edit.objective}, {"result", quest::ResultName(result)},
                                        {"progress", system->GetProgress(edit.entity, edit.quest_name, edit.objective)},
                                        {"state", status_name(system->State(edit.entity, edit.quest_name))}});
                 } else {
                     quest::Result result = quest::Result::Ok;
                     if (edit.action == "start") result = system->Start(edit.entity, edit.quest_name);
                     else if (edit.action == "abandon") result = system->Abandon(edit.entity, edit.quest_name);
                     else result = system->Fail(edit.entity, edit.quest_name);
                     results.push_back({{"entity", edit.entity_key}, {"action", edit.action}, {"quest", edit.quest_name},
                                        {"result", quest::ResultName(result)}, {"state", status_name(system->State(edit.entity, edit.quest_name))}});
                 }
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});

    server.AddTool(
        {"game_quest_states_many",
         "Read quest logs for 1 to 64 distinct entities in one request. Optionally pass a quest name to return that quest's state for each entity, including inactive entries. This is read-only.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"quest", {{"type", "string"}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             quest::QuestSystem* system = quest::QuestSystem::Active();
             if (system == nullptr) throw ToolError("This game has no quest system (load a scene with the Quests kit)");
             if (!args.contains("entities") || !args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             std::string filter;
             const bool has_filter = args.contains("quest");
             if (has_filter) {
                 if (!args["quest"].is_string()) throw ToolError("\"quest\" must be a string");
                 filter = args["quest"].get<std::string>();
             }
             struct QuestLogQuery {
                 Entity entity;
                 std::string key;
             };
             std::vector<QuestLogQuery> queries;
             queries.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty entity handle string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(queries.begin(), queries.end(), [entity](const QuestLogQuery& query) { return query.entity == entity; }) != queries.end()) {
                     throw ToolError("\"entities\" must not contain duplicate entities");
                 }
                 queries.push_back({entity, EntityKey(game.GetWorld(), entity)});
             }
             const World& world = game.GetWorld();
             auto status_name = [](i32 state) {
                 switch (state) {
                 case quest::kActive: return "active";
                 case quest::kCompleted: return "completed";
                 case quest::kFailed: return "failed";
                 default: return "inactive";
                 }
             };
             Json results = Json::array();
             for (const QuestLogQuery& query : queries) {
                 Json quests = Json::array();
                 const quest::QuestLog* log = world.GetComponent<quest::QuestLog>(query.entity);
                 auto append = [&](const std::string& name, i32 state, const std::vector<quest::ObjectiveState>& objectives) {
                     Json objective_rows = Json::array();
                     for (const quest::ObjectiveState& objective : objectives) {
                         objective_rows.push_back({{"id", objective.id}, {"text", system->ObjectiveText(name, objective.id)},
                                                   {"progress", objective.progress}, {"done", objective.done}});
                     }
                     quests.push_back({{"quest", name}, {"title", system->Title(name)}, {"state", status_name(state)},
                                       {"objectives", std::move(objective_rows)}});
                 };
                 if (has_filter) {
                     const quest::QuestState* saved = log == nullptr ? nullptr : log->Find(filter);
                     static const std::vector<quest::ObjectiveState> no_objectives;
                     append(filter, saved == nullptr ? quest::kInactive : saved->state,
                            saved == nullptr ? no_objectives : saved->objectives);
                 } else if (log != nullptr) {
                     for (const quest::QuestState& saved : log->quests) append(saved.name, saved.state, saved.objectives);
                 }
                 results.push_back({{"entity", query.key}, {"count", quests.size()}, {"quests", std::move(quests)}});
             }
             return {{"count", results.size()}, {"results", std::move(results)}};
         }});
#endif

#if AETHER_MCP_INTERACTION
    // -- Interaction (the Interactable component and the game's InteractionSystem) ---------------------------------
    server.AddTool(
        {"game_interactables",
         "The usable things in the running game: entities with an Interactable, with prompt, range, state (enabled, used, cooling down) and, "
         "if an `interactor` is given, whether and why it can use each (the reason names are the ones scripts hear: out_of_range, "
         "missing_tag, blocked_tag, cooldown, used, disabled...).",
         Schema({{"interactor", {{"type", "string"}, {"description", "Entity that would use them (GUID or #index[:generation])"}}}}), [host](const Json& args) -> Json {
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
        {"game_can_interact_many",
         "Check whether up to 64 interactor/target pairs can interact right now without changing the game. Returns the reason for each pair.",
         Schema({{"checks", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"interactor", {{"type", "string"}}}, {"target", {{"type", "string"}}}}},
                                          {"required", Json::array({"interactor", "target"})}}}}}},
                {"checks"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             interact::InteractionSystem* system = interact::InteractionSystem::Active();
             if (system == nullptr) throw ToolError("This game has no interaction system");
             if (!args["checks"].is_array() || args["checks"].empty() || args["checks"].size() > 64) {
                 throw ToolError("\"checks\" must be an array of 1 to 64 interactor/target pairs");
             }
             struct Pair {
                 Entity interactor;
                 Entity target;
             };
             std::vector<Pair> pairs;
             pairs.reserve(args["checks"].size());
             for (const Json& item : args["checks"]) {
                 if (!item.is_object() || !item.contains("interactor") || !item["interactor"].is_string() || item["interactor"].get<std::string>().empty()) {
                     throw ToolError("Every \"checks\" entry must contain a non-empty string \"interactor\"");
                 }
                 if (!item.contains("target") || !item["target"].is_string() || item["target"].get<std::string>().empty()) {
                     throw ToolError("Every \"checks\" entry must contain a non-empty string \"target\"");
                 }
                 const Entity interactor = FindEntity(game, item["interactor"].get<std::string>());
                 const Entity target = FindEntity(game, item["target"].get<std::string>());
                 if (std::find_if(pairs.begin(), pairs.end(), [interactor, target](const Pair& pair) {
                         return pair.interactor == interactor && pair.target == target;
                     }) != pairs.end()) {
                     throw ToolError("\"checks\" must not repeat the same interactor/target pair");
                 }
                 pairs.push_back({interactor, target});
             }
             Json results = Json::array();
             for (const Pair& pair : pairs) {
                 const interact::Reason reason = system->Check(pair.interactor, pair.target);
                 results.push_back({{"interactor", EntityKey(game.GetWorld(), pair.interactor)}, {"target", EntityKey(game.GetWorld(), pair.target)},
                                    {"can_interact", reason == interact::Reason::None}, {"reason", interact::ReasonName(reason)}});
             }
             return {{"count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_interact_many",
         "Attempt interactions for up to 64 unique interactor/target pairs using normal range, tag, cooldown and effect checks. Validates every entity pair first and reports each interaction result.",
         Schema({{"interactions", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                                    {"items", {{"type", "object"},
                                                {"properties", {{"interactor", {{"type", "string"}}}, {"target", {{"type", "string"}}}}},
                                                {"required", Json::array({"interactor", "target"})}}}}}},
                {"interactions"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             interact::InteractionSystem* system = interact::InteractionSystem::Active();
             if (system == nullptr) throw ToolError("This game has no interaction system");
             if (!args["interactions"].is_array() || args["interactions"].empty() || args["interactions"].size() > 64) {
                 throw ToolError("\"interactions\" must be an array of 1 to 64 interactor/target pairs");
             }
             struct Pair {
                 Entity interactor;
                 Entity target;
             };
             std::vector<Pair> pairs;
             pairs.reserve(args["interactions"].size());
             for (const Json& item : args["interactions"]) {
                 if (!item.is_object() || !item.contains("interactor") || !item["interactor"].is_string() || item["interactor"].get<std::string>().empty()) {
                     throw ToolError("Every \"interactions\" entry must contain a non-empty string \"interactor\"");
                 }
                 if (!item.contains("target") || !item["target"].is_string() || item["target"].get<std::string>().empty()) {
                     throw ToolError("Every \"interactions\" entry must contain a non-empty string \"target\"");
                 }
                 const Entity interactor = FindEntity(game, item["interactor"].get<std::string>());
                 const Entity target = FindEntity(game, item["target"].get<std::string>());
                 if (std::find_if(pairs.begin(), pairs.end(), [interactor, target](const Pair& pair) {
                         return pair.interactor == interactor && pair.target == target;
                     }) != pairs.end()) {
                     throw ToolError("\"interactions\" must not repeat the same interactor/target pair");
                 }
                 pairs.push_back({interactor, target});
             }
             Json results = Json::array();
             usize interacted_count = 0;
             for (const Pair& pair : pairs) {
                 const interact::Reason reason = system->Interact(pair.interactor, pair.target);
                 const bool interacted = reason == interact::Reason::None;
                 if (interacted) ++interacted_count;
                 results.push_back({{"interactor", EntityKey(game.GetWorld(), pair.interactor)}, {"target", EntityKey(game.GetWorld(), pair.target)},
                                    {"interacted", interacted}, {"reason", interact::ReasonName(reason)}});
             }
             return {{"interacted_count", interacted_count}, {"requested_count", results.size()}, {"results", results}};
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

    server.AddTool(
        {"game_interaction_focus_many",
         "Find the nearest usable target for up to 64 unique interactors, optionally using a different forward direction for each. Validates all inputs before resolving focus.",
         Schema({{"interactors", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                                   {"items", {{"type", "object"},
                                               {"properties", {{"interactor", {{"type", "string"}}},
                                                                {"forward", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                               {"required", Json::array({"interactor"})}}}}}},
                {"interactors"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             interact::InteractionSystem* system = interact::InteractionSystem::Active();
             if (system == nullptr) throw ToolError("This game has no interaction system");
             if (!args["interactors"].is_array() || args["interactors"].empty() || args["interactors"].size() > 64) {
                 throw ToolError("\"interactors\" must be an array of 1 to 64 interactors");
             }
             struct Request { Entity entity; Vec3 forward; };
             std::vector<Request> requests;
             requests.reserve(args["interactors"].size());
             for (const Json& item : args["interactors"]) {
                 if (!item.is_object() || !item.contains("interactor") || !item["interactor"].is_string() || item["interactor"].get<std::string>().empty()) {
                     throw ToolError("Every \"interactors\" entry must contain a non-empty string \"interactor\"");
                 }
                 Vec3 forward{0.0f, 0.0f, 0.0f};
                 if (item.contains("forward")) {
                     const Json& f = item["forward"];
                     if (!f.is_array() || f.size() != 3 || !f[0].is_number() || !f[1].is_number() || !f[2].is_number()) {
                         throw ToolError("\"forward\" must be [x, y, z]");
                     }
                     forward = Vec3(f[0].get<f32>(), f[1].get<f32>(), f[2].get<f32>());
                 }
                 const Entity entity = FindEntity(game, item["interactor"].get<std::string>());
                 if (std::find_if(requests.begin(), requests.end(), [entity](const Request& request) { return request.entity == entity; }) != requests.end()) {
                     throw ToolError("\"interactors\" must not repeat an interactor");
                 }
                 if (game.GetWorld().GetComponent<Transform>(entity) == nullptr) throw ToolError("An interactor has no Transform");
                 requests.push_back({entity, forward});
             }
             Json results = Json::array();
             for (const Request& request : requests) {
                 const Transform* transform = game.GetWorld().GetComponent<Transform>(request.entity);
                 const interact::FocusResult focus = system->Focus(request.entity, transform->position, request.forward);
                 Json result = {{"interactor", EntityKey(game.GetWorld(), request.entity)}, {"target", nullptr}};
                 if (!focus.target.IsNull()) {
                     result["target"] = EntityKey(game.GetWorld(), focus.target);
                     result["distance"] = focus.distance;
                     result["prompt"] = focus.prompt;
                 }
                 results.push_back(std::move(result));
             }
             return {{"count", results.size()}, {"results", results}};
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

    server.AddTool(
        {"game_set_interactables_many",
         "Enable or disable and/or reset up to 64 unique live interactables. Validates every target and requested action before applying changes.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"target", {{"type", "string"}}}, {"enabled", {{"type", "boolean"}}},
                                                          {"reset", {{"type", "boolean"}}}}},
                                         {"required", Json::array({"target"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             interact::InteractionSystem* system = interact::InteractionSystem::Active();
             if (system == nullptr) throw ToolError("This game has no interaction system");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 interactable updates");
             }
             struct InteractableEdit {
                 Entity target;
                 bool has_enabled = false;
                 bool enabled = false;
                 bool reset = false;
             };
             std::vector<InteractableEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("target") || !item["target"].is_string() || item["target"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"target\"");
                 }
                 if (!item.contains("enabled") && !item.contains("reset")) throw ToolError("Every edit must specify \"enabled\" and/or \"reset\"");
                 if (item.contains("enabled") && !item["enabled"].is_boolean()) throw ToolError("\"enabled\" must be a boolean");
                 if (item.contains("reset") && !item["reset"].is_boolean()) throw ToolError("\"reset\" must be a boolean");
                 const Entity target = FindEntity(game, item["target"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [target](const InteractableEdit& edit) { return edit.target == target; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat the same target");
                 }
                 if (world.GetComponent<interact::Interactable>(target) == nullptr) throw ToolError("Target " + EntityKey(world, target) + " is not an interactable");
                 edits.push_back({target, item.contains("enabled"), item.contains("enabled") ? item["enabled"].get<bool>() : false,
                                  item.contains("reset") && item["reset"].get<bool>()});
             }
             Json results = Json::array();
             for (const InteractableEdit& edit : edits) {
                 if (edit.has_enabled && !system->SetEnabled(edit.target, edit.enabled)) throw ToolError("Could not update interactable " + EntityKey(world, edit.target));
                 if (edit.reset && !system->Reset(edit.target)) throw ToolError("Could not reset interactable " + EntityKey(world, edit.target));
                 const interact::Interactable* state = world.GetComponent<interact::Interactable>(edit.target);
                 results.push_back({{"target", EntityKey(world, edit.target)}, {"enabled", state->enabled},
                                    {"used", state->used}, {"remaining", state->remaining}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
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
        {"game_audio_events",
         "Read AudioSource cues that finished during the loaded game's most recent audio update, with the source entity handle and cue path.",
         Schema(Json::object()),
         [host](const Json&) -> Json {
             Game& game = host->Require();
             audio::AudioSystem* audio = game.AudioSystem();
             if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
             World& world = game.GetWorld();
             Json events = Json::array();
             for (const audio::AudioFinishedEvent& event : audio->Events()) {
                 const std::string entity = world.IsAlive(event.entity)
                     ? EntityKey(world, event.entity)
                     : "#" + std::to_string(event.entity.index) + ":" + std::to_string(event.entity.generation);
                 events.push_back({{"type", "finished"}, {"entity", entity}, {"cue", event.cue}});
             }
             return {{"count", events.size()}, {"events", std::move(events)}};
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

    server.AddTool(
        {"game_play_sounds_many",
         "Play 1 to 64 one-shot sound cues in 2D or at individual world locations. Validates all cues, locations, volumes and pitches before starting playback; step the game to hear them.",
         Schema({{"sounds", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"cue", {{"type", "string"}}},
                                                           {"location", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                                                           {"volume_db", {{"type", "number"}}}, {"pitch", {{"type", "number"}}}}},
                                          {"required", Json::array({"cue"})}}}}}},
                {"sounds"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             audio::AudioSystem* audio = game.AudioSystem();
             if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
             if (!args["sounds"].is_array() || args["sounds"].empty() || args["sounds"].size() > 64) {
                 throw ToolError("\"sounds\" must be an array of 1 to 64 sound cues");
             }
             struct SoundRequest {
                 std::string cue;
                 bool spatial = false;
                 Vec3 location{};
                 f32 volume = 0.0f;
                 f32 pitch = 1.0f;
             };
             std::vector<SoundRequest> sounds;
             sounds.reserve(args["sounds"].size());
             for (const Json& item : args["sounds"]) {
                 if (!item.is_object() || !item.contains("cue") || !item["cue"].is_string() || item["cue"].get<std::string>().empty()) {
                     throw ToolError("Every \"sounds\" entry must contain a non-empty string \"cue\"");
                 }
                 SoundRequest sound;
                 sound.cue = item["cue"].get<std::string>();
                 if (item.contains("volume_db")) {
                     if (!item["volume_db"].is_number()) throw ToolError("\"volume_db\" must be a number");
                     sound.volume = item["volume_db"].get<f32>();
                     if (!std::isfinite(sound.volume)) throw ToolError("\"volume_db\" must be finite");
                 }
                 if (item.contains("pitch")) {
                     if (!item["pitch"].is_number()) throw ToolError("\"pitch\" must be a number");
                     sound.pitch = item["pitch"].get<f32>();
                     if (!std::isfinite(sound.pitch) || !(sound.pitch > 0.0f)) throw ToolError("\"pitch\" must be finite and above 0");
                 }
                 if (item.contains("location")) {
                     const Json& location = item["location"];
                     if (!location.is_array() || location.size() != 3 || !location[0].is_number() || !location[1].is_number() || !location[2].is_number()) {
                         throw ToolError("\"location\" must be [x, y, z]");
                     }
                     sound.location = Vec3(location[0].get<f32>(), location[1].get<f32>(), location[2].get<f32>());
                     if (!std::isfinite(sound.location.x) || !std::isfinite(sound.location.y) || !std::isfinite(sound.location.z)) {
                         throw ToolError("\"location\" values must be finite");
                     }
                     sound.spatial = true;
                 }
                 sounds.push_back(std::move(sound));
             }
             Json results = Json::array();
             usize played_count = 0;
             for (const SoundRequest& sound : sounds) {
                 const audio::CueHandle handle = sound.spatial
                                                     ? audio->PlaySoundAtLocation(sound.cue, sound.location, sound.volume, sound.pitch)
                                                     : audio->PlaySound2D(sound.cue, sound.volume, sound.pitch);
                 const bool played = handle != 0;
                 if (played) ++played_count;
                 Json result = {{"cue", sound.cue}, {"played", played}, {"spatial", sound.spatial}};
                 if (sound.spatial) result["location"] = Json::array({sound.location.x, sound.location.y, sound.location.z});
                 results.push_back(std::move(result));
             }
             return {{"played_count", played_count}, {"requested_count", results.size()}, {"results", results}, {"problems", audio->Problems()}};
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

    server.AddTool(
        {"game_set_buses_many",
         "Change volume and/or mute state on up to 64 unique mixer buses. Validates every bus and value before applying any changes; volume changes can fade over seconds.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"bus", {{"type", "string"}}}, {"volume_db", {{"type", "number"}}},
                                                          {"fade_seconds", {{"type", "number"}}}, {"muted", {{"type", "boolean"}}}}},
                                         {"required", Json::array({"bus"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             audio::AudioSystem* audio = game.AudioSystem();
             if (audio == nullptr) throw ToolError("This game has no audio system (load a scene first)");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 mixer bus updates");
             }
             struct BusEdit {
                 std::string name;
                 audio::BusId id = audio::Mixer::kInvalidBus;
                 bool has_volume = false;
                 f32 volume = 0.0f;
                 f32 fade = 0.0f;
                 bool has_muted = false;
                 bool muted = false;
             };
             std::vector<BusEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("bus") || !item["bus"].is_string() || item["bus"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"bus\"");
                 }
                 const std::string name = item["bus"].get<std::string>();
                 if (!item.contains("volume_db") && !item.contains("muted")) throw ToolError("Every bus edit must include \"volume_db\" and/or \"muted\"");
                 const audio::BusId id = audio->GetMixer().FindBus(name);
                 if (id == audio::Mixer::kInvalidBus) throw ToolError("No bus \"" + name + "\"");
                 if (std::find_if(edits.begin(), edits.end(), [&name](const BusEdit& edit) { return edit.name == name; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat the same bus");
                 }
                 BusEdit edit;
                 edit.name = name;
                 edit.id = id;
                 edit.has_volume = item.contains("volume_db");
                 if (edit.has_volume) {
                     if (!item["volume_db"].is_number()) throw ToolError("\"volume_db\" must be a number");
                     edit.volume = item["volume_db"].get<f32>();
                     if (!std::isfinite(edit.volume)) throw ToolError("\"volume_db\" must be finite");
                 }
                 if (item.contains("fade_seconds")) {
                     if (!edit.has_volume || !item["fade_seconds"].is_number()) throw ToolError("\"fade_seconds\" requires numeric \"volume_db\"");
                     edit.fade = item["fade_seconds"].get<f32>();
                     if (!std::isfinite(edit.fade) || edit.fade < 0.0f) throw ToolError("\"fade_seconds\" must be a finite non-negative number");
                 }
                 edit.has_muted = item.contains("muted");
                 if (edit.has_muted) {
                     if (!item["muted"].is_boolean()) throw ToolError("\"muted\" must be a boolean");
                     edit.muted = item["muted"].get<bool>();
                 }
                 edits.push_back(std::move(edit));
             }
             Json results = Json::array();
             for (const BusEdit& edit : edits) {
                 if (edit.has_volume && !audio->SetBusVolume(edit.name, edit.volume, edit.fade)) throw ToolError("Could not change volume for bus \"" + edit.name + "\"");
                 if (edit.has_muted) audio->GetMixer().SetBusMuted(edit.id, edit.muted);
                 results.push_back({{"bus", edit.name}, {"volume_db", audio->GetMixer().BusVolume(edit.id)}, {"muted", audio->GetMixer().BusMuted(edit.id)}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
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

    server.AddTool(
        {"game_audio_sources_many",
         "Queue different play, stop, fade, volume or cue commands on up to 64 unique live AudioSources. Validates every target and command before queuing; step the game to apply them.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}},
                                                          {"action", {{"type", "string"}, {"description", "play | stop | fade_in | fade_out | set_volume | set_cue"}}},
                                                          {"value", {{"type", "number"}}}, {"text", {{"type", "string"}}}}},
                                         {"required", Json::array({"entity", "action"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 AudioSource commands");
             }
             struct AudioEdit {
                 Entity entity;
                 std::string action;
                 f32 value = 0.0f;
                 std::string text;
             };
             std::vector<AudioEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("action") || !item["action"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"action\"");
                 const std::string action = item["action"].get<std::string>();
                 if (action != "play" && action != "stop" && action != "fade_in" && action != "fade_out" && action != "set_volume" && action != "set_cue") {
                     throw ToolError("\"action\" must be play, stop, fade_in, fade_out, set_volume or set_cue");
                 }
                 f32 value = 0.0f;
                 if (item.contains("value")) {
                     if (!item["value"].is_number()) throw ToolError("\"value\" must be a number");
                     value = item["value"].get<f32>();
                     if (!std::isfinite(value)) throw ToolError("\"value\" must be finite");
                 }
                 std::string text;
                 if (action == "set_cue") {
                     if (!item.contains("text") || !item["text"].is_string()) throw ToolError("A set_cue edit must contain string \"text\"");
                     text = item["text"].get<std::string>();
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const AudioEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity");
                 }
                 if (world.GetComponent<AudioSource>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no AudioSource");
                 edits.push_back({entity, action, value, std::move(text)});
             }
             Json results = Json::array();
             for (const AudioEdit& edit : edits) {
                 AudioSource* source = world.GetComponent<AudioSource>(edit.entity);
                 if (edit.action == "play") source->Play();
                 else if (edit.action == "stop") source->Stop();
                 else if (edit.action == "fade_in") source->FadeIn(edit.value);
                 else if (edit.action == "fade_out") source->FadeOut(edit.value);
                 else if (edit.action == "set_volume") source->SetVolume(edit.value);
                 else source->SetCue(edit.text);
                 results.push_back({{"entity", EntityKey(world, edit.entity)}, {"action", edit.action}, {"cue", source->cue},
                                    {"playing", source->playing}, {"volume_db", source->volume_db}, {"queued_commands", source->commands.size()}});
             }
             return {{"queued_count", results.size()}, {"results", results}};
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

    server.AddTool(
        {"game_sequence_events",
         "Read marker, audio-track, animation-track, and finished events from the loaded game's most recent sequence update.",
         Schema(Json::object()),
         [host](const Json&) -> Json {
             Game& game = host->Require();
             seq::SequenceSystem* system = seq::SequenceSystem::Active();
             if (system == nullptr) throw ToolError("This game has no sequence system (load a scene first)");
             World& world = game.GetWorld();
             auto event_entity = [&](Entity entity) -> Json {
                 if (entity.IsNull()) return nullptr;
                 if (world.IsAlive(entity)) return EntityKey(world, entity);
                 return "#" + std::to_string(entity.index) + ":" + std::to_string(entity.generation);
             };
             Json events = Json::array();
             for (const seq::SequenceEvent& event : system->Events()) {
                 const char* type = "marker";
                 switch (event.kind) {
                 case seq::SequenceEvent::Kind::Marker: type = "marker"; break;
                 case seq::SequenceEvent::Kind::Finished: type = "finished"; break;
                 case seq::SequenceEvent::Kind::Audio: type = "audio"; break;
                 case seq::SequenceEvent::Kind::Animation: type = "animation"; break;
                 }
                 events.push_back({{"type", type}, {"entity", event_entity(event.entity)},
                                   {"name", event.name}, {"payload", event.payload},
                                   {"subject", event_entity(event.subject)}, {"value", event.value},
                                   {"fade", event.fade}, {"skipped", event.skipped}});
             }
             return {{"count", events.size()}, {"events", std::move(events)}};
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
        {"game_play_sequences_many",
         "Start 1 to 64 level sequences by asset path, each with its own optional loop setting. Validates the full request before playback; missing sequence assets are reported per entry.",
         Schema({{"sequences", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                                 {"items", {{"type", "object"},
                                             {"properties", {{"sequence", {{"type", "string"}}}, {"loop", {{"type", "boolean"}}}}},
                                             {"required", Json::array({"sequence"})}}}}}},
                {"sequences"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             seq::SequenceSystem* system = seq::SequenceSystem::Active();
             if (system == nullptr) throw ToolError("This game has no sequence system (load a scene first)");
             if (!args["sequences"].is_array() || args["sequences"].empty() || args["sequences"].size() > 64) {
                 throw ToolError("\"sequences\" must be an array of 1 to 64 sequence requests");
             }
             struct SequenceRequest {
                 std::string path;
                 bool loop = false;
             };
             std::vector<SequenceRequest> requests;
             requests.reserve(args["sequences"].size());
             for (const Json& item : args["sequences"]) {
                 if (!item.is_object() || !item.contains("sequence") || !item["sequence"].is_string() || item["sequence"].get<std::string>().empty()) {
                     throw ToolError("Every \"sequences\" entry must contain a non-empty string \"sequence\"");
                 }
                 if (item.contains("loop") && !item["loop"].is_boolean()) throw ToolError("\"loop\" must be a boolean");
                 requests.push_back({item["sequence"].get<std::string>(), item.contains("loop") && item["loop"].get<bool>()});
             }
             Json results = Json::array();
             usize started_count = 0;
             for (const SequenceRequest& request : requests) {
                 const Entity entity = system->PlaySequence(request.path, request.loop);
                 const bool started = !entity.IsNull();
                 if (started) ++started_count;
                 Json result = {{"sequence", request.path}, {"loop", request.loop}, {"started", started}};
                 result["entity"] = started ? Json(EntityKey(game.GetWorld(), entity)) : Json(nullptr);
                 results.push_back(std::move(result));
             }
             return {{"started_count", started_count}, {"requested_count", results.size()}, {"results", results}, {"problems", system->Problems()}};
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

    server.AddTool(
        {"game_sequence_controls_many",
         "Queue different play, pause, stop, skip, time, rate or loop commands on up to 64 unique live SequenceComponents. Validates every target and action before queueing; step the game to apply them.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                             {"items", {{"type", "object"},
                                         {"properties", {{"entity", {{"type", "string"}}},
                                                          {"action", {{"type", "string"}, {"description", "play | pause | stop | skip | set_time | set_rate | set_loop"}}},
                                                          {"value", {{"type", "number"}}}}},
                                         {"required", Json::array({"entity", "action"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 sequence commands");
             }
             struct SequenceEdit {
                 Entity entity;
                 std::string action;
                 f32 value = 0.0f;
             };
             std::vector<SequenceEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("action") || !item["action"].is_string()) throw ToolError("Every \"edits\" entry must contain a string \"action\"");
                 const std::string action = item["action"].get<std::string>();
                 if (action != "play" && action != "pause" && action != "stop" && action != "skip" && action != "set_time" && action != "set_rate" && action != "set_loop") {
                     throw ToolError("\"action\" must be play, pause, stop, skip, set_time, set_rate or set_loop");
                 }
                 f32 value = 0.0f;
                 if (item.contains("value")) {
                     if (!item["value"].is_number()) throw ToolError("\"value\" must be a number");
                     value = item["value"].get<f32>();
                     if (!std::isfinite(value)) throw ToolError("\"value\" must be finite");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const SequenceEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat the same entity");
                 }
                 if (world.GetComponent<SequenceComponent>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no SequenceComponent");
                 edits.push_back({entity, action, value});
             }
             Json results = Json::array();
             for (const SequenceEdit& edit : edits) {
                 SequenceComponent* component = world.GetComponent<SequenceComponent>(edit.entity);
                 if (edit.action == "play") component->Play();
                 else if (edit.action == "pause") component->Pause();
                 else if (edit.action == "stop") component->Stop();
                 else if (edit.action == "skip") component->Skip();
                 else if (edit.action == "set_time") component->SetTime(edit.value);
                 else if (edit.action == "set_rate") component->SetRate(edit.value);
                 else component->SetLoop(edit.value != 0.0f);
                 results.push_back({{"entity", EntityKey(world, edit.entity)}, {"action", edit.action}, {"sequence", component->sequence},
                                    {"playing", component->playing}, {"time", component->time}, {"queued_commands", component->commands.size()}});
             }
             return {{"queued_count", results.size()}, {"results", results}};
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

    server.AddTool(
        {"game_set_transforms_many",
         "Set local position, rotation and/or scale on up to 64 unique live entities. Validates every edit before applying any; rotation is normalized.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                                                           {"rotation", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}},
                                                           {"scale", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 transform edits");
             }
             struct TransformEdit {
                 Entity entity;
                 std::optional<Vec3> position;
                 std::optional<Quaternion> rotation;
                 std::optional<Vec3> scale;
             };
             auto read_vec3 = [](const Json& value, const char* name) {
                 if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number()) {
                     throw ToolError(std::string("\"") + name + "\" must be [x, y, z]");
                 }
                 Vec3 result(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>());
                 if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) {
                     throw ToolError(std::string("\"") + name + "\" coordinates must be finite");
                 }
                 return result;
             };
             std::vector<TransformEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("position") && !item.contains("rotation") && !item.contains("scale")) {
                     throw ToolError("Every transform edit must specify at least one of \"position\", \"rotation\" or \"scale\"");
                 }
                 TransformEdit edit{FindEntity(game, item["entity"].get<std::string>())};
                 if (std::find_if(edits.begin(), edits.end(), [&edit](const TransformEdit& previous) { return previous.entity == edit.entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(edit.entity) == nullptr) throw ToolError("Entity " + EntityKey(world, edit.entity) + " has no Transform");
                 if (item.contains("position")) edit.position = read_vec3(item["position"], "position");
                 if (item.contains("scale")) edit.scale = read_vec3(item["scale"], "scale");
                 if (item.contains("rotation")) {
                     const Json& value = item["rotation"];
                     if (!value.is_array() || value.size() != 4 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number() || !value[3].is_number()) {
                         throw ToolError("\"rotation\" must be [x, y, z, w]");
                     }
                     Quaternion rotation(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>());
                     if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) || rotation.LengthSq() <= 1.0e-12f) {
                         throw ToolError("\"rotation\" must be a finite non-zero quaternion");
                     }
                     edit.rotation = rotation.Normalized();
                 }
                 edits.push_back(std::move(edit));
             }
             Json results = Json::array();
             for (const TransformEdit& edit : edits) {
                 Transform* transform = world.GetComponent<Transform>(edit.entity);
                 if (edit.position) transform->position = *edit.position;
                 if (edit.rotation) transform->rotation = *edit.rotation;
                 if (edit.scale) transform->scale = *edit.scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)}, {"position", Json::array({transform->position.x, transform->position.y, transform->position.z})},
                                    {"rotation", Json::array({transform->rotation.x, transform->rotation.y, transform->rotation.z, transform->rotation.w})},
                                    {"scale", Json::array({transform->scale.x, transform->scale.y, transform->scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_transforms_shared_many",
         "Set the same subset of local position, rotation and scale on up to 64 unique live entities. Preserves unspecified fields, normalizes rotation and validates every entity before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                 {"rotation", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}},
                 {"scale", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args.contains("position") && !args.contains("rotation") && !args.contains("scale")) {
                 throw ToolError("Specify at least one of \"position\", \"rotation\" or \"scale\"");
             }
             std::optional<Vec3> position;
             std::optional<Quaternion> rotation;
             std::optional<Vec3> scale;
             auto read_vec3 = [](const Json& value, const char* field) {
                 if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number()) {
                     throw ToolError(std::string("\"") + field + "\" must be [x, y, z]");
                 }
                 const Vec3 result(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>());
                 if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) throw ToolError(std::string("\"") + field + "\" values must be finite");
                 return result;
             };
             if (args.contains("position")) position = read_vec3(args["position"], "position");
             if (args.contains("scale")) scale = read_vec3(args["scale"], "scale");
             if (args.contains("rotation")) {
                 const Json& value = args["rotation"];
                 if (!value.is_array() || value.size() != 4 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number() || !value[3].is_number()) {
                     throw ToolError("\"rotation\" must be [x, y, z, w]");
                 }
                 Quaternion parsed(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>());
                 if (!std::isfinite(parsed.x) || !std::isfinite(parsed.y) || !std::isfinite(parsed.z) || !std::isfinite(parsed.w) ||
                     !std::isfinite(parsed.LengthSq()) || parsed.LengthSq() <= 1.0e-12f) throw ToolError("\"rotation\" must be a finite non-zero quaternion");
                 rotation = parsed.Normalized();
             }
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) throw ToolError("\"entities\" must not repeat an entity");
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (const Entity entity : entities) {
                 Transform* transform = world.GetComponent<Transform>(entity);
                 if (position) transform->position = *position;
                 if (rotation) transform->rotation = *rotation;
                 if (scale) transform->scale = *scale;
                 results.push_back({{"entity", EntityKey(world, entity)},
                                    {"position", Json::array({transform->position.x, transform->position.y, transform->position.z})},
                                    {"rotation", Json::array({transform->rotation.x, transform->rotation.y, transform->rotation.z, transform->rotation.w})},
                                    {"scale", Json::array({transform->scale.x, transform->scale.y, transform->scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_local_many",
         "Add individual local-position offsets to up to 64 unique live entities. Preserves rotation and scale; validates every offset and resulting position before applying the batch.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"offset", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "offset"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 local-position offsets");
             }
             struct PositionEdit { Entity entity; Vec3 offset; Vec3 position; };
             std::vector<PositionEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("offset") || !item["offset"].is_array() || item["offset"].size() != 3 ||
                     !item["offset"][0].is_number() || !item["offset"][1].is_number() || !item["offset"][2].is_number()) {
                     throw ToolError("Every \"offset\" must be [x, y, z]");
                 }
                 const Vec3 offset(item["offset"][0].get<f32>(), item["offset"][1].get<f32>(), item["offset"][2].get<f32>());
                 if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) throw ToolError("\"offset\" values must be finite");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const PositionEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 if (!std::isfinite(transform->position.x) || !std::isfinite(transform->position.y) || !std::isfinite(transform->position.z)) {
                     throw ToolError("Entity " + EntityKey(world, entity) + " has a non-finite local position");
                 }
                 const Vec3 position = transform->position + offset;
                 if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                     throw ToolError("Offset would produce a non-finite local position for " + EntityKey(world, entity));
                 }
                 edits.push_back({entity, offset, position});
             }
             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->position = edit.position;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"offset", Json::array({edit.offset.x, edit.offset.y, edit.offset.z})},
                                    {"position", Json::array({edit.position.x, edit.position.y, edit.position.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_local_shared_many",
         "Add one shared local-position offset to up to 64 unique live entities. Preserves rotation and scale; validates every target and resulting position before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"offset", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "offset"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["offset"].is_array() || args["offset"].size() != 3 || !args["offset"][0].is_number() ||
                 !args["offset"][1].is_number() || !args["offset"][2].is_number()) {
                 throw ToolError("\"offset\" must be [x, y, z]");
             }
             const Vec3 offset(args["offset"][0].get<f32>(), args["offset"][1].get<f32>(), args["offset"][2].get<f32>());
             if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) throw ToolError("\"offset\" values must be finite");
             std::vector<Entity> entities;
             entities.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty handle");
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(entities.begin(), entities.end(), entity) != entities.end()) throw ToolError("\"entities\" must not repeat an entity");
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 if (!std::isfinite(transform->position.x) || !std::isfinite(transform->position.y) || !std::isfinite(transform->position.z)) {
                     throw ToolError("Entity " + EntityKey(world, entity) + " has a non-finite local position");
                 }
                 const Vec3 position = transform->position + offset;
                 if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                     throw ToolError("Offset would produce a non-finite local position for " + EntityKey(world, entity));
                 }
                 entities.push_back(entity);
             }
             Json results = Json::array();
             for (Entity entity : entities) {
                 Transform* transform = world.GetComponent<Transform>(entity);
                 transform->position = transform->position + offset;
                 results.push_back({{"entity", EntityKey(world, entity)}, {"offset", Json::array({offset.x, offset.y, offset.z})},
                                    {"position", Json::array({transform->position.x, transform->position.y, transform->position.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_world_many",
         "Add individual world-position offsets to up to 64 unique live entities while preserving local rotation and scale. Resolves parent transforms; rejects ancestor overlap, cycles, excessive depth and singular parents before applying.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"offset", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "offset"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-position offsets");
             }
             struct PositionEdit { Entity entity; Vec3 offset; Vec3 world_position; Vec3 local_position; };
             std::vector<PositionEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("offset") || !item["offset"].is_array() || item["offset"].size() != 3 ||
                     !item["offset"][0].is_number() || !item["offset"][1].is_number() || !item["offset"][2].is_number()) {
                     throw ToolError("Every \"offset\" must be [x, y, z]");
                 }
                 const Vec3 offset(item["offset"][0].get<f32>(), item["offset"][1].get<f32>(), item["offset"][2].get<f32>());
                 if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) throw ToolError("\"offset\" values must be finite");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const PositionEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, offset, {}, {}});
             }
             for (const PositionEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> ancestors;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) {
                         throw ToolError("An edited entity has a hierarchy cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [current](const PositionEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     ancestors.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (PositionEdit& edit : edits) {
                 const Mat4 current_world = ComputeWorldTransform(world, game.Guids(), edit.entity);
                 for (const Vec4& column : current_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("Entity " + EntityKey(world, edit.entity) + " has a non-finite world transform");
                     }
                 }
                 const Vec4 world_origin = current_world * Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                 if (!std::isfinite(world_origin.w) || std::abs(world_origin.w) <= 1.0e-12f) throw ToolError("Cannot resolve entity world position");
                 edit.world_position = Vec3(world_origin.x / world_origin.w, world_origin.y / world_origin.w, world_origin.z / world_origin.w) + edit.offset;
                 if (!std::isfinite(edit.world_position.x) || !std::isfinite(edit.world_position.y) || !std::isfinite(edit.world_position.z)) {
                     throw ToolError("Offset would produce a non-finite world position for " + EntityKey(world, edit.entity));
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 inverse_parent = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) {
                     const Mat4 parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                     for (const Vec4& column : parent_world.cols) {
                         if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                             throw ToolError("Cannot offset through a non-finite parent transform");
                         }
                     }
                     if (!parent_world.TryInverse(inverse_parent)) throw ToolError("Cannot offset through a singular parent transform");
                 }
                 const Vec4 local = inverse_parent * Vec4(edit.world_position.x, edit.world_position.y, edit.world_position.z, 1.0f);
                 if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) || std::abs(local.w) <= 1.0e-12f) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
                 edit.local_position = Vec3(local.x / local.w, local.y / local.w, local.z / local.w);
                 if (!std::isfinite(edit.local_position.x) || !std::isfinite(edit.local_position.y) || !std::isfinite(edit.local_position.z)) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
             }
             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->position = edit.local_position;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"offset", Json::array({edit.offset.x, edit.offset.y, edit.offset.z})},
                                    {"world_position", Json::array({edit.world_position.x, edit.world_position.y, edit.world_position.z})},
                                    {"local_position", Json::array({edit.local_position.x, edit.local_position.y, edit.local_position.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_world_shared_many",
         "Add one shared world-position offset to up to 64 unique live entities while preserving local rotation and scale. Converts through parent transforms; rejects ancestor overlap, cycles, excessive depth and singular parents before applying.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"offset", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "offset"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["offset"].is_array() || args["offset"].size() != 3 || !args["offset"][0].is_number() ||
                 !args["offset"][1].is_number() || !args["offset"][2].is_number()) {
                 throw ToolError("\"offset\" must be [x, y, z]");
             }
             const Vec3 offset(args["offset"][0].get<f32>(), args["offset"][1].get<f32>(), args["offset"][2].get<f32>());
             if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) throw ToolError("\"offset\" values must be finite");
             struct PositionEdit { Entity entity; Vec3 world_position; Vec3 local_position; };
             std::vector<PositionEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty handle");
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const PositionEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, {}, {}});
             }
             for (const PositionEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> ancestors;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) {
                         throw ToolError("An edited entity has a hierarchy cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [current](const PositionEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     ancestors.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (PositionEdit& edit : edits) {
                 const Mat4 current_world = ComputeWorldTransform(world, game.Guids(), edit.entity);
                 for (const Vec4& column : current_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("Entity " + EntityKey(world, edit.entity) + " has a non-finite world transform");
                     }
                 }
                 const Vec4 world_origin = current_world * Vec4(0.0f, 0.0f, 0.0f, 1.0f);
                 if (!std::isfinite(world_origin.w) || std::abs(world_origin.w) <= 1.0e-12f) throw ToolError("Cannot resolve entity world position");
                 edit.world_position = Vec3(world_origin.x / world_origin.w, world_origin.y / world_origin.w, world_origin.z / world_origin.w) + offset;
                 if (!std::isfinite(edit.world_position.x) || !std::isfinite(edit.world_position.y) || !std::isfinite(edit.world_position.z)) {
                     throw ToolError("Offset would produce a non-finite world position for " + EntityKey(world, edit.entity));
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 inverse_parent = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) {
                     const Mat4 parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                     for (const Vec4& column : parent_world.cols) {
                         if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                             throw ToolError("Cannot offset through a non-finite parent transform");
                         }
                     }
                     if (!parent_world.TryInverse(inverse_parent)) throw ToolError("Cannot offset through a singular parent transform");
                 }
                 const Vec4 local = inverse_parent * Vec4(edit.world_position.x, edit.world_position.y, edit.world_position.z, 1.0f);
                 if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) || std::abs(local.w) <= 1.0e-12f) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
                 edit.local_position = Vec3(local.x / local.w, local.y / local.w, local.z / local.w);
                 if (!std::isfinite(edit.local_position.x) || !std::isfinite(edit.local_position.y) || !std::isfinite(edit.local_position.z)) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
             }
             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->position = edit.local_position;
                 results.push_back({{"entity", EntityKey(world, edit.entity)}, {"offset", Json::array({offset.x, offset.y, offset.z})},
                                    {"world_position", Json::array({edit.world_position.x, edit.world_position.y, edit.world_position.z})},
                                    {"local_position", Json::array({edit.local_position.x, edit.local_position.y, edit.local_position.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_scale_entities_local_many",
         "Multiply local scale independently on up to 64 unique live entities using per-entity component factors. Preserves local position and rotation; validates factors and resulting scales for the entire batch before applying.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"factor", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "factor"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 local-scale edits");
             }
             struct ScaleEdit { Entity entity; Vec3 scale; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("factor") || !item["factor"].is_array() || item["factor"].size() != 3 ||
                     !item["factor"][0].is_number() || !item["factor"][1].is_number() || !item["factor"][2].is_number()) {
                     throw ToolError("Every \"factor\" must be [x, y, z]");
                 }
                 const Vec3 factor(item["factor"][0].get<f32>(), item["factor"][1].get<f32>(), item["factor"][2].get<f32>());
                 if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z)) throw ToolError("\"factor\" values must be finite");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Vec3& current = transform->scale;
                 const Vec3 updated(current.x * factor.x, current.y * factor.y, current.z * factor.z);
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) ||
                     !std::isfinite(updated.x) || !std::isfinite(updated.y) || !std::isfinite(updated.z)) {
                     throw ToolError("Local scale multiplication produced a non-finite result for " + EntityKey(world, entity));
                 }
                 edits.push_back({entity, updated});
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 Transform* transform = world.GetComponent<Transform>(edit.entity);
                 transform->scale = edit.scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"scale", Json::array({edit.scale.x, edit.scale.y, edit.scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_scale_entities_local_shared_many",
         "Multiply local scale by one shared component factor on up to 64 unique live entities. Preserves local position and rotation; validates all handles, factor values and resulting scales before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"factor", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "factor"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["factor"].is_array() || args["factor"].size() != 3 ||
                 !args["factor"][0].is_number() || !args["factor"][1].is_number() || !args["factor"][2].is_number()) {
                 throw ToolError("\"factor\" must be [x, y, z]");
             }
             const Vec3 factor(args["factor"][0].get<f32>(), args["factor"][1].get<f32>(), args["factor"][2].get<f32>());
             if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z)) throw ToolError("\"factor\" values must be finite");
             struct ScaleEdit { Entity entity; Vec3 scale; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Vec3& current = transform->scale;
                 const Vec3 updated(current.x * factor.x, current.y * factor.y, current.z * factor.z);
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) ||
                     !std::isfinite(updated.x) || !std::isfinite(updated.y) || !std::isfinite(updated.z)) {
                     throw ToolError("Local scale multiplication produced a non-finite result for " + EntityKey(world, entity));
                 }
                 edits.push_back({entity, updated});
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"scale", Json::array({edit.scale.x, edit.scale.y, edit.scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_local_scale_many",
         "Add individual component deltas to local scale on up to 64 unique live entities. Preserves local position and rotation; validates all deltas and resulting scales before applying the batch.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"delta", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "delta"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 local-scale edits");
             }
             struct ScaleEdit { Entity entity; Vec3 before; Vec3 delta; Vec3 after; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("delta") || !item["delta"].is_array() || item["delta"].size() != 3 ||
                     !item["delta"][0].is_number() || !item["delta"][1].is_number() || !item["delta"][2].is_number()) {
                     throw ToolError("Every \"delta\" must be [x, y, z]");
                 }
                 const Vec3 delta(item["delta"][0].get<f32>(), item["delta"][1].get<f32>(), item["delta"][2].get<f32>());
                 if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) throw ToolError("\"delta\" values must be finite");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Vec3 current = transform->scale;
                 const Vec3 updated = current + delta;
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) ||
                     !std::isfinite(updated.x) || !std::isfinite(updated.y) || !std::isfinite(updated.z)) {
                     throw ToolError("Local scale offset produced a non-finite result for " + EntityKey(world, entity));
                 }
                 edits.push_back({entity, current, delta, updated});
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.after;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"previous_scale", Json::array({edit.before.x, edit.before.y, edit.before.z})},
                                    {"delta", Json::array({edit.delta.x, edit.delta.y, edit.delta.z})},
                                    {"scale", Json::array({edit.after.x, edit.after.y, edit.after.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_local_scale_shared_many",
         "Add one shared component delta to local scale on up to 64 unique live entities. Preserves local position and rotation; validates all entities and resulting scales before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"delta", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "delta"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["delta"].is_array() || args["delta"].size() != 3 ||
                 !args["delta"][0].is_number() || !args["delta"][1].is_number() || !args["delta"][2].is_number()) {
                 throw ToolError("\"delta\" must be [x, y, z]");
             }
             const Vec3 delta(args["delta"][0].get<f32>(), args["delta"][1].get<f32>(), args["delta"][2].get<f32>());
             if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) throw ToolError("\"delta\" values must be finite");
             struct ScaleEdit { Entity entity; Vec3 before; Vec3 after; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Vec3 current = transform->scale;
                 const Vec3 updated = current + delta;
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) ||
                     !std::isfinite(updated.x) || !std::isfinite(updated.y) || !std::isfinite(updated.z)) {
                     throw ToolError("Local scale offset produced a non-finite result for " + EntityKey(world, entity));
                 }
                 edits.push_back({entity, current, updated});
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.after;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"previous_scale", Json::array({edit.before.x, edit.before.y, edit.before.z})},
                                    {"delta", Json::array({delta.x, delta.y, delta.z})},
                                    {"scale", Json::array({edit.after.x, edit.after.y, edit.after.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_rotate_entities_local_many",
         "Apply individual local-space quaternion deltas to up to 64 unique live entities. Preserves position and scale; validates the full request and normalizes each resulting rotation before applying.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"delta", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "delta"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 rotation edits");
             }
             struct RotationEdit { Entity entity; Quaternion rotation; };
             std::vector<RotationEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("delta") || !item["delta"].is_array() || item["delta"].size() != 4 ||
                     !item["delta"][0].is_number() || !item["delta"][1].is_number() || !item["delta"][2].is_number() || !item["delta"][3].is_number()) {
                     throw ToolError("Every \"delta\" must be [x, y, z, w]");
                 }
                 Quaternion delta(item["delta"][0].get<f32>(), item["delta"][1].get<f32>(), item["delta"][2].get<f32>(), item["delta"][3].get<f32>());
                 if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) || !std::isfinite(delta.w) ||
                     !std::isfinite(delta.LengthSq()) || delta.LengthSq() <= 1.0e-12f) {
                     throw ToolError("\"delta\" must be a finite non-zero quaternion");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const RotationEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Quaternion& current = transform->rotation;
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) || !std::isfinite(current.w) ||
                     !std::isfinite(current.LengthSq()) || current.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, entity) + " has an invalid local rotation");
                 }
                 const Quaternion updated = (current.Normalized() * delta.Normalized()).Normalized();
                 if (!std::isfinite(updated.x) || !std::isfinite(updated.y) || !std::isfinite(updated.z) || !std::isfinite(updated.w)) {
                     throw ToolError("Rotation delta produced a non-finite result");
                 }
                 edits.push_back({entity, updated});
             }
             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->rotation = edit.rotation;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"rotation", Json::array({edit.rotation.x, edit.rotation.y, edit.rotation.z, edit.rotation.w})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_rotate_entities_local_shared_many",
         "Apply one local-space quaternion delta to up to 64 unique live entities. Preserves local position and scale; validates all handles and rotations and normalizes every result before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"delta", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}}},
                {"entities", "delta"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["delta"].is_array() || args["delta"].size() != 4 ||
                 !args["delta"][0].is_number() || !args["delta"][1].is_number() || !args["delta"][2].is_number() || !args["delta"][3].is_number()) {
                 throw ToolError("\"delta\" must be [x, y, z, w]");
             }
             Quaternion delta(args["delta"][0].get<f32>(), args["delta"][1].get<f32>(), args["delta"][2].get<f32>(), args["delta"][3].get<f32>());
             if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) || !std::isfinite(delta.w) ||
                 !std::isfinite(delta.LengthSq()) || delta.LengthSq() <= 1.0e-12f) throw ToolError("\"delta\" must be a finite non-zero quaternion");
             delta = delta.Normalized();
             struct RotationEdit { Entity entity; Quaternion rotation; };
             std::vector<RotationEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const RotationEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Quaternion& current = transform->rotation;
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) || !std::isfinite(current.w) ||
                     !std::isfinite(current.LengthSq()) || current.LengthSq() <= 1.0e-12f) throw ToolError("Entity " + EntityKey(world, entity) + " has an invalid local rotation");
                 const Quaternion updated = (current.Normalized() * delta).Normalized();
                 if (!std::isfinite(updated.x) || !std::isfinite(updated.y) || !std::isfinite(updated.z) || !std::isfinite(updated.w)) {
                     throw ToolError("Rotation delta produced a non-finite result");
                 }
                 edits.push_back({entity, updated});
             }
             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->rotation = edit.rotation;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"rotation", Json::array({edit.rotation.x, edit.rotation.y, edit.rotation.z, edit.rotation.w})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_rotate_entities_world_many",
         "Apply per-entity world-space quaternion deltas to up to 64 unique live entities. Preserves local position and scale, normalizes rotations, and rejects ancestor overlap or invalid hierarchies before applying the batch.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"delta", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "delta"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-rotation edits");
             }
             struct RotationEdit { Entity entity; Quaternion world_rotation; Quaternion local_rotation; };
             std::vector<RotationEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("delta") || !item["delta"].is_array() || item["delta"].size() != 4 ||
                     !item["delta"][0].is_number() || !item["delta"][1].is_number() || !item["delta"][2].is_number() || !item["delta"][3].is_number()) {
                     throw ToolError("Every \"delta\" must be [x, y, z, w]");
                 }
                 Quaternion delta(item["delta"][0].get<f32>(), item["delta"][1].get<f32>(), item["delta"][2].get<f32>(), item["delta"][3].get<f32>());
                 if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) || !std::isfinite(delta.w) ||
                     !std::isfinite(delta.LengthSq()) || delta.LengthSq() <= 1.0e-12f) {
                     throw ToolError("\"delta\" must be a finite non-zero quaternion");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const RotationEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Quaternion& current = transform->rotation;
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) || !std::isfinite(current.w) ||
                     !std::isfinite(current.LengthSq()) || current.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, entity) + " has an invalid local rotation");
                 }
                 const Entity parent = GetParent(world, game.Guids(), entity);
                 std::vector<Quaternion> parent_locals;
                 Entity ancestor = parent;
                 std::vector<Entity> seen_ancestors;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == entity || std::find(seen_ancestors.begin(), seen_ancestors.end(), ancestor) != seen_ancestors.end()) {
                         throw ToolError("Entity hierarchy contains a cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const RotationEdit& edit) { return edit.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     seen_ancestors.push_back(ancestor);
                     const Transform* parent_transform = world.GetComponent<Transform>(ancestor);
                     if (parent_transform == nullptr) break;
                     const Quaternion& local = parent_transform->rotation;
                     if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) ||
                         !std::isfinite(local.LengthSq()) || local.LengthSq() <= 1.0e-12f) {
                         throw ToolError("An ancestor has an invalid rotation");
                     }
                     parent_locals.push_back(local.Normalized());
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
                 Quaternion parent_world = Quaternion::Identity();
                 for (auto it = parent_locals.rbegin(); it != parent_locals.rend(); ++it) parent_world = (parent_world * *it).Normalized();
                 const Quaternion current_world = (parent_world * current.Normalized()).Normalized();
                 delta = delta.Normalized();
                 const Quaternion desired_world = (delta * current_world).Normalized();
                 const Quaternion inverse_parent{-parent_world.x, -parent_world.y, -parent_world.z, parent_world.w};
                 const Quaternion desired_local = (inverse_parent * desired_world).Normalized();
                 if (!std::isfinite(desired_local.x) || !std::isfinite(desired_local.y) || !std::isfinite(desired_local.z) || !std::isfinite(desired_local.w)) {
                     throw ToolError("World rotation delta produced a non-finite local rotation");
                 }
                 edits.push_back({entity, desired_world, desired_local});
             }
             for (const RotationEdit& edit : edits) {
                 Entity ancestor = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen_ancestors;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == edit.entity || std::find(seen_ancestors.begin(), seen_ancestors.end(), ancestor) != seen_ancestors.end()) {
                         throw ToolError("Entity hierarchy contains a cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const RotationEdit& other) { return other.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     seen_ancestors.push_back(ancestor);
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
             }
             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->rotation = edit.local_rotation;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_rotation", Json::array({edit.world_rotation.x, edit.world_rotation.y, edit.world_rotation.z, edit.world_rotation.w})},
                                    {"local_rotation", Json::array({edit.local_rotation.x, edit.local_rotation.y, edit.local_rotation.z, edit.local_rotation.w})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_rotate_entities_world_shared_many",
         "Apply one world-space quaternion delta to up to 64 unique live entities. Preserves local position and scale, resolves parent orientations, and rejects ancestor overlap or invalid hierarchies before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"delta", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}}},
                {"entities", "delta"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["delta"].is_array() || args["delta"].size() != 4 ||
                 !args["delta"][0].is_number() || !args["delta"][1].is_number() ||
                 !args["delta"][2].is_number() || !args["delta"][3].is_number()) {
                 throw ToolError("\"delta\" must be [x, y, z, w]");
             }
             Quaternion delta(args["delta"][0].get<f32>(), args["delta"][1].get<f32>(),
                              args["delta"][2].get<f32>(), args["delta"][3].get<f32>());
             if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) || !std::isfinite(delta.w) ||
                 !std::isfinite(delta.LengthSq()) || delta.LengthSq() <= 1.0e-12f) {
                 throw ToolError("\"delta\" must be a finite non-zero quaternion");
             }
             delta = delta.Normalized();
             struct RotationEdit { Entity entity; Quaternion world_rotation; Quaternion local_rotation; };
             std::vector<RotationEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const RotationEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 const Quaternion& current = transform->rotation;
                 if (!std::isfinite(current.x) || !std::isfinite(current.y) || !std::isfinite(current.z) || !std::isfinite(current.w) ||
                     !std::isfinite(current.LengthSq()) || current.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, entity) + " has an invalid local rotation");
                 }
                 Entity ancestor = GetParent(world, game.Guids(), entity);
                 std::vector<Entity> seen;
                 std::vector<Quaternion> parent_locals;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == entity || std::find(seen.begin(), seen.end(), ancestor) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const RotationEdit& edit) { return edit.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(ancestor);
                     const Transform* parent_transform = world.GetComponent<Transform>(ancestor);
                     if (parent_transform == nullptr) break;
                     const Quaternion& local = parent_transform->rotation;
                     if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) ||
                         !std::isfinite(local.LengthSq()) || local.LengthSq() <= 1.0e-12f) throw ToolError("An ancestor has an invalid rotation");
                     parent_locals.push_back(local.Normalized());
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
                 Quaternion parent_world = Quaternion::Identity();
                 for (auto it = parent_locals.rbegin(); it != parent_locals.rend(); ++it) parent_world = (parent_world * *it).Normalized();
                 const Quaternion current_world = (parent_world * current.Normalized()).Normalized();
                 const Quaternion desired_world = (delta * current_world).Normalized();
                 const Quaternion inverse_parent{-parent_world.x, -parent_world.y, -parent_world.z, parent_world.w};
                 const Quaternion desired_local = (inverse_parent * desired_world).Normalized();
                 if (!std::isfinite(desired_world.x) || !std::isfinite(desired_world.y) || !std::isfinite(desired_world.z) || !std::isfinite(desired_world.w) ||
                     !std::isfinite(desired_local.x) || !std::isfinite(desired_local.y) || !std::isfinite(desired_local.z) || !std::isfinite(desired_local.w)) {
                     throw ToolError("World rotation delta produced a non-finite result");
                 }
                 edits.push_back({entity, desired_world, desired_local});
             }
             for (const RotationEdit& edit : edits) {
                 Entity ancestor = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == edit.entity || std::find(seen.begin(), seen.end(), ancestor) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const RotationEdit& other) { return other.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(ancestor);
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
             }
             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->rotation = edit.local_rotation;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_rotation", Json::array({edit.world_rotation.x, edit.world_rotation.y, edit.world_rotation.z, edit.world_rotation.w})},
                                    {"local_rotation", Json::array({edit.local_rotation.x, edit.local_rotation.y, edit.local_rotation.z, edit.local_rotation.w})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_transforms_many",
         "Set any combination of world position, rotation and scale on up to 64 unique live entities. Converts through parent transforms while preserving unspecified local fields; validates the full batch and rejects ancestor overlap, cycles, excessive depth and singular parents before applying.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                                                           {"rotation", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}},
                                                           {"scale", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-transform edits");
             }
             struct WorldTransformEdit {
                 Entity entity;
                 std::optional<Vec3> world_position;
                 std::optional<Quaternion> world_rotation;
                 std::optional<Vec3> world_scale;
                 std::optional<Vec3> local_position;
                 std::optional<Quaternion> local_rotation;
                 std::optional<Vec3> local_scale;
             };
             auto read_vec3 = [](const Json& value, const char* name) {
                 if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number()) {
                     throw ToolError(std::string("\"") + name + "\" must be [x, y, z]");
                 }
                 const Vec3 result(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>());
                 if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) {
                     throw ToolError(std::string("\"") + name + "\" values must be finite");
                 }
                 return result;
             };
             std::vector<WorldTransformEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("position") && !item.contains("rotation") && !item.contains("scale")) {
                     throw ToolError("Every world-transform edit must specify at least one of \"position\", \"rotation\" or \"scale\"");
                 }
                 WorldTransformEdit edit{FindEntity(game, item["entity"].get<std::string>())};
                 if (std::find_if(edits.begin(), edits.end(), [&edit](const WorldTransformEdit& previous) { return previous.entity == edit.entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(edit.entity) == nullptr) throw ToolError("Entity " + EntityKey(world, edit.entity) + " has no Transform");
                 if (item.contains("position")) edit.world_position = read_vec3(item["position"], "position");
                 if (item.contains("scale")) {
                     edit.world_scale = read_vec3(item["scale"], "scale");
                     if (edit.world_scale->x < 0.0f || edit.world_scale->y < 0.0f || edit.world_scale->z < 0.0f) {
                         throw ToolError("\"scale\" must contain non-negative world magnitudes");
                     }
                 }
                 if (item.contains("rotation")) {
                     const Json& value = item["rotation"];
                     if (!value.is_array() || value.size() != 4 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number() || !value[3].is_number()) {
                         throw ToolError("\"rotation\" must be [x, y, z, w]");
                     }
                     Quaternion rotation(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>());
                     if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                         !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) {
                         throw ToolError("\"rotation\" must be a finite non-zero quaternion");
                     }
                     edit.world_rotation = rotation.Normalized();
                 }
                 edits.push_back(std::move(edit));
             }
             for (const WorldTransformEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("An edited entity has a hierarchy cycle");
                     if (std::find_if(edits.begin(), edits.end(), [current](const WorldTransformEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (WorldTransformEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 if (!std::isfinite(transform->position.x) || !std::isfinite(transform->position.y) || !std::isfinite(transform->position.z) ||
                     !std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(transform->rotation.x) || !std::isfinite(transform->rotation.y) || !std::isfinite(transform->rotation.z) ||
                     !std::isfinite(transform->rotation.w) || !std::isfinite(transform->rotation.LengthSq()) || transform->rotation.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, edit.entity) + " has an invalid local transform");
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world_matrix = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world_matrix = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world_matrix.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("Parent has a non-finite world transform");
                     }
                 }
                 if (edit.world_position) {
                     Mat4 inverse_parent = Mat4::Identity();
                     if (!parent_world_matrix.TryInverse(inverse_parent)) throw ToolError("Cannot set world position through a singular parent transform");
                     const Vec4 local = inverse_parent * Vec4(edit.world_position->x, edit.world_position->y, edit.world_position->z, 1.0f);
                     if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) || std::abs(local.w) <= 1.0e-12f) {
                         throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                     }
                     edit.local_position = Vec3(local.x / local.w, local.y / local.w, local.z / local.w);
                     if (!std::isfinite(edit.local_position->x) || !std::isfinite(edit.local_position->y) || !std::isfinite(edit.local_position->z)) {
                         throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                     }
                 }
                 std::vector<Quaternion> parent_locals;
                 Entity ancestor = parent;
                 std::vector<Entity> seen;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == edit.entity || std::find(seen.begin(), seen.end(), ancestor) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     seen.push_back(ancestor);
                     const Transform* ancestor_transform = world.GetComponent<Transform>(ancestor);
                     if (ancestor_transform == nullptr) break;
                     const Quaternion& local_rotation = ancestor_transform->rotation;
                     if (!std::isfinite(local_rotation.x) || !std::isfinite(local_rotation.y) || !std::isfinite(local_rotation.z) || !std::isfinite(local_rotation.w) ||
                         !std::isfinite(local_rotation.LengthSq()) || local_rotation.LengthSq() <= 1.0e-12f) throw ToolError("An ancestor has an invalid rotation");
                     parent_locals.push_back(local_rotation.Normalized());
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
                 Quaternion parent_world_rotation = Quaternion::Identity();
                 for (auto it = parent_locals.rbegin(); it != parent_locals.rend(); ++it) parent_world_rotation = (parent_world_rotation * *it).Normalized();
                 if (edit.world_rotation) {
                     const Quaternion inverse_parent{-parent_world_rotation.x, -parent_world_rotation.y, -parent_world_rotation.z, parent_world_rotation.w};
                     edit.local_rotation = (inverse_parent * *edit.world_rotation).Normalized();
                 }
                 if (edit.world_scale) {
                     Transform unit_transform = *transform;
                     unit_transform.rotation = edit.local_rotation.value_or(transform->rotation.Normalized());
                     unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                     const Mat4 unit_world = parent_world_matrix * LocalTransformMatrix(unit_transform);
                     const f32 axis_lengths[3] = {
                         Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                         Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                         Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                     const f32 requested[3] = {edit.world_scale->x, edit.world_scale->y, edit.world_scale->z};
                     const f32 signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f,
                                           transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                           transform->scale.z < 0.0f ? -1.0f : 1.0f};
                     f32 local[3]{};
                     for (int axis = 0; axis < 3; ++axis) {
                         if (!std::isfinite(axis_lengths[axis])) throw ToolError("Parent transform has a non-finite world basis");
                         if (axis_lengths[axis] <= 1.0e-12f) {
                             if (requested[axis] > 0.0f) throw ToolError("Cannot set non-zero world scale on an axis collapsed by the parent transform");
                             local[axis] = 0.0f;
                         } else {
                             local[axis] = signs[axis] * requested[axis] / axis_lengths[axis];
                             if (!std::isfinite(local[axis]) || (requested[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale would produce an invalid local scale");
                         }
                     }
                     edit.local_scale = Vec3(local[0], local[1], local[2]);
                 }
                 if (edit.local_rotation && (!std::isfinite(edit.local_rotation->x) || !std::isfinite(edit.local_rotation->y) ||
                     !std::isfinite(edit.local_rotation->z) || !std::isfinite(edit.local_rotation->w))) throw ToolError("World rotation produced a non-finite local rotation");
             }
             Json results = Json::array();
             for (const WorldTransformEdit& edit : edits) {
                 Transform* transform = world.GetComponent<Transform>(edit.entity);
                 if (edit.local_position) transform->position = *edit.local_position;
                 if (edit.local_rotation) transform->rotation = *edit.local_rotation;
                 if (edit.local_scale) transform->scale = *edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"position", edit.world_position ? Json::array({edit.world_position->x, edit.world_position->y, edit.world_position->z}) : Json(nullptr)},
                                    {"rotation", edit.world_rotation ? Json::array({edit.world_rotation->x, edit.world_rotation->y, edit.world_rotation->z, edit.world_rotation->w}) : Json(nullptr)},
                                    {"scale", edit.world_scale ? Json::array({edit.world_scale->x, edit.world_scale->y, edit.world_scale->z}) : Json(nullptr)},
                                    {"local_position", Json::array({transform->position.x, transform->position.y, transform->position.z})},
                                    {"local_rotation", Json::array({transform->rotation.x, transform->rotation.y, transform->rotation.z, transform->rotation.w})},
                                    {"local_scale", Json::array({transform->scale.x, transform->scale.y, transform->scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_transforms_shared_many",
         "Set the same combination of world position, rotation and scale on up to 64 unique live entities. Converts through parent transforms, preserves unspecified local fields, and validates all entities, hierarchies and conversions before applying.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}},
                 {"rotation", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}},
                 {"scale", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args.contains("position") && !args.contains("rotation") && !args.contains("scale")) {
                 throw ToolError("Specify at least one of \"position\", \"rotation\" or \"scale\"");
             }
             auto read_vec3 = [](const Json& value, const char* name) {
                 if (!value.is_array() || value.size() != 3 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number()) {
                     throw ToolError(std::string("\"") + name + "\" must be [x, y, z]");
                 }
                 const Vec3 result(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>());
                 if (!std::isfinite(result.x) || !std::isfinite(result.y) || !std::isfinite(result.z)) throw ToolError(std::string("\"") + name + "\" values must be finite");
                 return result;
             };
             std::optional<Vec3> world_position;
             std::optional<Quaternion> world_rotation;
             std::optional<Vec3> world_scale;
             if (args.contains("position")) world_position = read_vec3(args["position"], "position");
             if (args.contains("scale")) {
                 world_scale = read_vec3(args["scale"], "scale");
                 if (world_scale->x < 0.0f || world_scale->y < 0.0f || world_scale->z < 0.0f) throw ToolError("\"scale\" must contain non-negative world magnitudes");
             }
             if (args.contains("rotation")) {
                 const Json& value = args["rotation"];
                 if (!value.is_array() || value.size() != 4 || !value[0].is_number() || !value[1].is_number() || !value[2].is_number() || !value[3].is_number()) {
                     throw ToolError("\"rotation\" must be [x, y, z, w]");
                 }
                 Quaternion rotation(value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>());
                 if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                     !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) throw ToolError("\"rotation\" must be a finite non-zero quaternion");
                 world_rotation = rotation.Normalized();
             }
             struct WorldTransformEdit { Entity entity; Vec3 local_position; Quaternion local_rotation; Vec3 local_scale; };
             std::vector<WorldTransformEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const WorldTransformEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 const Transform* transform = world.GetComponent<Transform>(entity);
                 if (transform == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, transform->position, transform->rotation, transform->scale});
             }
             for (const WorldTransformEdit& edit : edits) {
                 Entity ancestor = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == edit.entity || std::find(seen.begin(), seen.end(), ancestor) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const WorldTransformEdit& other) { return other.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(ancestor);
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
             }
             for (WorldTransformEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 const auto finite_vec = [](const Vec3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); };
                 if (!finite_vec(transform->position) || !finite_vec(transform->scale) || !std::isfinite(transform->rotation.x) ||
                     !std::isfinite(transform->rotation.y) || !std::isfinite(transform->rotation.z) || !std::isfinite(transform->rotation.w) ||
                     !std::isfinite(transform->rotation.LengthSq()) || transform->rotation.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, edit.entity) + " has an invalid local transform");
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) throw ToolError("Parent has a non-finite world transform");
                 }
                 if (world_position) {
                     Mat4 inverse_parent = Mat4::Identity();
                     if (!parent_world.TryInverse(inverse_parent)) throw ToolError("Cannot set world position through a singular parent transform");
                     const Vec4 local = inverse_parent * Vec4(world_position->x, world_position->y, world_position->z, 1.0f);
                     if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) || std::abs(local.w) <= 1.0e-12f) {
                         throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                     }
                     edit.local_position = Vec3(local.x / local.w, local.y / local.w, local.z / local.w);
                     if (!finite_vec(edit.local_position)) throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
                 if (world_rotation || world_scale) {
                     std::vector<Quaternion> parent_locals;
                     Entity current = parent;
                     std::vector<Entity> seen;
                     for (int depth = 0; !current.IsNull(); ++depth) {
                         if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                         if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                         seen.push_back(current);
                         const Transform* parent_transform = world.GetComponent<Transform>(current);
                         if (parent_transform == nullptr) break;
                         const Quaternion& local = parent_transform->rotation;
                         if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) ||
                             !std::isfinite(local.LengthSq()) || local.LengthSq() <= 1.0e-12f) throw ToolError("An ancestor has an invalid rotation");
                         parent_locals.push_back(local.Normalized());
                         current = GetParent(world, game.Guids(), current);
                     }
                     Quaternion parent_world_rotation = Quaternion::Identity();
                     for (auto it = parent_locals.rbegin(); it != parent_locals.rend(); ++it) parent_world_rotation = (parent_world_rotation * *it).Normalized();
                     if (world_rotation) {
                         const Quaternion inverse_parent{-parent_world_rotation.x, -parent_world_rotation.y, -parent_world_rotation.z, parent_world_rotation.w};
                         edit.local_rotation = (inverse_parent * *world_rotation).Normalized();
                     }
                     if (world_scale) {
                         Transform unit_transform = *transform;
                         unit_transform.rotation = world_rotation ? edit.local_rotation : transform->rotation.Normalized();
                         unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                         const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                         const f32 axis_lengths[3] = {
                             Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                             Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                             Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                         const f32 requested[3] = {world_scale->x, world_scale->y, world_scale->z};
                         const f32 signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f, transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                               transform->scale.z < 0.0f ? -1.0f : 1.0f};
                         f32 local[3]{};
                         for (int axis = 0; axis < 3; ++axis) {
                             if (!std::isfinite(axis_lengths[axis])) throw ToolError("Parent transform has a non-finite world basis");
                             if (axis_lengths[axis] <= 1.0e-12f) {
                                 if (requested[axis] > 0.0f) throw ToolError("Cannot set non-zero world scale on an axis collapsed by the parent transform");
                                 local[axis] = 0.0f;
                             } else {
                                 local[axis] = signs[axis] * requested[axis] / axis_lengths[axis];
                                 if (!std::isfinite(local[axis]) || (requested[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale would produce an invalid local scale");
                             }
                         }
                         edit.local_scale = Vec3(local[0], local[1], local[2]);
                     }
                 }
             }
             Json results = Json::array();
             for (const WorldTransformEdit& edit : edits) {
                 Transform* transform = world.GetComponent<Transform>(edit.entity);
                 transform->position = edit.local_position;
                 transform->rotation = edit.local_rotation;
                 transform->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"local_position", Json::array({edit.local_position.x, edit.local_position.y, edit.local_position.z})},
                                    {"local_rotation", Json::array({edit.local_rotation.x, edit.local_rotation.y, edit.local_rotation.z, edit.local_rotation.w})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_positions_many",
         "Set world positions for up to 64 unique live entities while preserving local rotation and scale. Converts through parent transforms; rejects ancestor overlap, cycles, excessive depth and singular parent transforms.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "position"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-position edits");
             }
             struct PositionEdit { Entity entity; Vec3 world_position; Vec3 local_position; };
             std::vector<PositionEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("position") || !item["position"].is_array() || item["position"].size() != 3 ||
                     !item["position"][0].is_number() || !item["position"][1].is_number() || !item["position"][2].is_number()) {
                     throw ToolError("Every \"position\" must be [x, y, z]");
                 }
                 const Vec3 position(item["position"][0].get<f32>(), item["position"][1].get<f32>(), item["position"][2].get<f32>());
                 if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) throw ToolError("\"position\" values must be finite");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const PositionEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, position, {}});
             }
             for (const PositionEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> ancestors;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) {
                         throw ToolError("An edited entity has a hierarchy cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [current](const PositionEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     ancestors.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (PositionEdit& edit : edits) {
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 inverse_parent = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) {
                     const Mat4 parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                     for (const Vec4& column : parent_world.cols) {
                         if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                             throw ToolError("Cannot set world position through a non-finite parent transform");
                         }
                     }
                     if (!parent_world.TryInverse(inverse_parent)) throw ToolError("Cannot set world position through a singular parent transform");
                 }
                 const Vec4 local = inverse_parent * Vec4(edit.world_position.x, edit.world_position.y, edit.world_position.z, 1.0f);
                 if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) || std::abs(local.w) <= 1.0e-12f) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
                 edit.local_position = Vec3(local.x / local.w, local.y / local.w, local.z / local.w);
                 if (!std::isfinite(edit.local_position.x) || !std::isfinite(edit.local_position.y) || !std::isfinite(edit.local_position.z)) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
             }
             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->position = edit.local_position;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_position", Json::array({edit.world_position.x, edit.world_position.y, edit.world_position.z})},
                                    {"local_position", Json::array({edit.local_position.x, edit.local_position.y, edit.local_position.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_positions_shared_many",
         "Set one world position on up to 64 unique live entities while preserving local rotation and scale. Converts through parent transforms and rejects ancestor overlap, cycles, excessive depth and singular parents before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"position", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "position"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["position"].is_array() || args["position"].size() != 3 ||
                 !args["position"][0].is_number() || !args["position"][1].is_number() || !args["position"][2].is_number()) {
                 throw ToolError("\"position\" must be [x, y, z]");
             }
             const Vec3 world_position(args["position"][0].get<f32>(), args["position"][1].get<f32>(), args["position"][2].get<f32>());
             if (!std::isfinite(world_position.x) || !std::isfinite(world_position.y) || !std::isfinite(world_position.z)) {
                 throw ToolError("\"position\" values must be finite");
             }
             struct PositionEdit { Entity entity; Vec3 local_position; };
             std::vector<PositionEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const PositionEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, {}});
             }
             for (const PositionEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("An edited entity has a hierarchy cycle");
                     if (std::find_if(edits.begin(), edits.end(), [current](const PositionEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (PositionEdit& edit : edits) {
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 inverse_parent = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) {
                     const Mat4 parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                     for (const Vec4& column : parent_world.cols) {
                         if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                             throw ToolError("Cannot set world position through a non-finite parent transform");
                         }
                     }
                     if (!parent_world.TryInverse(inverse_parent)) throw ToolError("Cannot set world position through a singular parent transform");
                 }
                 const Vec4 local = inverse_parent * Vec4(world_position.x, world_position.y, world_position.z, 1.0f);
                 if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) || std::abs(local.w) <= 1.0e-12f) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
                 edit.local_position = Vec3(local.x / local.w, local.y / local.w, local.z / local.w);
                 if (!std::isfinite(edit.local_position.x) || !std::isfinite(edit.local_position.y) || !std::isfinite(edit.local_position.z)) {
                     throw ToolError("Cannot calculate a finite local position for " + EntityKey(world, edit.entity));
                 }
             }
             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->position = edit.local_position;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_position", Json::array({world_position.x, world_position.y, world_position.z})},
                                    {"local_position", Json::array({edit.local_position.x, edit.local_position.y, edit.local_position.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_rotations_many",
         "Set world rotations for up to 64 unique live entities while preserving local position and scale. Rotations are normalized and converted through parent orientations; ancestor overlap, invalid quaternions, cycles and excessive depth are rejected.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"rotation", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "rotation"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-rotation edits");
             }
             struct RotationEdit { Entity entity; Quaternion world_rotation; Quaternion local_rotation; };
             std::vector<RotationEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("rotation") || !item["rotation"].is_array() || item["rotation"].size() != 4 ||
                     !item["rotation"][0].is_number() || !item["rotation"][1].is_number() || !item["rotation"][2].is_number() || !item["rotation"][3].is_number()) {
                     throw ToolError("Every \"rotation\" must be [x, y, z, w]");
                 }
                 Quaternion rotation(item["rotation"][0].get<f32>(), item["rotation"][1].get<f32>(),
                                     item["rotation"][2].get<f32>(), item["rotation"][3].get<f32>());
                 if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                     !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) {
                     throw ToolError("\"rotation\" must be a finite non-zero quaternion");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const RotationEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, rotation.Normalized(), Quaternion::Identity()});
             }
             for (const RotationEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> ancestors;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) {
                         throw ToolError("An edited entity has a hierarchy cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [current](const RotationEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     ancestors.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (RotationEdit& edit : edits) {
                 std::vector<Quaternion> parent_locals;
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 for (int depth = 0; !current.IsNull() && depth < kMaxHierarchyDepth; ++depth) {
                     const Transform* transform = world.GetComponent<Transform>(current);
                     if (transform == nullptr) break;
                     const Quaternion& local = transform->rotation;
                     if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) ||
                         !std::isfinite(local.LengthSq()) || local.LengthSq() <= 1.0e-12f) {
                         throw ToolError("An ancestor has an invalid rotation quaternion");
                     }
                     parent_locals.push_back(local.Normalized());
                     current = GetParent(world, game.Guids(), current);
                 }
                 Quaternion parent_world = Quaternion::Identity();
                 for (auto it = parent_locals.rbegin(); it != parent_locals.rend(); ++it) parent_world = (parent_world * *it).Normalized();
                 const Quaternion inverse_parent{-parent_world.x, -parent_world.y, -parent_world.z, parent_world.w};
                 edit.local_rotation = (inverse_parent * edit.world_rotation).Normalized();
                 if (!std::isfinite(edit.local_rotation.x) || !std::isfinite(edit.local_rotation.y) || !std::isfinite(edit.local_rotation.z) ||
                     !std::isfinite(edit.local_rotation.w)) {
                     throw ToolError("World rotation would produce a non-finite local rotation");
                 }
             }
             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->rotation = edit.local_rotation;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_rotation", Json::array({edit.world_rotation.x, edit.world_rotation.y, edit.world_rotation.z, edit.world_rotation.w})},
                                    {"local_rotation", Json::array({edit.local_rotation.x, edit.local_rotation.y, edit.local_rotation.z, edit.local_rotation.w})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_rotations_shared_many",
         "Set one world rotation on up to 64 unique live entities while preserving local position and scale. Normalizes the quaternion and converts through parent orientations; ancestor overlap, invalid hierarchies and excessive depth are rejected before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"rotation", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}, {"items", {{"type", "number"}}}}}},
                {"entities", "rotation"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["rotation"].is_array() || args["rotation"].size() != 4 ||
                 !args["rotation"][0].is_number() || !args["rotation"][1].is_number() ||
                 !args["rotation"][2].is_number() || !args["rotation"][3].is_number()) {
                 throw ToolError("\"rotation\" must be [x, y, z, w]");
             }
             Quaternion desired_world(args["rotation"][0].get<f32>(), args["rotation"][1].get<f32>(),
                                      args["rotation"][2].get<f32>(), args["rotation"][3].get<f32>());
             if (!std::isfinite(desired_world.x) || !std::isfinite(desired_world.y) || !std::isfinite(desired_world.z) || !std::isfinite(desired_world.w) ||
                 !std::isfinite(desired_world.LengthSq()) || desired_world.LengthSq() <= 1.0e-12f) {
                 throw ToolError("\"rotation\" must be a finite non-zero quaternion");
             }
             desired_world = desired_world.Normalized();
             struct RotationEdit { Entity entity; Quaternion local_rotation; };
             std::vector<RotationEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const RotationEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 Entity ancestor = GetParent(world, game.Guids(), entity);
                 std::vector<Entity> seen;
                 std::vector<Quaternion> parent_locals;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == entity || std::find(seen.begin(), seen.end(), ancestor) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const RotationEdit& edit) { return edit.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(ancestor);
                     const Transform* transform = world.GetComponent<Transform>(ancestor);
                     if (transform == nullptr) break;
                     const Quaternion& local = transform->rotation;
                     if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w) ||
                         !std::isfinite(local.LengthSq()) || local.LengthSq() <= 1.0e-12f) throw ToolError("An ancestor has an invalid rotation quaternion");
                     parent_locals.push_back(local.Normalized());
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
                 Quaternion parent_world = Quaternion::Identity();
                 for (auto it = parent_locals.rbegin(); it != parent_locals.rend(); ++it) parent_world = (parent_world * *it).Normalized();
                 const Quaternion inverse_parent{-parent_world.x, -parent_world.y, -parent_world.z, parent_world.w};
                 const Quaternion local_rotation = (inverse_parent * desired_world).Normalized();
                 if (!std::isfinite(local_rotation.x) || !std::isfinite(local_rotation.y) || !std::isfinite(local_rotation.z) || !std::isfinite(local_rotation.w)) {
                     throw ToolError("World rotation would produce a non-finite local rotation");
                 }
                 edits.push_back({entity, local_rotation});
             }
             for (const RotationEdit& edit : edits) {
                 Entity ancestor = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !ancestor.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (ancestor == edit.entity || std::find(seen.begin(), seen.end(), ancestor) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [ancestor](const RotationEdit& other) { return other.entity == ancestor; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(ancestor);
                     ancestor = GetParent(world, game.Guids(), ancestor);
                 }
             }
             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->rotation = edit.local_rotation;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_rotation", Json::array({desired_world.x, desired_world.y, desired_world.z, desired_world.w})},
                                    {"local_rotation", Json::array({edit.local_rotation.x, edit.local_rotation.y, edit.local_rotation.z, edit.local_rotation.w})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_scales_many",
         "Set world-scale magnitudes individually for up to 64 unique live entities while preserving local position and rotation. Converts through parent basis lengths and preserves local mirror signs; invalid hierarchies and collapsed parent axes are rejected.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"scale", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "scale"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-scale edits");
             }
             struct ScaleEdit { Entity entity; Vec3 world_scale; Vec3 local_scale; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("scale") || !item["scale"].is_array() || item["scale"].size() != 3 ||
                     !item["scale"][0].is_number() || !item["scale"][1].is_number() || !item["scale"][2].is_number()) {
                     throw ToolError("Every \"scale\" must be [x, y, z]");
                 }
                 const Vec3 scale(item["scale"][0].get<f32>(), item["scale"][1].get<f32>(), item["scale"][2].get<f32>());
                 if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) || scale.x < 0.0f || scale.y < 0.0f || scale.z < 0.0f) {
                     throw ToolError("\"scale\" must contain finite non-negative magnitudes");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, scale, {}});
             }
             for (const ScaleEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> ancestors;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) {
                         throw ToolError("An edited entity has a hierarchy cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [current](const ScaleEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     if (const Transform* ancestor_transform = world.GetComponent<Transform>(current)) {
                         const Quaternion& rotation = ancestor_transform->rotation;
                         if (!std::isfinite(ancestor_transform->scale.x) || !std::isfinite(ancestor_transform->scale.y) || !std::isfinite(ancestor_transform->scale.z) ||
                             !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                             !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) {
                             throw ToolError("An ancestor has an invalid transform");
                         }
                     }
                     ancestors.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (ScaleEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 if (!std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(transform->rotation.x) || !std::isfinite(transform->rotation.y) || !std::isfinite(transform->rotation.z) ||
                     !std::isfinite(transform->rotation.w) || !std::isfinite(transform->rotation.LengthSq()) || transform->rotation.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, edit.entity) + " has an invalid local transform");
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("Cannot set world scale through a non-finite parent transform");
                     }
                 }
                 Transform unit_transform = *transform;
                 unit_transform.rotation = unit_transform.rotation.Normalized();
                 unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                 const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                 const f32 axis_lengths[3] = {
                     Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                     Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                     Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                 const f32 requested[3] = {edit.world_scale.x, edit.world_scale.y, edit.world_scale.z};
                 const f32 signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f,
                                       transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                       transform->scale.z < 0.0f ? -1.0f : 1.0f};
                 f32 local[3]{};
                 for (int axis = 0; axis < 3; ++axis) {
                     if (!std::isfinite(axis_lengths[axis])) throw ToolError("Parent transform has a non-finite world basis");
                     if (axis_lengths[axis] <= 1.0e-12f) {
                         if (requested[axis] > 0.0f) throw ToolError("Cannot set non-zero world scale on an axis collapsed by the parent transform");
                         local[axis] = 0.0f;
                     } else {
                         local[axis] = signs[axis] * requested[axis] / axis_lengths[axis];
                         if (!std::isfinite(local[axis]) || (requested[axis] > 0.0f && local[axis] == 0.0f)) {
                             throw ToolError("World scale would produce an invalid local scale");
                         }
                     }
                 }
                 edit.local_scale = Vec3(local[0], local[1], local[2]);
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_scale", Json::array({edit.world_scale.x, edit.world_scale.y, edit.world_scale.z})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_set_world_scale_many",
         "Set one shared world-scale magnitude on up to 64 unique live entities while preserving local position and rotation. Converts through parent basis lengths and preserves local mirror signs; invalid hierarchies and collapsed parent axes are rejected.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"scale", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "scale"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["scale"].is_array() || args["scale"].size() != 3 || !args["scale"][0].is_number() ||
                 !args["scale"][1].is_number() || !args["scale"][2].is_number()) {
                 throw ToolError("\"scale\" must be [x, y, z]");
             }
             const Vec3 requested(args["scale"][0].get<f32>(), args["scale"][1].get<f32>(), args["scale"][2].get<f32>());
             if (!std::isfinite(requested.x) || !std::isfinite(requested.y) || !std::isfinite(requested.z) ||
                 requested.x < 0.0f || requested.y < 0.0f || requested.z < 0.0f) {
                 throw ToolError("\"scale\" must contain finite non-negative magnitudes");
             }
             struct ScaleEdit { Entity entity; Vec3 local_scale; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty handle");
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, {}});
             }
             for (const ScaleEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> ancestors;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("An edited entity has a parent chain deeper than the supported limit");
                     if (current == edit.entity || std::find(ancestors.begin(), ancestors.end(), current) != ancestors.end()) {
                         throw ToolError("An edited entity has a hierarchy cycle");
                     }
                     if (std::find_if(edits.begin(), edits.end(), [current](const ScaleEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     if (const Transform* ancestor = world.GetComponent<Transform>(current)) {
                         const Quaternion& rotation = ancestor->rotation;
                         if (!std::isfinite(ancestor->scale.x) || !std::isfinite(ancestor->scale.y) || !std::isfinite(ancestor->scale.z) ||
                             !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                             !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) {
                             throw ToolError("An ancestor has an invalid transform");
                         }
                     }
                     ancestors.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (ScaleEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 const Quaternion& local_rotation = transform->rotation;
                 if (!std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(local_rotation.x) || !std::isfinite(local_rotation.y) || !std::isfinite(local_rotation.z) || !std::isfinite(local_rotation.w) ||
                     !std::isfinite(local_rotation.LengthSq()) || local_rotation.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, edit.entity) + " has an invalid local transform");
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("Cannot set world scale through a non-finite parent transform");
                     }
                 }
                 Transform unit_transform = *transform;
                 unit_transform.rotation = unit_transform.rotation.Normalized();
                 unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                 const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                 const f32 basis[3] = {
                     Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                     Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                     Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                 const f32 target[3] = {requested.x, requested.y, requested.z};
                 const f32 sign[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f,
                                      transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                      transform->scale.z < 0.0f ? -1.0f : 1.0f};
                 f32 local[3]{};
                 for (int axis = 0; axis < 3; ++axis) {
                     if (!std::isfinite(basis[axis])) throw ToolError("Parent transform has a non-finite world basis");
                     if (basis[axis] <= 1.0e-12f) {
                         if (target[axis] > 0.0f) throw ToolError("Cannot set non-zero world scale on an axis collapsed by the parent transform");
                         local[axis] = 0.0f;
                     } else {
                         local[axis] = sign[axis] * target[axis] / basis[axis];
                         if (!std::isfinite(local[axis]) || (target[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale would produce an invalid local scale");
                     }
                 }
                 edit.local_scale = Vec3(local[0], local[1], local[2]);
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"world_scale", Json::array({requested.x, requested.y, requested.z})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_scale_entities_world_shared_many",
         "Multiply world-scale magnitudes by one shared non-negative component factor on up to 64 unique live entities. Preserves local position and rotation and local mirror signs; validates hierarchies, parent bases and results before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}},
                 {"factor", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "factor"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["factor"].is_array() || args["factor"].size() != 3 ||
                 !args["factor"][0].is_number() || !args["factor"][1].is_number() || !args["factor"][2].is_number()) {
                 throw ToolError("\"factor\" must be [x, y, z]");
             }
             const Vec3 factor(args["factor"][0].get<f32>(), args["factor"][1].get<f32>(), args["factor"][2].get<f32>());
             if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z) || factor.x < 0.0f || factor.y < 0.0f || factor.z < 0.0f) {
                 throw ToolError("\"factor\" must contain finite non-negative magnitudes");
             }
             struct ScaleEdit { Entity entity; Vec3 before; Vec3 local_scale; Vec3 after; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"entities\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, {}, {}, {}});
             }
             for (const ScaleEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [current](const ScaleEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (ScaleEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 const Quaternion& rotation = transform->rotation;
                 if (!std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                     !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) {
                     throw ToolError("Entity " + EntityKey(world, edit.entity) + " has an invalid local transform");
                 }
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) {
                         throw ToolError("Cannot scale through a non-finite parent transform");
                     }
                 }
                 Transform unit_transform = *transform;
                 unit_transform.rotation = rotation.Normalized();
                 unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                 const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                 const Mat4 current_world = parent_world * LocalTransformMatrix(*transform);
                 const f32 unit_basis[3] = {
                     Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                     Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                     Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                 const f32 current_basis[3] = {
                     Vec3(current_world.cols[0].x, current_world.cols[0].y, current_world.cols[0].z).Length(),
                     Vec3(current_world.cols[1].x, current_world.cols[1].y, current_world.cols[1].z).Length(),
                     Vec3(current_world.cols[2].x, current_world.cols[2].y, current_world.cols[2].z).Length()};
                 const f32 factors[3] = {factor.x, factor.y, factor.z};
                 const f32 local_signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f,
                                             transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                             transform->scale.z < 0.0f ? -1.0f : 1.0f};
                 f32 target[3]{};
                 f32 local[3]{};
                 for (int axis = 0; axis < 3; ++axis) {
                     if (!std::isfinite(unit_basis[axis]) || !std::isfinite(current_basis[axis])) throw ToolError("Entity has a non-finite world basis");
                     target[axis] = current_basis[axis] * factors[axis];
                     if (!std::isfinite(target[axis])) throw ToolError("World scale multiplication produced a non-finite result");
                     if (unit_basis[axis] <= 1.0e-12f) {
                         if (target[axis] > 0.0f) throw ToolError("Cannot scale a world axis collapsed by the parent transform");
                         local[axis] = 0.0f;
                     } else {
                         local[axis] = local_signs[axis] * target[axis] / unit_basis[axis];
                         if (!std::isfinite(local[axis]) || (target[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale would produce an invalid local scale");
                     }
                 }
                 edit.before = Vec3(current_basis[0], current_basis[1], current_basis[2]);
                 edit.after = Vec3(target[0], target[1], target[2]);
                 edit.local_scale = Vec3(local[0], local[1], local[2]);
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"previous_world_scale", Json::array({edit.before.x, edit.before.y, edit.before.z})},
                                    {"world_scale", Json::array({edit.after.x, edit.after.y, edit.after.z})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_scale_entities_world_many",
         "Multiply world-scale magnitudes using per-entity non-negative component factors on up to 64 unique live entities. Preserves local position and rotation and local mirror signs; validates hierarchies, parent bases and results before applying the batch.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"factor", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "factor"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-scale edits");
             }
             struct ScaleEdit { Entity entity; Vec3 factor; Vec3 before; Vec3 local_scale; Vec3 after; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("factor") || !item["factor"].is_array() || item["factor"].size() != 3 ||
                     !item["factor"][0].is_number() || !item["factor"][1].is_number() || !item["factor"][2].is_number()) {
                     throw ToolError("Every \"factor\" must be [x, y, z]");
                 }
                 const Vec3 factor(item["factor"][0].get<f32>(), item["factor"][1].get<f32>(), item["factor"][2].get<f32>());
                 if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z) || factor.x < 0.0f || factor.y < 0.0f || factor.z < 0.0f) {
                     throw ToolError("\"factor\" must contain finite non-negative magnitudes");
                 }
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, factor, {}, {}, {}});
             }
             for (const ScaleEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [current](const ScaleEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (ScaleEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 const Quaternion& rotation = transform->rotation;
                 if (!std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                     !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) throw ToolError("Entity has an invalid local transform");
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) throw ToolError("Cannot scale through a non-finite parent transform");
                 }
                 Transform unit_transform = *transform;
                 unit_transform.rotation = rotation.Normalized();
                 unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                 const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                 const Mat4 current_world = parent_world * LocalTransformMatrix(*transform);
                 const f32 unit_basis[3] = {
                     Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                     Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                     Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                 const f32 current_basis[3] = {
                     Vec3(current_world.cols[0].x, current_world.cols[0].y, current_world.cols[0].z).Length(),
                     Vec3(current_world.cols[1].x, current_world.cols[1].y, current_world.cols[1].z).Length(),
                     Vec3(current_world.cols[2].x, current_world.cols[2].y, current_world.cols[2].z).Length()};
                 const f32 factors[3] = {edit.factor.x, edit.factor.y, edit.factor.z};
                 const f32 signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f, transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                       transform->scale.z < 0.0f ? -1.0f : 1.0f};
                 f32 target[3]{};
                 f32 local[3]{};
                 for (int axis = 0; axis < 3; ++axis) {
                     if (!std::isfinite(unit_basis[axis]) || !std::isfinite(current_basis[axis])) throw ToolError("Entity has a non-finite world basis");
                     target[axis] = current_basis[axis] * factors[axis];
                     if (!std::isfinite(target[axis])) throw ToolError("World scale multiplication produced a non-finite result");
                     if (unit_basis[axis] <= 1.0e-12f) {
                         if (target[axis] > 0.0f) throw ToolError("Cannot scale a world axis collapsed by the parent transform");
                         local[axis] = 0.0f;
                     } else {
                         local[axis] = signs[axis] * target[axis] / unit_basis[axis];
                         if (!std::isfinite(local[axis]) || (target[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale would produce an invalid local scale");
                     }
                 }
                 edit.before = Vec3(current_basis[0], current_basis[1], current_basis[2]);
                 edit.after = Vec3(target[0], target[1], target[2]);
                 edit.local_scale = Vec3(local[0], local[1], local[2]);
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"previous_world_scale", Json::array({edit.before.x, edit.before.y, edit.before.z})},
                                    {"world_scale", Json::array({edit.after.x, edit.after.y, edit.after.z})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_world_scale_many",
         "Add individual deltas to world-scale magnitudes on up to 64 unique live entities. Preserves local position and rotation and local mirror signs; validates hierarchies, parent bases and non-negative results before applying the batch.",
         Schema({{"edits", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64},
                              {"items", {{"type", "object"},
                                          {"properties", {{"entity", {{"type", "string"}}},
                                                           {"delta", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}}},
                                          {"required", Json::array({"entity", "delta"})}}}}}},
                {"edits"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be an array of 1 to 64 world-scale edits");
             }
             struct ScaleEdit { Entity entity; Vec3 delta; Vec3 before; Vec3 local_scale; Vec3 after; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             for (const Json& item : args["edits"]) {
                 if (!item.is_object() || !item.contains("entity") || !item["entity"].is_string() || item["entity"].get<std::string>().empty()) {
                     throw ToolError("Every \"edits\" entry must contain a non-empty string \"entity\"");
                 }
                 if (!item.contains("delta") || !item["delta"].is_array() || item["delta"].size() != 3 ||
                     !item["delta"][0].is_number() || !item["delta"][1].is_number() || !item["delta"][2].is_number()) {
                     throw ToolError("Every \"delta\" must be [x, y, z]");
                 }
                 const Vec3 delta(item["delta"][0].get<f32>(), item["delta"][1].get<f32>(), item["delta"][2].get<f32>());
                 if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) throw ToolError("\"delta\" values must be finite");
                 const Entity entity = FindEntity(game, item["entity"].get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) {
                     throw ToolError("\"edits\" must not repeat an entity");
                 }
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, delta, {}, {}, {}});
             }
             for (const ScaleEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [current](const ScaleEdit& other) { return other.entity == current; }) != edits.end()) {
                         throw ToolError("\"edits\" must not include both an entity and one of its ancestors");
                     }
                     seen.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (ScaleEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 const Quaternion& rotation = transform->rotation;
                 if (!std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                     !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) throw ToolError("Entity has an invalid local transform");
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) {
                     if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) throw ToolError("Cannot scale through a non-finite parent transform");
                 }
                 Transform unit_transform = *transform;
                 unit_transform.rotation = rotation.Normalized();
                 unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                 const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                 const Mat4 current_world = parent_world * LocalTransformMatrix(*transform);
                 const f32 unit_basis[3] = {
                     Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                     Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                     Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                 const f32 current_basis[3] = {
                     Vec3(current_world.cols[0].x, current_world.cols[0].y, current_world.cols[0].z).Length(),
                     Vec3(current_world.cols[1].x, current_world.cols[1].y, current_world.cols[1].z).Length(),
                     Vec3(current_world.cols[2].x, current_world.cols[2].y, current_world.cols[2].z).Length()};
                 const f32 deltas[3] = {edit.delta.x, edit.delta.y, edit.delta.z};
                 const f32 signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f, transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                       transform->scale.z < 0.0f ? -1.0f : 1.0f};
                 f32 target[3]{};
                 f32 local[3]{};
                 for (int axis = 0; axis < 3; ++axis) {
                     if (!std::isfinite(unit_basis[axis]) || !std::isfinite(current_basis[axis])) throw ToolError("Entity has a non-finite world basis");
                     target[axis] = current_basis[axis] + deltas[axis];
                     if (!std::isfinite(target[axis]) || target[axis] < 0.0f) throw ToolError("World scale offset must produce finite non-negative magnitudes");
                     if (unit_basis[axis] <= 1.0e-12f) {
                         if (target[axis] > 0.0f) throw ToolError("Cannot scale a world axis collapsed by the parent transform");
                         local[axis] = 0.0f;
                     } else {
                         local[axis] = signs[axis] * target[axis] / unit_basis[axis];
                         if (!std::isfinite(local[axis]) || (target[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale offset would produce an invalid local scale");
                     }
                 }
                 edit.before = Vec3(current_basis[0], current_basis[1], current_basis[2]);
                 edit.after = Vec3(target[0], target[1], target[2]);
                 edit.local_scale = Vec3(local[0], local[1], local[2]);
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"previous_world_scale", Json::array({edit.before.x, edit.before.y, edit.before.z})},
                                    {"delta", Json::array({edit.delta.x, edit.delta.y, edit.delta.z})},
                                    {"world_scale", Json::array({edit.after.x, edit.after.y, edit.after.z})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_offset_entities_world_scale_shared_many",
         "Add one shared delta to world-scale magnitudes on up to 64 unique live entities. Preserves local position and rotation and local mirror signs; validates hierarchies, parent bases and non-negative results before applying the batch.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}},
                 {"delta", {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}}}},
                {"entities", "delta"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             if (!args["delta"].is_array() || args["delta"].size() != 3 ||
                 !args["delta"][0].is_number() || !args["delta"][1].is_number() || !args["delta"][2].is_number()) throw ToolError("\"delta\" must be [x, y, z]");
             const Vec3 delta(args["delta"][0].get<f32>(), args["delta"][1].get<f32>(), args["delta"][2].get<f32>());
             if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z)) throw ToolError("\"delta\" values must be finite");
             struct ScaleEdit { Entity entity; Vec3 before; Vec3 local_scale; Vec3 after; };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string() || value.get<std::string>().empty()) throw ToolError("Every \"entities\" entry must be a non-empty string");
                 const Entity entity = FindEntity(game, value.get<std::string>());
                 if (std::find_if(edits.begin(), edits.end(), [entity](const ScaleEdit& edit) { return edit.entity == entity; }) != edits.end()) throw ToolError("\"entities\" must not repeat an entity");
                 if (world.GetComponent<Transform>(entity) == nullptr) throw ToolError("Entity " + EntityKey(world, entity) + " has no Transform");
                 edits.push_back({entity, {}, {}, {}});
             }
             for (const ScaleEdit& edit : edits) {
                 Entity current = GetParent(world, game.Guids(), edit.entity);
                 std::vector<Entity> seen;
                 for (int depth = 0; !current.IsNull(); ++depth) {
                     if (depth >= kMaxHierarchyDepth) throw ToolError("Entity hierarchy exceeds the supported depth");
                     if (current == edit.entity || std::find(seen.begin(), seen.end(), current) != seen.end()) throw ToolError("Entity hierarchy contains a cycle");
                     if (std::find_if(edits.begin(), edits.end(), [current](const ScaleEdit& other) { return other.entity == current; }) != edits.end()) throw ToolError("\"entities\" must not include both an entity and one of its ancestors");
                     seen.push_back(current);
                     current = GetParent(world, game.Guids(), current);
                 }
             }
             for (ScaleEdit& edit : edits) {
                 const Transform* transform = world.GetComponent<Transform>(edit.entity);
                 const Quaternion& rotation = transform->rotation;
                 if (!std::isfinite(transform->scale.x) || !std::isfinite(transform->scale.y) || !std::isfinite(transform->scale.z) ||
                     !std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w) ||
                     !std::isfinite(rotation.LengthSq()) || rotation.LengthSq() <= 1.0e-12f) throw ToolError("Entity has an invalid local transform");
                 const Entity parent = GetParent(world, game.Guids(), edit.entity);
                 Mat4 parent_world = Mat4::Identity();
                 if (!parent.IsNull() && world.GetComponent<Transform>(parent) != nullptr) parent_world = ComputeWorldTransform(world, game.Guids(), parent);
                 for (const Vec4& column : parent_world.cols) if (!std::isfinite(column.x) || !std::isfinite(column.y) || !std::isfinite(column.z) || !std::isfinite(column.w)) throw ToolError("Cannot scale through a non-finite parent transform");
                 Transform unit_transform = *transform;
                 unit_transform.rotation = rotation.Normalized();
                 unit_transform.scale = Vec3(1.0f, 1.0f, 1.0f);
                 const Mat4 unit_world = parent_world * LocalTransformMatrix(unit_transform);
                 const Mat4 current_world = parent_world * LocalTransformMatrix(*transform);
                 const f32 unit_basis[3] = {Vec3(unit_world.cols[0].x, unit_world.cols[0].y, unit_world.cols[0].z).Length(),
                                            Vec3(unit_world.cols[1].x, unit_world.cols[1].y, unit_world.cols[1].z).Length(),
                                            Vec3(unit_world.cols[2].x, unit_world.cols[2].y, unit_world.cols[2].z).Length()};
                 const f32 current_basis[3] = {Vec3(current_world.cols[0].x, current_world.cols[0].y, current_world.cols[0].z).Length(),
                                               Vec3(current_world.cols[1].x, current_world.cols[1].y, current_world.cols[1].z).Length(),
                                               Vec3(current_world.cols[2].x, current_world.cols[2].y, current_world.cols[2].z).Length()};
                 const f32 deltas[3] = {delta.x, delta.y, delta.z};
                 const f32 signs[3] = {transform->scale.x < 0.0f ? -1.0f : 1.0f, transform->scale.y < 0.0f ? -1.0f : 1.0f,
                                       transform->scale.z < 0.0f ? -1.0f : 1.0f};
                 f32 target[3]{};
                 f32 local[3]{};
                 for (int axis = 0; axis < 3; ++axis) {
                     if (!std::isfinite(unit_basis[axis]) || !std::isfinite(current_basis[axis])) throw ToolError("Entity has a non-finite world basis");
                     target[axis] = current_basis[axis] + deltas[axis];
                     if (!std::isfinite(target[axis]) || target[axis] < 0.0f) throw ToolError("World scale offset must produce finite non-negative magnitudes");
                     if (unit_basis[axis] <= 1.0e-12f) {
                         if (target[axis] > 0.0f) throw ToolError("Cannot scale a world axis collapsed by the parent transform");
                         local[axis] = 0.0f;
                     } else {
                         local[axis] = signs[axis] * target[axis] / unit_basis[axis];
                         if (!std::isfinite(local[axis]) || (target[axis] > 0.0f && local[axis] == 0.0f)) throw ToolError("World scale offset would produce an invalid local scale");
                     }
                 }
                 edit.before = Vec3(current_basis[0], current_basis[1], current_basis[2]);
                 edit.after = Vec3(target[0], target[1], target[2]);
                 edit.local_scale = Vec3(local[0], local[1], local[2]);
             }
             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 world.GetComponent<Transform>(edit.entity)->scale = edit.local_scale;
                 results.push_back({{"entity", EntityKey(world, edit.entity)},
                                    {"previous_world_scale", Json::array({edit.before.x, edit.before.y, edit.before.z})},
                                    {"delta", Json::array({delta.x, delta.y, delta.z})},
                                    {"world_scale", Json::array({edit.after.x, edit.after.y, edit.after.z})},
                                    {"local_scale", Json::array({edit.local_scale.x, edit.local_scale.y, edit.local_scale.z})}});
             }
             return {{"updated_count", results.size()}, {"results", results}};
         }});

    server.AddTool(
        {"game_destroy_entities_many",
         "Destroy up to 64 unique live entity roots through the lifecycle (OnDestroy runs for each root and descendant). The complete selection is validated first; overlapping ancestor/descendant roots are rejected.",
         Schema({{"entities", {{"type", "array"}, {"minItems", 1}, {"maxItems", 64}, {"items", {{"type", "string"}}}}}},
                {"entities"}),
         [host](const Json& args) -> Json {
             Game& game = host->Require();
             World& world = game.GetWorld();
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be an array of 1 to 64 entity handles");
             }
             std::vector<Entity> roots;
             roots.reserve(args["entities"].size());
             for (const Json& handle : args["entities"]) {
                 if (!handle.is_string() || handle.get<std::string>().empty()) {
                     throw ToolError("Every \"entities\" entry must be a non-empty entity handle");
                 }
                 const Entity entity = FindEntity(game, handle.get<std::string>());
                 if (std::find(roots.begin(), roots.end(), entity) != roots.end()) {
                     throw ToolError("\"entities\" must not contain the same entity more than once");
                 }
                 roots.push_back(entity);
             }

             std::vector<Entity> affected;
             std::vector<std::string> affected_keys;
             for (Entity root : roots) {
                 std::vector<Entity> subtree;
                 std::vector<Entity> pending{root};
                 while (!pending.empty()) {
                     const Entity current = pending.back();
                     pending.pop_back();
                     if (std::find(subtree.begin(), subtree.end(), current) != subtree.end()) continue;
                     subtree.push_back(current);
                     for (Entity child : ChildrenOf(world, game.Guids(), current)) pending.push_back(child);
                 }
                 for (Entity entity : subtree) {
                     if (std::find(affected.begin(), affected.end(), entity) != affected.end()) {
                         throw ToolError("\"entities\" must not include both an ancestor and its descendant");
                     }
                 }
                 for (Entity entity : subtree) {
                     affected.push_back(entity);
                     affected_keys.push_back(EntityKey(world, entity));
                 }
             }

             for (Entity root : roots) game.GetLifecycle().Destroy(root);
             Json destroyed = Json::array();
             for (const std::string& key : affected_keys) destroyed.push_back(key);
             return {{"destroyed_count", affected.size()}, {"destroyed", destroyed}};
         }});
}

} // namespace aether::mcp
