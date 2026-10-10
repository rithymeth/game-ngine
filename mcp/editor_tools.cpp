#include "editor_tools.h"

#include "aether/reflection/serialize.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/gameplay/gameplay_tag.h"
#if AETHER_MCP_INTERACTION
#include "aether/interaction/interactable.h"
#endif
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"

#include <algorithm>
#include <new>
#include <set>

namespace aether::mcp {

namespace {

// ---------------------------------------------------------------------------
// A component edit as one undoable step: the whole component's bytes before
// and after. Works for every reflected component without per-field plumbing.
// ---------------------------------------------------------------------------
class SetComponentCommand final : public editor::ICommand {
public:
    SetComponentCommand(EntityGuid entity, ComponentId component, std::vector<u8> before, std::vector<u8> after)
        : entity_(entity), component_(component), before_(std::move(before)), after_(std::move(after)) {}

    void Do(editor::CommandContext& ctx) override { Apply(ctx, after_); }
    void Undo(editor::CommandContext& ctx) override { Apply(ctx, before_); }
    std::string Label() const override {
        const ComponentInfo& info = GetComponentInfo(component_);
        return std::string("Edit ") + (info.reflected ? info.reflected->name : info.name);
    }
    usize MemoryBytes() const override { return sizeof(*this) + before_.size() + after_.size(); }

private:
    void Apply(editor::CommandContext& ctx, const std::vector<u8>& bytes) {
        Entity entity = ctx.guids.Find(ctx.world, entity_);
        void* data = entity.IsNull() ? nullptr : ctx.world.GetComponentRaw(entity, component_);
        if (data != nullptr) {
            GetComponentInfo(component_).deserialize(data, bytes.data(), bytes.size());
        }
    }

    EntityGuid entity_;
    ComponentId component_;
    std::vector<u8> before_;
    std::vector<u8> after_;
};

// ---------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------

std::string ComponentName(ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    return info.reflected != nullptr ? info.reflected->name : info.name;
}

std::vector<std::string> KnownComponentNames() {
    std::vector<std::string> names;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (GetComponentInfo(id).reflected != nullptr) {
            names.push_back(ComponentName(id));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

ComponentId RequireComponent(const std::string& name) {
    ComponentId id = FindComponentIdByName(name);
    if (id == kInvalidComponentId || id >= RegisteredComponentCount() || GetComponentInfo(id).reflected == nullptr) {
        std::string known;
        for (const std::string& n : KnownComponentNames()) {
            known += (known.empty() ? "" : ", ") + n;
        }
        throw ToolError("Unknown component \"" + name + "\". Known components: " + known);
    }
    return id;
}

EntityGuid RequireGuid(const Json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw ToolError(std::string("Missing string argument \"") + key + "\" (an entity GUID)");
    }
    EntityGuid guid;
    if (!ParseEntityGuid(args[key].get<std::string>(), guid)) {
        throw ToolError("Not a valid entity GUID: " + args[key].get<std::string>());
    }
    return guid;
}

Entity RequireEntity(EditorSession& s, const Json& args, const char* key = "entity") {
    EntityGuid guid = RequireGuid(args, key);
    Entity e = s.guids.Find(s.world, guid);
    if (e.IsNull()) {
        throw ToolError("No entity with GUID " + ToString(guid));
    }
    return e;
}

std::string RequireString(const Json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string()) {
        throw ToolError(std::string("Missing string argument \"") + key + "\"");
    }
    return args[key].get<std::string>();
}

Json ComponentJson(const EditorSession& s, Entity e, ComponentId id) {
    const ComponentInfo& info = GetComponentInfo(id);
    if (info.reflected == nullptr) {
        return "<unreflected component, " + std::to_string(info.size) + " bytes>";
    }
    return reflect::ToJson(*info.reflected, s.world.GetComponentRaw(e, id));
}

std::vector<Entity> AllEntities(const EditorSession& s) {
    std::vector<Entity> entities;
    s.world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* chunk = archetype.EntityArray(c);
            entities.insert(entities.end(), chunk, chunk + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return entities;
}

std::vector<std::string> ComponentNamesOf(const EditorSession& s, Entity e) {
    std::vector<std::string> names;
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (s.world.HasComponentRaw(e, id)) {
            names.push_back(ComponentName(id));
        }
    }
    return names;
}

Json EntityJson(const EditorSession& s, Entity e) {
    Json components = Json::object();
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        if (s.world.HasComponentRaw(e, id)) {
            components[ComponentName(id)] = ComponentJson(s, e, id);
        }
    }
    const IdComponent* idc = s.world.GetComponent<IdComponent>(e);
    return {{"entity", idc ? ToString(idc->guid) : ""}, {"components", components}};
}

const char* PlayStateName(editor::PlaySession::State state) {
    switch (state) {
    case editor::PlaySession::State::Editing: return "editing";
    case editor::PlaySession::State::Playing: return "playing";
    case editor::PlaySession::State::Paused: return "paused";
    }
    return "?";
}

// Merges `patch` (a partial JSON object of the component's fields) over the
// component's current value and returns the resulting serialized bytes.
// `warnings` collects loader complaints (wrong types, unknown fields).
std::vector<u8> PatchedBytes(const EditorSession& s, Entity e, ComponentId id, const Json& patch,
                             std::vector<u8>& before, Json& warnings) {
    const ComponentInfo& info = GetComponentInfo(id);
    const void* current = s.world.GetComponentRaw(e, id);
    before.clear();
    info.serialize(current, before);

    Json merged = reflect::ToJson(*info.reflected, current);
    merged.merge_patch(patch);

    // Build the new value in scratch storage, starting from a copy of the old.
    struct Scratch {
        const ComponentInfo& info;
        void* ptr;
        explicit Scratch(const ComponentInfo& i)
            : info(i), ptr(::operator new(i.size, std::align_val_t(i.alignment))) {
            info.construct(ptr);
        }
        ~Scratch() {
            info.destruct(ptr);
            ::operator delete(ptr, std::align_val_t(info.alignment));
        }
    } scratch(info);
    info.deserialize(scratch.ptr, before.data(), before.size());

    reflect::LoadReport report;
    if (!reflect::FromJson(*info.reflected, scratch.ptr, merged, &report)) {
        throw ToolError("Value does not match the shape of " + ComponentName(id));
    }
    for (const std::string& w : report.warnings) {
        warnings.push_back(w);
    }
    std::vector<u8> after;
    info.serialize(scratch.ptr, after);
    return after;
}

Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) {
        schema["required"] = required;
    }
    return schema;
}

