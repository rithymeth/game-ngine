#include "editor_tools.h"

#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"

#include <algorithm>
#include <new>

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

void RegisterBuiltinComponents() {
    GetComponentId<Transform>();
    GetComponentId<Parent>();
    GetComponentId<IdComponent>();
    GetComponentId<ModelRenderer>();
}

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
                    "Enter play mode (or resume from pause). The scene is snapshotted and restored exactly on stop.",
                    Schema(Json::object()), [&s](const Json&) -> Json {
                        editor::CommandContext ctx = s.Context();
                        s.play.Play(ctx, s.stack);
                        return {{"play_state", PlayStateName(s.play.GetState())}};
                    }});
    server.AddTool({"pause", "Pause play mode.", Schema(Json::object()), [&s](const Json&) -> Json {
                        s.play.Pause();
                        return {{"play_state", PlayStateName(s.play.GetState())}};
                    }});
    server.AddTool({"stop", "Leave play mode, restoring the scene as it was before play.", Schema(Json::object()),
                    [&s](const Json&) -> Json {
                        editor::CommandContext ctx = s.Context();
                        s.play.Stop(ctx, s.stack);
                        editor::EnsureAllGuids(s.world, s.guids);
                        return {{"play_state", PlayStateName(s.play.GetState())}};
                    }});
}

} // namespace aether::mcp