const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID, as returned by list_entities"}};

} // namespace

void RegisterAttributeTools(McpServer& server, EditorSession& s);
void RegisterAudioEditorTools(McpServer& server, EditorSession& s);
#if AETHER_MCP_INTERACTION
void RegisterInteractionEditorTools(McpServer& server, EditorSession& s);
#endif

void RegisterEditorTools(McpServer& server, EditorSession& s) {
    server.AddTool({"editor_state",
                    "Summary of the editor session: entity count, undo/redo availability, unsaved changes, play state.",
                    Schema(Json::object()), [&s](const Json&) -> Json {
                        return {{"entities", s.world.EntityCount()},
                                {"can_undo", s.stack.CanUndo()},
                                {"undo_label", s.stack.UndoLabel()},
                                {"can_redo", s.stack.CanRedo()},
                                {"redo_label", s.stack.RedoLabel()},
                                {"dirty", s.stack.IsDirty()},
                                {"play_state", PlayStateName(s.play.GetState())}};
                    }});

    server.AddTool({"list_component_types",
                    "Component types that can be added to entities, with their fields. Use these names in the other tools.",
                    Schema(Json::object()), [](const Json&) -> Json {
                        Json out = Json::array();
                        for (const std::string& name : KnownComponentNames()) {
                            const ComponentInfo& info = GetComponentInfo(FindComponentIdByName(name));
                            Json fields = Json::array();
                            for (const reflect::FieldInfo& f : info.reflected->fields) {
                                fields.push_back({{"name", f.name},
                                                  {"type", f.type ? f.type->name : "?"},
                                                  {"read_only", f.HasFlag(reflect::Field_ReadOnly)}});
                            }
                            out.push_back({{"name", name}, {"fields", fields}});
                        }
                        return out;
                    }});

    server.AddTool({"list_entities",
                    "List entities in the scene (GUID and component names). Optionally only those with a given component.",
                    Schema({{"component", {{"type", "string"}, {"description", "Only entities having this component"}}}}),
                    [&s](const Json& args) -> Json {
                        editor::EnsureAllGuids(s.world, s.guids);
                        ComponentId filter = kInvalidComponentId;
                        if (args.contains("component")) {
                            filter = RequireComponent(args["component"].get<std::string>());
                        }
                        Json out = Json::array();
                        for (Entity e : AllEntities(s)) {
                            if (filter != kInvalidComponentId && !s.world.HasComponentRaw(e, filter)) {
                                continue;
                            }
                            const IdComponent* id = s.world.GetComponent<IdComponent>(e);
                            out.push_back({{"entity", id ? ToString(id->guid) : ""},
                                           {"components", ComponentNamesOf(s, e)}});
                        }
                        return {{"count", out.size()}, {"entities", out}};
                    }});

    server.AddTool({"get_entity", "Every component of one entity, as JSON.", Schema({{"entity", kGuidProp}}, {"entity"}),
                    [&s](const Json& args) -> Json { return EntityJson(s, RequireEntity(s, args)); }});

    server.AddTool(
        {"create_entity",
         "Create an entity. `components` maps component names to (partial) field values, e.g. "
         "{\"Transform\": {\"position\": [1, 2, 3]}}. Optionally parent it to another entity. Undoable.",
         Schema({{"components", {{"type", "object"}, {"description", "Component name -> field values"}}},
                 {"parent", kGuidProp}}),
         [&s](const Json& args) -> Json {
             // Validate everything before touching the world, so a bad request creates nothing.
             std::vector<std::pair<ComponentId, Json>> wanted;
             if (args.contains("components")) {
                 if (!args["components"].is_object()) {
                     throw ToolError("\"components\" must be an object");
                 }
                 for (auto& [name, value] : args["components"].items()) {
                     wanted.emplace_back(RequireComponent(name), value);
                 }
             }
             EntityGuid parent;
             if (args.contains("parent") && !args["parent"].is_null()) {
                 parent = RequireGuid(args, "parent");
                 RequireEntity(s, args, "parent");
             }

             EntityGuid guid = NewEntityGuid();
             Entity e = s.world.CreateEntity(IdComponent{guid});
             s.guids.Add(guid, e);
             Json warnings = Json::array();
             for (auto& [id, value] : wanted) {
                 if (id == GetComponentId<IdComponent>()) {
                     continue; // identity is assigned, not chosen
                 }
                 s.world.AddComponentRaw(e, id);
                 reflect::LoadReport report;
                 const ComponentInfo& info = GetComponentInfo(id);
                 if (!reflect::FromJson(*info.reflected, s.world.GetComponentRaw(e, id), value, &report)) {
                     s.guids.Remove(guid);
                     s.world.DestroyEntity(e);
                     throw ToolError("Value does not match the shape of " + ComponentName(id));
                 }
                 for (const std::string& w : report.warnings) {
                     warnings.push_back(w);
                 }
             }
             if (!parent.IsNull()) {
                 s.world.AddComponent(e, Parent{parent});
             }
             editor::CommandContext ctx = s.Context();
             s.stack.Record(editor::CreateEntityCommand::FromExisting(ctx, e));
             Json out = EntityJson(s, e);
             if (!warnings.empty()) {
                 out["warnings"] = warnings;
             }
             return out;
         }});

    server.AddTool({"destroy_entity", "Delete an entity. Undoable.", Schema({{"entity", kGuidProp}}, {"entity"}),
                    [&s](const Json& args) -> Json {
                        EntityGuid guid = RequireGuid(args, "entity");
                        RequireEntity(s, args);
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::DestroyEntityCommand>(guid));
                        return {{"destroyed", ToString(guid)}};
                    }});

    server.AddTool({"add_component",
                    "Add a component to an entity (default values, or the given partial field values). Undoable.",
                    Schema({{"entity", kGuidProp},
                            {"component", {{"type", "string"}}},
                            {"value", {{"type", "object"}, {"description", "Optional initial field values"}}}},
                           {"entity", "component"}),
                    [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        ComponentId id = RequireComponent(RequireString(args, "component"));
                        if (s.world.HasComponentRaw(e, id)) {
                            throw ToolError("Entity already has " + ComponentName(id) + "; use set_component");
                        }
                        editor::CommandContext ctx = s.Context();
                        s.stack.BeginTransaction("Add " + ComponentName(id));
                        s.stack.Execute(ctx, std::make_unique<editor::AddComponentCommand>(guid, id));
                        Json warnings = Json::array();
                        if (args.contains("value") && args["value"].is_object() && !args["value"].empty()) {
                            std::vector<u8> before;
                            std::vector<u8> after;
                            try {
                                after = PatchedBytes(s, e, id, args["value"], before, warnings);
                            } catch (...) {
                                s.stack.EndTransaction();
                                s.stack.Undo(ctx);
                                throw;
                            }
                            s.stack.Execute(ctx, std::make_unique<SetComponentCommand>(guid, id, before, after));
                        }
                        s.stack.EndTransaction();
                        Json out = {{"component", ComponentName(id)}, {"value", ComponentJson(s, e, id)}};
                        if (!warnings.empty()) {
                            out["warnings"] = warnings;
                        }
                        return out;
                    }});

    server.AddTool({"remove_component", "Remove a component from an entity. Undoable.",
                    Schema({{"entity", kGuidProp}, {"component", {{"type", "string"}}}}, {"entity", "component"}),
                    [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        ComponentId id = RequireComponent(RequireString(args, "component"));
                        if (id == GetComponentId<IdComponent>()) {
                            throw ToolError("An entity's identity can't be removed");
                        }
                        if (!s.world.HasComponentRaw(e, id)) {
                            throw ToolError("Entity doesn't have " + ComponentName(id));
                        }
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::RemoveComponentCommand>(guid, id));
                        return {{"removed", ComponentName(id)}};
                    }});

    server.AddTool({"set_component",
                    "Change fields of a component the entity already has. `value` is a partial object: only the "
                    "fields it names change. Undoable.",
                    Schema({{"entity", kGuidProp},
                            {"component", {{"type", "string"}}},
                            {"value", {{"type", "object"}, {"description", "Field name -> new value"}}}},
                           {"entity", "component", "value"}),
                    [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        ComponentId id = RequireComponent(RequireString(args, "component"));
                        if (id == GetComponentId<IdComponent>()) {
                            throw ToolError("An entity's identity is read-only");
                        }
                        if (!s.world.HasComponentRaw(e, id)) {
                            throw ToolError("Entity doesn't have " + ComponentName(id) + "; use add_component");
                        }
                        if (!args.contains("value") || !args["value"].is_object()) {
                            throw ToolError("\"value\" must be an object of field values");
                        }
                        Json warnings = Json::array();
                        std::vector<u8> before;
                        std::vector<u8> after = PatchedBytes(s, e, id, args["value"], before, warnings);
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<SetComponentCommand>(guid, id, before, after));
                        Json out = {{"component", ComponentName(id)}, {"value", ComponentJson(s, e, id)}};
                        if (!warnings.empty()) {
                            out["warnings"] = warnings;
                        }
                        return out;
                    }});

    server.AddTool({"reparent",
                    "Make an entity a child of another (its Transform is then relative to the parent), or detach it "
                    "by omitting `parent`. Undoable.",
                    Schema({{"entity", kGuidProp}, {"parent", kGuidProp}}, {"entity"}), [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        EntityGuid parent;
                        if (args.contains("parent") && !args["parent"].is_null()) {
                            Entity p = RequireEntity(s, args, "parent");
                            if (WouldCreateCycle(s.world, s.guids, e, p)) {
                                throw ToolError("That would make the entity its own ancestor");
                            }
                            parent = RequireGuid(args, "parent");
                        }
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::ReparentCommand>(guid, parent));
                        return {{"entity", ToString(guid)}, {"parent", parent.IsNull() ? Json(nullptr) : Json(ToString(parent))}};
                    }});

    server.AddTool({"undo", "Undo the last editor action.", Schema(Json::object()), [&s](const Json&) -> Json {
                        std::string label = s.stack.UndoLabel();
                        editor::CommandContext ctx = s.Context();
                        if (!s.stack.Undo(ctx)) {
                            throw ToolError("Nothing to undo");
                        }
                        return {{"undone", label}};
                    }});
    server.AddTool({"redo", "Redo the last undone editor action.", Schema(Json::object()), [&s](const Json&) -> Json {
                        std::string label = s.stack.RedoLabel();
                        editor::CommandContext ctx = s.Context();
                        if (!s.stack.Redo(ctx)) {
                            throw ToolError("Nothing to redo");
                        }
                        return {{"redone", label}};
                    }});

    server.AddTool({"save_scene", "Save the scene to a file: JSON (.json, human-readable) or binary (.aesc).",
                    Schema({{"path", {{"type", "string"}}}}, {"path"}), [&s](const Json& args) -> Json {
                        std::string path = RequireString(args, "path");
                        bool binary = path.size() >= 5 && path.compare(path.size() - 5, 5, ".aesc") == 0;
                        bool ok = binary ? SaveScene(s.world, path) : SaveSceneJson(s.world, path);
                        if (!ok) {
                            throw ToolError("Could not write " + path);
                        }
                        s.stack.MarkSaved();
                        return {{"saved", path}, {"entities", s.world.EntityCount()}};
                    }});

    server.AddTool({"load_scene",
                    "Load a scene file (.json or .aesc). By default it is added to the current scene; with "
                    "replace=true the current scene and undo history are cleared first.",
                    Schema({{"path", {{"type", "string"}}}, {"replace", {{"type", "boolean"}}}}, {"path"}),
                    [&s](const Json& args) -> Json {
                        if (!s.play.IsEditing()) {
                            throw ToolError("Stop play mode before loading a scene");
                        }
                        std::string path = RequireString(args, "path");
                        if (args.value("replace", false)) {
                            for (Entity e : AllEntities(s)) {
                                s.world.DestroyEntity(e);
                            }
                            s.guids.Clear();
                            s.stack.Clear();
                        }
                        bool binary = path.size() >= 5 && path.compare(path.size() - 5, 5, ".aesc") == 0;
                        bool ok = binary ? LoadScene(s.world, path) : LoadSceneJson(s.world, path);
                        if (!ok) {
                            throw ToolError("Could not load " + path);
                        }
                        editor::EnsureAllGuids(s.world, s.guids);
                        s.stack.MarkSaved();
                        return {{"loaded", path}, {"entities", s.world.EntityCount()}};
                    }});

    server.AddTool({"play",
                    "Enter play mode (or resume from pause). The scene is snapshotted and restored exactly on stop. "
                    "Starts the simulation systems (see list_systems); advance them with step_simulation.",
                    Schema(Json::object()), [&s](const Json&) -> Json {
                        editor::CommandContext ctx = s.Context();
                        s.play.Play(ctx, s.stack);
                        if (!s.play.IsEditing() && !s.sim) {
                            s.sim = std::make_unique<Simulation>(s.world);
                        }
                        return {{"play_state", PlayStateName(s.play.GetState())}};
                    }});
    server.AddTool({"pause", "Pause play mode.", Schema(Json::object()), [&s](const Json&) -> Json {
                        s.play.Pause();
                        return {{"play_state", PlayStateName(s.play.GetState())}};
                    }});
    server.AddTool({"stop", "Leave play mode, restoring the scene as it was before play.", Schema(Json::object()),
                    [&s](const Json&) -> Json {
                        s.sim.reset(); // its physics bodies go before the world is rebuilt
                        editor::CommandContext ctx = s.Context();
                        s.play.Stop(ctx, s.stack);
                        editor::EnsureAllGuids(s.world, s.guids);
                        return {{"play_state", PlayStateName(s.play.GetState())}};
                    }});

    // -- Systems ------------------------------------------------------------
    server.AddTool({"list_systems",
                    "The simulation systems in execution order (phase by phase), with whether each is enabled. "
                    "Available in play mode.",
                    Schema(Json::object()), [&s](const Json&) -> Json {
                        if (!s.sim) throw ToolError("No simulation: call play first");
                        Json out = Json::array();
                        for (const Simulation::SystemInfo& info : s.sim->Systems()) {
                            out.push_back({{"name", info.name}, {"phase", SystemPhaseName(info.phase)}, {"enabled", info.enabled}});
                        }
                        return out;
                    }});
    server.AddTool({"set_system_enabled", "Turn one simulation system on or off.",
                    Schema({{"system", {{"type", "string"}, {"description", "A name from list_systems"}}},
                            {"enabled", {{"type", "boolean"}}}},
                           {"system", "enabled"}),
                    [&s](const Json& args) -> Json {
                        if (!s.sim) throw ToolError("No simulation: call play first");
                        if (!args.contains("enabled") || !args["enabled"].is_boolean()) {
                            throw ToolError("Missing boolean argument \"enabled\"");
                        }
                        const std::string name = RequireString(args, "system");
                        if (!s.sim->SetEnabled(name, args["enabled"].get<bool>())) {
                            throw ToolError("No system named \"" + name + "\"");
                        }
                        return {{"system", name}, {"enabled", args["enabled"].get<bool>()}};
                    }});
    server.AddTool({"step_simulation",
                    "Advance the running simulation by a number of frames (default 1) of dt seconds (default 1/60), "
                    "running every enabled system. Works in play mode and while paused.",
                    Schema({{"frames", {{"type", "integer"}, {"description", "Frames to run, 1-10000 (default 1)"}}},
                            {"dt", {{"type", "number"}, {"description", "Seconds per frame (default 1/60)"}}}}),
                    [&s](const Json& args) -> Json {
                        if (!s.sim) throw ToolError("No simulation: call play first");
                        i64 frames = 1;
                        if (args.contains("frames")) {
                            if (!args["frames"].is_number_integer()) throw ToolError("\"frames\" must be an integer");
                            frames = args["frames"].get<i64>();
                        }
                        if (frames < 1 || frames > 10000) throw ToolError("\"frames\" must be between 1 and 10000");
                        f64 dt = 1.0 / 60.0;
                        if (args.contains("dt")) {
                            if (!args["dt"].is_number()) throw ToolError("\"dt\" must be a number");
                            dt = args["dt"].get<f64>();
                        }
                        if (!(dt > 0.0 && dt <= 1.0)) throw ToolError("\"dt\" must be in (0, 1] seconds");
                        FrameContext last;
                        for (i64 i = 0; i < frames; ++i) {
                            last = s.sim->Step(static_cast<f32>(dt));
                        }
                        return {{"frames_run", frames},
                                {"frame", last.frame},
                                {"fixed_steps", last.fixed_step},
                                {"time", last.time},
                                {"entities", s.world.EntityCount()}};
                    }});
    server.AddTool({"set_platformer_input",
                    "Set the input fed to 2D platformer controllers every fixed step until changed.",
                    Schema({{"move", {{"type", "number"}, {"description", "-1 (left) to 1 (right)"}}},
                            {"jump", {{"type", "boolean"}}}}),
                    [&s](const Json& args) -> Json {
                        if (!s.sim) throw ToolError("No simulation: call play first");
                        const f32 move = args.contains("move") && args["move"].is_number() ? std::clamp(args["move"].get<f32>(), -1.0f, 1.0f) : 0.0f;
                        const bool jump = args.contains("jump") && args["jump"].is_boolean() && args["jump"].get<bool>();
                        s.sim->SetPlatformerInput(move, jump);
                        return {{"move", move}, {"jump", jump}};
                    }});
    RegisterAttributeTools(server, s);
    RegisterAudioEditorTools(server, s);
#if AETHER_MCP_INTERACTION
    RegisterInteractionEditorTools(server, s);
#endif
}


// ---------------------------------------------------------------------------
// Attributes. There is no attribute-definition file: an entity's stats (Health, Mana, ...) are the
// AttributeSet component's data, saved in scenes and prefabs. These tools edit that component as one
// undoable step, keeping its attributes sorted and clamped the way the game does (AttributeSet::Define).
// ---------------------------------------------------------------------------

namespace {

Json AttributeJson(const gas::Attribute& a) {
    return {{"name", a.name}, {"base", a.base}, {"current", a.current}, {"min", a.min}, {"max", a.max}};
}

Json AttributesJson(const gas::AttributeSet& set) {
    Json out = Json::array();
    for (const gas::Attribute& a : set.attributes) out.push_back(AttributeJson(a));
    return out;
}

// Replaces the entity's AttributeSet with `next` as one undoable step (adding the component first if the entity has none).
void CommitAttributeSet(EditorSession& s, Entity e, EntityGuid guid, const gas::AttributeSet& next, const std::string& label) {
    const ComponentId id = GetComponentId<gas::AttributeSet>();
    editor::CommandContext ctx = s.Context();
    s.stack.BeginTransaction(label);
    if (!s.world.HasComponentRaw(e, id)) {
        s.stack.Execute(ctx, std::make_unique<editor::AddComponentCommand>(guid, id));
    }
    Json patch = {{"attributes", AttributesJson(next)}};
    Json warnings = Json::array();
    std::vector<u8> before;
    std::vector<u8> after;
    try {
        after = PatchedBytes(s, e, id, patch, before, warnings);
    } catch (...) {
        s.stack.EndTransaction();
        s.stack.Undo(ctx);
        throw;
    }
    s.stack.Execute(ctx, std::make_unique<SetComponentCommand>(guid, id, before, after));
    s.stack.EndTransaction();
}

} // namespace

void RegisterAttributeTools(McpServer& server, EditorSession& s) {
    const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID"}};

    server.AddTool({"attribute_list", "The entity's attributes (name, base, current, min, max). Empty if it has no AttributeSet.",
                    Schema({{"entity", kGuidProp}}, {"entity"}), [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        const gas::AttributeSet* set = s.world.GetComponent<gas::AttributeSet>(e);
                        return {{"entity", RequireString(args, "entity")}, {"attributes", set ? AttributesJson(*set) : Json::array()}};
                    }});

    server.AddTool(
        {"attribute_define",
         "Create or redefine attributes on an entity, adding an AttributeSet if it has none. One undoable step for the whole list. "
         "An existing attribute gets the new base and bounds (the base is clamped into them; min above max is read as the range "
         "[max, min]). Omitted min/max mean unbounded.",
         Schema({{"entity", kGuidProp},
                 {"attributes", {{"type", "array"},
                                 {"description", "[{name, base, min?, max?}, ...]"},
                                 {"items", {{"type", "object"}}}}}},
                {"entity", "attributes"}),
         [&s](const Json& args) -> Json {
             Entity e = RequireEntity(s, args);
             EntityGuid guid = RequireGuid(args, "entity");
             if (!args["attributes"].is_array() || args["attributes"].empty()) throw ToolError("\"attributes\" must be a non-empty array");
             gas::AttributeSet next;
             if (const gas::AttributeSet* current = s.world.GetComponent<gas::AttributeSet>(e)) next = *current;
             Json defined = Json::array();
             for (const Json& a : args["attributes"]) {
                 if (!a.is_object() || !a.contains("name") || !a["name"].is_string()) throw ToolError("Each attribute needs a string \"name\"");
                 const auto number = [&](const char* key, f32 fallback) {
                     if (!a.contains(key)) return fallback;
                     if (!a[key].is_number()) throw ToolError(std::string("\"") + key + "\" of " + a["name"].get<std::string>() + " must be a number");
                     return a[key].get<f32>();
                 };
                 const std::string name = a["name"].get<std::string>();
                 const f32 base = number("base", 0.0f);
                 if (!next.Define(name, base, number("min", -3.0e38f), number("max", 3.0e38f))) {
                     throw ToolError("Could not define \"" + name + "\": a bad name or a NaN");
                 }
                 defined.push_back(name);
             }
             CommitAttributeSet(s, e, guid, next, "Define attributes");
             const gas::AttributeSet* now = s.world.GetComponent<gas::AttributeSet>(e);
             return {{"defined", defined}, {"attributes", now ? AttributesJson(*now) : Json::array()}};
         }});

    server.AddTool({"attribute_remove", "Remove attributes from an entity by name. Undoable. Names it doesn't have are an error.",
                    Schema({{"entity", kGuidProp}, {"names", {{"type", "array"}, {"items", {{"type", "string"}}}}}}, {"entity", "names"}),
                    [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        const gas::AttributeSet* current = s.world.GetComponent<gas::AttributeSet>(e);
                        if (current == nullptr) throw ToolError("Entity has no AttributeSet");
                        if (!args["names"].is_array() || args["names"].empty()) throw ToolError("\"names\" must be a non-empty array of strings");
                        gas::AttributeSet next = *current;
                        for (const Json& n : args["names"]) {
                            if (!n.is_string()) throw ToolError("\"names\" must be an array of strings");
                            const std::string name = n.get<std::string>();
                            const auto it = std::find_if(next.attributes.begin(), next.attributes.end(), [&](const gas::Attribute& a) { return a.name == name; });
                            if (it == next.attributes.end()) throw ToolError("No attribute \"" + name + "\" on this entity");
                            next.attributes.erase(it);
                        }
                        CommitAttributeSet(s, e, guid, next, "Remove attributes");
                        return {{"attributes", AttributesJson(*s.world.GetComponent<gas::AttributeSet>(e))}};
                    }});
}


#if AETHER_MCP_INTERACTION
// ---------------------------------------------------------------------------
// Interaction. There is no interaction asset: what can be used is the Interactable component on an
// entity (a door, a lever, a chest, a person to talk to), saved in scenes and prefabs. These tools edit it
// as one undoable step with the checks the game cannot make for a hand-edited value.
// ---------------------------------------------------------------------------

namespace {

// Throws a ToolError naming the first thing wrong with a partial Interactable.
void ValidateInteractableFields(const Json& fields) {
    static const std::set<std::string> kKnown = {"enabled", "prompt", "prompt_key", "range", "required_tags", "blocked_tags", "effect", "ability", "one_shot", "cooldown"};
    for (auto& [key, value] : fields.items()) {
        if (key == "used" || key == "remaining") {
            throw ToolError("\"" + key + "\" is runtime state (a spent or cooling interactable); use game_reset_interactable in a running game");
        }
        if (kKnown.count(key) == 0) throw ToolError("Unknown Interactable field \"" + key + "\"");
        const bool is_bool = key == "enabled" || key == "one_shot";
        const bool is_number = key == "range" || key == "cooldown";
        const bool is_list = key == "required_tags" || key == "blocked_tags";
        if (is_bool && !value.is_boolean()) throw ToolError("\"" + key + "\" must be true or false");
        if (is_number) {
            if (!value.is_number() || !(value.get<double>() >= 0.0)) throw ToolError("\"" + key + "\" must be a number, 0 or more");
        }
        if (is_list) {
            if (!value.is_array()) throw ToolError("\"" + key + "\" must be an array of tag names");
            for (const Json& tag : value) {
                if (!tag.is_string() || !gas::GameplayTag::ValidName(tag.get<std::string>())) {
                    throw ToolError("\"" + key + "\" has a bad tag name (tags are dotted words such as State.Stunned)");
                }
            }
        }
        if (!is_bool && !is_number && !is_list && !value.is_string()) throw ToolError("\"" + key + "\" must be a string");
    }
}

} // namespace

void RegisterInteractionEditorTools(McpServer& server, EditorSession& s) {
    const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID"}};

    server.AddTool({"interactable_list", "Entities in the editor scene that have an Interactable, with its settings.", Schema(Json::object()),
                    [&s](const Json&) -> Json {
                        const ComponentId id = GetComponentId<interact::Interactable>();
                        Json out = Json::array();
                        for (Entity e : AllEntities(s)) {
                            if (!s.world.HasComponentRaw(e, id)) continue;
                            const IdComponent* idc = s.world.GetComponent<IdComponent>(e);
                            out.push_back({{"entity", idc ? ToString(idc->guid) : ""}, {"interactable", ComponentJson(s, e, id)}});
                        }
                        return out;
                    }});

    server.AddTool(
        {"interactable_set",
         "Make an entity something the player can use, or change how: adds an Interactable if it has none and applies the given fields "
         "(a partial update). One undoable step. Fields: enabled, prompt, prompt_key (localization), range (0 = no limit), required_tags "
         "and blocked_tags (dotted tag names the user must / must not have), effect (an effect applied to the user), ability (an ability "
         "the user activates), one_shot, cooldown (seconds). What using it does is its script's or Blueprint's OnInteract.",
         Schema({{"entity", kGuidProp},
                 {"fields", {{"type", "object"}, {"description", "e.g. {\"prompt\": \"Open\", \"range\": 2.5, \"one_shot\": true}"}}}},
                {"entity", "fields"}),
         [&s](const Json& args) -> Json {
             Entity e = RequireEntity(s, args);
             EntityGuid guid = RequireGuid(args, "entity");
             if (!args["fields"].is_object()) throw ToolError("\"fields\" must be an object");
             ValidateInteractableFields(args["fields"]);
             const ComponentId id = GetComponentId<interact::Interactable>();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction("Set interactable");
             if (!s.world.HasComponentRaw(e, id)) s.stack.Execute(ctx, std::make_unique<editor::AddComponentCommand>(guid, id));
             Json warnings = Json::array();
             std::vector<u8> before;
             std::vector<u8> after;
             try {
                 after = PatchedBytes(s, e, id, args["fields"], before, warnings);
             } catch (...) {
                 s.stack.EndTransaction();
                 s.stack.Undo(ctx);
                 throw;
             }
             s.stack.Execute(ctx, std::make_unique<SetComponentCommand>(guid, id, before, after));
             s.stack.EndTransaction();
             Json out = {{"interactable", ComponentJson(s, e, id)}};
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool({"interactable_remove", "Make an entity no longer usable (removes its Interactable). Undoable.",
                    Schema({{"entity", kGuidProp}}, {"entity"}), [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        const ComponentId id = GetComponentId<interact::Interactable>();
                        if (!s.world.HasComponentRaw(e, id)) throw ToolError("Entity has no Interactable");
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::RemoveComponentCommand>(guid, id));
                        return {{"removed", true}};
                    }});
}
#endif // AETHER_MCP_INTERACTION


// ---------------------------------------------------------------------------
// Audio components. AudioSource (plays a cue from an entity), AudioListener (where the player hears from) and
// ReverbZone (a space with its own reverb) are components saved in scenes and prefabs; audio_set edits them as one
// validated, undoable step.
// ---------------------------------------------------------------------------

namespace {

bool IsAudioComponent(const std::string& name) { return name == "AudioSource" || name == "AudioListener" || name == "ReverbZone"; }

// Throws a ToolError naming the first thing wrong with a partial audio component.
void ValidateAudioFields(const std::string& component, const Json& fields) {
    auto number = [&](const std::string& key, const Json& v, double lo, double hi) {
        if (!v.is_number() || !(v.get<double>() >= lo) || !(v.get<double>() <= hi)) {
            throw ToolError("\"" + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
        }
    };
    for (auto& [key, value] : fields.items()) {
        if (component == "AudioSource") {
            if (key == "playing" || key == "commands") throw ToolError("\"" + key + "\" is runtime state; use game_audio_source in a running game");
            if (key == "cue") { if (!value.is_string()) throw ToolError("\"cue\" must be a cue asset path (a string)"); }
            else if (key == "auto_play") { if (!value.is_boolean()) throw ToolError("\"auto_play\" must be true or false"); }
            else if (key == "volume_db") number(key, value, -120.0, 24.0);
            else if (key == "pitch") { number(key, value, 0.0, 8.0); if (!(value.get<double>() > 0.0)) throw ToolError("\"pitch\" must be above 0"); }
            else throw ToolError("Unknown AudioSource field \"" + key + "\" (cue, auto_play, volume_db, pitch)");
        } else if (component == "AudioListener") {
            if (key != "active") throw ToolError("Unknown AudioListener field \"" + key + "\" (active)");
            if (!value.is_boolean()) throw ToolError("\"active\" must be true or false");
        } else {
            if (key == "radius" || key == "blend_distance") number(key, value, 0.0, 100000.0);
            else if (key == "priority") { if (!value.is_number_integer()) throw ToolError("\"priority\" must be an integer"); }
            else if (key == "room_size" || key == "damping" || key == "wet") number(key, value, 0.0, 1.0);
            else throw ToolError("Unknown ReverbZone field \"" + key + "\" (radius, blend_distance, priority, room_size, damping, wet)");
        }
    }
}

} // namespace

void RegisterAudioEditorTools(McpServer& server, EditorSession& s) {
    const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID"}};

    server.AddTool({"audio_list", "Entities in the editor scene with an AudioSource, AudioListener or ReverbZone, and their settings.", Schema(Json::object()),
                    [&s](const Json&) -> Json {
                        Json out = Json::array();
                        for (Entity e : AllEntities(s)) {
                            Json row = Json::object();
                            for (const char* name : {"AudioSource", "AudioListener", "ReverbZone"}) {
                                const ComponentId id = FindComponentIdByName(name);
                                if (id != kInvalidComponentId && s.world.HasComponentRaw(e, id)) row[name] = ComponentJson(s, e, id);
                            }
                            if (row.empty()) continue;
                            const IdComponent* idc = s.world.GetComponent<IdComponent>(e);
                            row["entity"] = idc ? ToString(idc->guid) : "";
                            out.push_back(row);
                        }
                        return out;
                    }});

    server.AddTool(
        {"audio_set",
         "Add an audio component to an entity if it has none and apply fields (a partial update), as one undoable step. AudioSource: cue (a "
         "cue asset path such as Audio/hurt.acue), auto_play, volume_db, pitch. AudioListener: active (the first active one hears). "
         "ReverbZone: radius, blend_distance, priority, room_size, damping, wet (0..1).",
         Schema({{"entity", kGuidProp},
                 {"component", {{"type", "string"}, {"description", "AudioSource, AudioListener or ReverbZone"}}},
                 {"fields", {{"type", "object"}, {"description", "e.g. {\"cue\": \"Audio/ambient.acue\", \"volume_db\": -6}"}}}},
                {"entity", "component"}),
         [&s](const Json& args) -> Json {
             Entity e = RequireEntity(s, args);
             EntityGuid guid = RequireGuid(args, "entity");
             const std::string name = RequireString(args, "component");
             if (!IsAudioComponent(name)) throw ToolError("\"component\" must be AudioSource, AudioListener or ReverbZone");
             const Json fields = args.contains("fields") ? args["fields"] : Json::object();
             if (!fields.is_object()) throw ToolError("\"fields\" must be an object");
             ValidateAudioFields(name, fields);
             const ComponentId id = RequireComponent(name);
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction("Set " + name);
             if (!s.world.HasComponentRaw(e, id)) s.stack.Execute(ctx, std::make_unique<editor::AddComponentCommand>(guid, id));
             Json warnings = Json::array();
             if (!fields.empty()) {
                 std::vector<u8> before;
                 std::vector<u8> after;
                 try {
                     after = PatchedBytes(s, e, id, fields, before, warnings);
                 } catch (...) {
                     s.stack.EndTransaction();
                     s.stack.Undo(ctx);
                     throw;
                 }
                 s.stack.Execute(ctx, std::make_unique<SetComponentCommand>(guid, id, before, after));
             }
             s.stack.EndTransaction();
             Json out = {{"component", name}, {"value", ComponentJson(s, e, id)}};
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool({"audio_remove", "Remove an AudioSource, AudioListener or ReverbZone from an entity. Undoable.",
                    Schema({{"entity", kGuidProp}, {"component", {{"type", "string"}}}}, {"entity", "component"}), [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        const std::string name = RequireString(args, "component");
                        if (!IsAudioComponent(name)) throw ToolError("\"component\" must be AudioSource, AudioListener or ReverbZone");
                        const ComponentId id = RequireComponent(name);
                        if (!s.world.HasComponentRaw(e, id)) throw ToolError("Entity has no " + name);
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::RemoveComponentCommand>(guid, id));
                        return {{"removed", name}};
                    }});
}

} // namespace aether::mcp
