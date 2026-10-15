#include "editor_tools.h"

#include "aether/reflection/serialize.h"
#include "aether/reflection/any.h"
#include "aether/gameplay/attribute_set.h"
#include "aether/gameplay/gameplay_tag.h"
#include "aether/scene/gameplay.h"
#if AETHER_MCP_INTERACTION
#include "aether/interaction/interactable.h"
#endif
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"
#include "core/commands.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

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

void RemapEntityGuidValues(const reflect::TypeInfo& type, Json& value,
                           const std::unordered_map<EntityGuid, EntityGuid>& guid_remap) {
    if (&type == &reflect::Reflect<EntityGuid>()) {
        if (!value.is_string()) return;
        EntityGuid referenced;
        if (!ParseEntityGuid(value.get<std::string>(), referenced)) return;
        const auto remapped = guid_remap.find(referenced);
        if (remapped != guid_remap.end()) value = ToString(remapped->second);
        return;
    }

    if (type.kind == reflect::TypeKind::Array) {
        if (type.element == nullptr || !value.is_array()) return;
        for (Json& element : value) RemapEntityGuidValues(*type.element, element, guid_remap);
        return;
    }
    if (type.kind != reflect::TypeKind::Struct) return;

    if (type.serialize_as_array && value.is_array()) {
        const usize count = std::min<usize>(type.fields.size(), value.size());
        for (usize index = 0; index < count; ++index) {
            RemapEntityGuidValues(*type.fields[index].type, value[index], guid_remap);
        }
    } else if (value.is_object()) {
        for (const reflect::FieldInfo& field : type.fields) {
            if (field.HasFlag(reflect::Field_Transient)) continue;
            auto member = value.find(field.name);
            if (member != value.end()) RemapEntityGuidValues(*field.type, *member, guid_remap);
        }
    }
}

} // namespace

void RegisterAttributeTools(McpServer& server, EditorSession& s);
void RegisterAudioEditorTools(McpServer& server, EditorSession& s);
void RegisterParticleEditorTools(McpServer& server, EditorSession& s);
void RegisterNavEditorTools(McpServer& server, EditorSession& s);
void RegisterSequenceEditorTools(McpServer& server, EditorSession& s);
#if AETHER_MCP_INTERACTION
void RegisterInteractionEditorTools(McpServer& server, EditorSession& s);
#endif
std::filesystem::path ResolveScenePath(const std::filesystem::path& root, const std::string& requested, std::string& error) {
    namespace fs = std::filesystem;
    if (requested.empty()) {
        error = "path is empty";
        return {};
    }
    const fs::path given(requested);
    const std::string ext = given.extension().string();
    if (ext != ".json" && ext != ".aesc") {
        error = "scene files end in .json or .aesc";
        return {};
    }
    if (root.empty()) return given;
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(root, ec);
    if (ec) {
        error = "the scene folder can't be resolved";
        return {};
    }
    const fs::path full = fs::weakly_canonical(given.is_absolute() ? given : base / given, ec);
    if (ec) {
        error = "can't resolve " + requested;
        return {};
    }
    const fs::path rel = full.lexically_relative(base);
    if (rel.empty() || *rel.begin() == "..") {
        error = requested + " is outside the scene folder";
        return {};
    }
    return full;
}

void RegisterEditorTools(McpServer& server, EditorSession& s) {
    // This session is controlled through MCP, so all its scene history entries
    // are attributable to an agent. The stack preserves this across undo/redo.
    s.stack.SetAttribution("agent");
    server.AddTool({"editor_state",
                    "Summary of the editor session: entity count, undo/redo availability, unsaved changes, play state.",
                    Schema(Json::object()), [&s](const Json&) -> Json {
                        return {{"entities", s.world.EntityCount()},
                                {"can_undo", s.stack.CanUndo()},
                                {"undo_label", s.stack.UndoLabel()},
                                {"undo_attribution", s.stack.UndoAttribution()},
                                {"can_redo", s.stack.CanRedo()},
                                {"redo_label", s.stack.RedoLabel()},
                                {"redo_attribution", s.stack.RedoAttribution()},
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
                    "List entities in the scene (GUID, component names and parent GUID). Optionally filter by component, exact entity name, a single exact tag, or tag groups that must all/any/none match; supplied filters combine with AND.",
                    Schema({{"component", {{"type", "string"}, {"description", "Only entities having this component"}}},
                            {"name", {{"type", "string"}, {"description", "Only entities whose name exactly matches"}}},
                            {"tag", {{"type", "string"}, {"description", "Only entities with this exact case-sensitive tag (legacy single-tag filter)"}}},
                            {"tags_all", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 32},
                                          {"description", "Only entities that have every exact case-sensitive tag listed"}}},
                            {"tags_any", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 32},
                                          {"description", "Only entities that have at least one exact case-sensitive tag listed"}}},
                            {"tags_none", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 32},
                                           {"description", "Exclude entities that have any exact case-sensitive tag listed"}}}}),
                    [&s](const Json& args) -> Json {
                        editor::EnsureAllGuids(s.world, s.guids);
                        ComponentId filter = kInvalidComponentId;
                        if (args.contains("component")) {
                            filter = RequireComponent(args["component"].get<std::string>());
                        }
                        const std::string name_filter = args.contains("name") ? RequireString(args, "name") : std::string{};
                        const std::string tag_filter = args.contains("tag") ? RequireString(args, "tag") : std::string{};
                        auto ReadTagFilter = [&](const char* key) {
                            std::vector<std::string> tags;
                            if (!args.contains(key)) return tags;
                            if (!args[key].is_array() || args[key].size() > 32) {
                                throw ToolError(std::string("\"") + key + "\" must be an array of at most 32 strings");
                            }
                            std::set<std::string> unique;
                            for (const Json& value : args[key]) {
                                if (!value.is_string() || value.get<std::string>().empty()) {
                                    throw ToolError(std::string("Every \"") + key + "\" entry must be a non-empty string");
                                }
                                if (!unique.insert(value.get<std::string>()).second) {
                                    throw ToolError(std::string("Duplicate tag in \"") + key + "\": " + value.get<std::string>());
                                }
                                tags.push_back(value.get<std::string>());
                            }
                            return tags;
                        };
                        const std::vector<std::string> tags_all = ReadTagFilter("tags_all");
                        const std::vector<std::string> tags_any = ReadTagFilter("tags_any");
                        const std::vector<std::string> tags_none = ReadTagFilter("tags_none");
                        Json out = Json::array();
                        for (Entity e : AllEntities(s)) {
                            if (filter != kInvalidComponentId && !s.world.HasComponentRaw(e, filter)) {
                                continue;
                            }
                            if (args.contains("name")) {
                                const EntityName* name = s.world.GetComponent<EntityName>(e);
                                if (name == nullptr || name->value != name_filter) continue;
                            }
                            if (args.contains("tag") && !HasTag(s.world, e, tag_filter)) continue;
                            bool has_all_tags = true;
                            for (const std::string& tag : tags_all) has_all_tags = has_all_tags && HasTag(s.world, e, tag);
                            if (!has_all_tags) continue;
                            if (!tags_any.empty()) {
                                bool has_any_tag = false;
                                for (const std::string& tag : tags_any) has_any_tag = has_any_tag || HasTag(s.world, e, tag);
                                if (!has_any_tag) continue;
                            }
                            bool has_excluded_tag = false;
                            for (const std::string& tag : tags_none) has_excluded_tag = has_excluded_tag || HasTag(s.world, e, tag);
                            if (has_excluded_tag) continue;
                            const IdComponent* id = s.world.GetComponent<IdComponent>(e);
                            const Parent* parent = s.world.GetComponent<Parent>(e);
                            Json row = {{"entity", id ? ToString(id->guid) : ""}, {"components", ComponentNamesOf(s, e)}};
                            if (parent != nullptr && !parent->parent.IsNull()) row["parent"] = ToString(parent->parent);
                            out.push_back(std::move(row));
                        }
                        return {{"count", out.size()}, {"entities", out}};
                    }});

    server.AddTool({"get_entity", "Every component of one entity, as JSON.", Schema({{"entity", kGuidProp}}, {"entity"}),
                    [&s](const Json& args) -> Json { return EntityJson(s, RequireEntity(s, args)); }});

    server.AddTool(
        {"get_entities_many",
         "Get every reflected component value for 1–64 unique scene entities in one response. The complete GUID selection is validated before any entity data is returned.",
         Schema({{"entities", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                               {"description", "1–64 unique scene entity GUIDs, returned in this order"}}}},
                {"entities"}),
         [&s](const Json& args) -> Json {
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
             }
             std::vector<std::pair<EntityGuid, Entity>> entities;
             entities.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"entities\" must be an entity GUID string");
                 const std::string guid_text = value.get<std::string>();
                 EntityGuid guid;
                 if (!ParseEntityGuid(guid_text, guid)) throw ToolError("Not a valid entity GUID: " + guid_text);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 entities.emplace_back(guid, entity);
             }

             Json results = Json::array();
             for (const auto& [guid, entity] : entities) {
                 Json result = EntityJson(s, entity);
                 result["entity"] = ToString(guid);
                 results.push_back(std::move(result));
             }
             return {{"count", results.size()}, {"entities", std::move(results)}};
         }});

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

    server.AddTool(
        {"create_entities",
         "Create 1–64 scene entities as one undoable action. Each entry may specify component values and either a current-scene parent GUID or a zero-based parent_index referring to another entry in this batch; all values and hierarchy links are validated before creation.",
         Schema({{"entities", {{"type", "array"},
                               {"items", {{"type", "object"},
                                          {"properties", {{"components", {{"type", "object"}, {"description", "Component names mapped to field values"}}},
                                                          {"parent", {{"type", Json::array({"string", "null"})}, {"description", "Optional current-scene parent GUID"}},
                                                          {"parent_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 63}, {"description", "Optional index of another entity entry in this batch"}}}}}}},
                               {"description", "1–64 entity definitions"}}}},
                {"entities"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before creating entities");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 definitions");
             }

             struct EntitySpec {
                 EntityGuid parent;
                 std::optional<usize> parent_index;
                 EntityGuid guid;
                 std::vector<std::pair<ComponentId, Json>> components;
             };
             std::vector<EntitySpec> specs;
             specs.reserve(args["entities"].size());
             const ComponentId id_component = GetComponentId<IdComponent>();
             for (usize index = 0; index < args["entities"].size(); ++index) {
                 const Json& entry = args["entities"][index];
                 if (!entry.is_object()) throw ToolError("Each entry in \"entities\" must be an object");
                 EntitySpec spec;
                 if (entry.contains("parent") && entry.contains("parent_index")) {
                     throw ToolError("Specify either \"parent\" or \"parent_index\", not both");
                 }
                 if (entry.contains("parent") && !entry["parent"].is_null()) {
                     spec.parent = RequireGuid(entry, "parent");
                     RequireEntity(s, entry, "parent");
                 }
                 if (entry.contains("parent_index")) {
                     const Json& parent_index = entry["parent_index"];
                     if (!parent_index.is_number_integer() || parent_index.is_boolean()) {
                         throw ToolError("\"parent_index\" must be an integer referring to another entry in \"entities\"");
                     }
                     const i64 raw_parent_index = parent_index.get<i64>();
                     if (raw_parent_index < 0 || static_cast<usize>(raw_parent_index) >= args["entities"].size()) {
                         throw ToolError("\"parent_index\" must refer to an entry in \"entities\"");
                     }
                     if (static_cast<usize>(raw_parent_index) == index) throw ToolError("An entity cannot parent itself");
                     spec.parent_index = static_cast<usize>(raw_parent_index);
                 }
                 if (entry.contains("components")) {
                     if (!entry["components"].is_object()) throw ToolError("\"components\" must be an object");
                     for (const auto& [name, value] : entry["components"].items()) {
                         const ComponentId id = RequireComponent(name);
                         if (id == id_component) continue;
                         const ComponentInfo& info = GetComponentInfo(id);
                         if (info.reflected == nullptr) throw ToolError("Cannot initialize unreflected component " + ComponentName(id));
                         reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                         if (!sample.HasValue()) throw ToolError("Cannot default-construct component " + ComponentName(id));
                         reflect::LoadReport report;
                         if (!reflect::FromJson(*info.reflected, sample.Data(), value, &report)) {
                             throw ToolError("Value does not match the shape of " + ComponentName(id));
                         }
                         spec.components.emplace_back(id, value);
                     }
                 }
                 specs.push_back(std::move(spec));
             }

             std::vector<std::vector<usize>> children(specs.size());
             std::vector<usize> indegree(specs.size(), 0);
             for (usize index = 0; index < specs.size(); ++index) {
                 if (specs[index].parent_index) {
                     children[*specs[index].parent_index].push_back(index);
                     indegree[index] = 1;
                 }
             }
             std::vector<usize> creation_order;
             creation_order.reserve(specs.size());
             for (usize index = 0; index < specs.size(); ++index) {
                 if (indegree[index] == 0) creation_order.push_back(index);
             }
             for (usize cursor = 0; cursor < creation_order.size(); ++cursor) {
                 for (const usize child : children[creation_order[cursor]]) {
                     if (--indegree[child] == 0) creation_order.push_back(child);
                 }
             }
             if (creation_order.size() != specs.size()) throw ToolError("The requested parent_index links contain a cycle");
             for (EntitySpec& spec : specs) spec.guid = NewEntityGuid();
             for (EntitySpec& spec : specs) {
                 if (spec.parent_index) spec.parent = specs[*spec.parent_index].guid;
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Create Entities " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             std::vector<Json> result_rows(specs.size());
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const usize spec_index : creation_order) {
                     const EntitySpec& spec = specs[spec_index];
                     const EntityGuid guid = spec.guid;
                     Entity entity = s.world.CreateEntity(IdComponent{guid});
                     s.guids.Add(guid, entity);
                     try {
                         Json warnings = Json::array();
                         for (const auto& [id, value] : spec.components) {
                             s.world.AddComponentRaw(entity, id);
                             reflect::LoadReport report;
                             const ComponentInfo& info = GetComponentInfo(id);
                             if (!reflect::FromJson(*info.reflected, s.world.GetComponentRaw(entity, id), value, &report)) {
                                 throw ToolError("Could not initialize " + ComponentName(id));
                             }
                             for (const std::string& warning : report.warnings) warnings.push_back(warning);
                         }
                         if (!spec.parent.IsNull()) s.world.AddComponent(entity, Parent{spec.parent});
                         Json row = EntityJson(s, entity);
                         if (!warnings.empty()) row["warnings"] = std::move(warnings);
                         result_rows[spec_index] = std::move(row);
                         auto command = editor::CreateEntityCommand::FromExisting(ctx, entity);
                         command->SetAttribution("agent");
                         s.stack.Record(std::move(command));
                     } catch (...) {
                         s.guids.Remove(guid);
                         s.world.DestroyEntity(entity);
                         throw;
                     }
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }
             Json results = Json::array();
             for (Json& row : result_rows) results.push_back(std::move(row));
             return {{"created", results.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"duplicate_entity",
         "Duplicate one entity with its reflected component values. The new entity keeps the source's parent unless `parent` is supplied; "
         "set `parent` to null to detach it. `name` can override the copied name. Children are not duplicated, and entity GUID references in "
         "component values are copied as-is. Undoable.",
         Schema({{"entity", kGuidProp}, {"name", {{"type", "string"}, {"description", "Optional name for the duplicate"}}},
                 {"parent", {{"type", Json::array({"string", "null"})}, {"description", "Parent entity GUID, or null to make the duplicate a root"}}}},
                {"entity"}),
         [&s](const Json& args) -> Json {
             const Entity source = RequireEntity(s, args);
             const IdComponent* source_id = s.world.GetComponent<IdComponent>(source);
             if (source_id == nullptr || source_id->guid.IsNull()) throw ToolError("The source entity has no GUID");

             const ComponentId id_component = GetComponentId<IdComponent>();
             const ComponentId parent_component = GetComponentId<Parent>();
             std::vector<std::pair<ComponentId, Json>> values;
             for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
                 if (!s.world.HasComponentRaw(source, id) || id == id_component || id == parent_component) continue;
                 const ComponentInfo& info = GetComponentInfo(id);
                 if (info.reflected == nullptr) throw ToolError("Cannot duplicate an entity with unreflected component " + ComponentName(id));
                 values.emplace_back(id, reflect::ToJson(*info.reflected, s.world.GetComponentRaw(source, id)));
             }

             EntityGuid parent;
             if (args.contains("parent")) {
                 if (!args["parent"].is_null()) {
                     parent = RequireGuid(args, "parent");
                     RequireEntity(s, args, "parent");
                 }
             } else if (const Parent* source_parent = s.world.GetComponent<Parent>(source)) {
                 parent = source_parent->parent;
             }

             const EntityGuid guid = NewEntityGuid();
             Entity duplicate = s.world.CreateEntity(IdComponent{guid});
             s.guids.Add(guid, duplicate);
             Json warnings = Json::array();
             Json out;
             try {
                 for (const auto& [id, value] : values) {
                     s.world.AddComponentRaw(duplicate, id);
                     reflect::LoadReport report;
                     const ComponentInfo& info = GetComponentInfo(id);
                     if (!reflect::FromJson(*info.reflected, s.world.GetComponentRaw(duplicate, id), value, &report)) {
                         throw ToolError("Could not copy " + ComponentName(id));
                     }
                     for (const std::string& warning : report.warnings) warnings.push_back(warning);
                 }
                 if (args.contains("name")) {
                     const std::string name = RequireString(args, "name");
                     EntityName* component = s.world.GetComponent<EntityName>(duplicate);
                     if (component == nullptr) {
                         s.world.AddComponent<EntityName>(duplicate);
                         component = s.world.GetComponent<EntityName>(duplicate);
                     }
                     component->value = name;
                 }
                 if (!parent.IsNull()) s.world.AddComponent(duplicate, Parent{parent});
                 out = EntityJson(s, duplicate);
                 if (!warnings.empty()) out["warnings"] = std::move(warnings);
             } catch (...) {
                 s.guids.Remove(guid);
                 s.world.DestroyEntity(duplicate);
                 throw;
             }
             editor::CommandContext ctx = s.Context();
             s.stack.Record(editor::CreateEntityCommand::FromExisting(ctx, duplicate));
             return out;
         }});

    server.AddTool(
        {"duplicate_entities",
         "Duplicate up to 64 unique entities as one undoable action. Each duplicate copies reflected components and keeps its source's parent. Set include_children to true to expand each selection to its descendants; this also preserves parent links inside the duplicated group. Otherwise, set preserve_hierarchy to true to preserve links only among explicitly selected entities. Set remap_internal_references to true to redirect reflected EntityGuid references between duplicated entities to their copies; external references stay unchanged.",
         Schema({{"entities", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "1–64 unique entity GUIDs"}}},
                 {"preserve_hierarchy", {{"type", "boolean"}, {"description", "When true, preserve parent links whose parent is also in the selection; defaults to false"}}},
                 {"include_children", {{"type", "boolean"}, {"description", "When true, include all descendants of the selected entities; defaults to false and still enforces a 64-entity total limit"}}},
                 {"remap_internal_references", {{"type", "boolean"}, {"description", "When true, remap reflected EntityGuid references to duplicated entities; defaults to false"}}}},
                {"entities"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before duplicating entities");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 GUIDs");
             }
             if (args.contains("preserve_hierarchy") && !args["preserve_hierarchy"].is_boolean()) {
                 throw ToolError("\"preserve_hierarchy\" must be true or false");
             }
             if (args.contains("include_children") && !args["include_children"].is_boolean()) {
                 throw ToolError("\"include_children\" must be true or false");
             }
             if (args.contains("remap_internal_references") && !args["remap_internal_references"].is_boolean()) {
                 throw ToolError("\"remap_internal_references\" must be true or false");
             }
             const bool include_children = args.value("include_children", false);
             const bool preserve_hierarchy = args.value("preserve_hierarchy", false) || include_children;
             const bool remap_internal_references = args.value("remap_internal_references", false);

             struct DuplicateSpec {
                 EntityGuid source_guid;
                 EntityGuid parent;
                 EntityGuid duplicate_guid;
                 std::vector<std::pair<ComponentId, Json>> components;
             };
             std::vector<DuplicateSpec> specs;
             specs.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             std::vector<EntityGuid> source_guids;
             source_guids.reserve(args["entities"].size());
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Each entry in \"entities\" must be an entity GUID string");
                 const Entity source = RequireEntity(s, Json{{"entity", value}});
                 const IdComponent* source_id = s.world.GetComponent<IdComponent>(source);
                 if (source_id == nullptr || source_id->guid.IsNull()) throw ToolError("The source entity has no GUID");
                 if (!seen.insert(source_id->guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(source_id->guid));
                 source_guids.push_back(source_id->guid);
             }

             if (include_children) {
                 std::map<EntityGuid, std::vector<EntityGuid>> children_by_parent;
                 std::set<EntityGuid> parents_with_unguided_children;
                 for (const Entity entity : AllEntities(s)) {
                     const Parent* parent = s.world.GetComponent<Parent>(entity);
                     if (parent == nullptr || parent->parent.IsNull()) continue;
                     const IdComponent* child_id = s.world.GetComponent<IdComponent>(entity);
                     if (child_id == nullptr || child_id->guid.IsNull()) {
                         parents_with_unguided_children.insert(parent->parent);
                         continue;
                     }
                     children_by_parent[parent->parent].push_back(child_id->guid);
                 }
                 for (usize cursor = 0; cursor < source_guids.size(); ++cursor) {
                     const EntityGuid parent_guid = source_guids[cursor];
                     if (parents_with_unguided_children.contains(parent_guid)) {
                         throw ToolError("Cannot include descendants because a child entity has no GUID");
                     }
                     const auto descendants = children_by_parent.find(parent_guid);
                     if (descendants == children_by_parent.end()) continue;
                     for (const EntityGuid child_guid : descendants->second) {
                         if (!seen.insert(child_guid).second) continue;
                         if (source_guids.size() >= 64) {
                             throw ToolError("Including descendants would duplicate more than 64 entities");
                         }
                         source_guids.push_back(child_guid);
                     }
                 }
             }

             const ComponentId id_component = GetComponentId<IdComponent>();
             const ComponentId parent_component = GetComponentId<Parent>();
             for (const EntityGuid source_guid : source_guids) {
                 const Entity source = s.guids.Find(s.world, source_guid);
                 if (source.IsNull()) throw ToolError("The source entity no longer exists: " + ToString(source_guid));
                 const IdComponent* source_id = s.world.GetComponent<IdComponent>(source);
                 if (source_id == nullptr || source_id->guid.IsNull()) throw ToolError("The source entity has no GUID");

                 DuplicateSpec spec;
                 spec.source_guid = source_id->guid;
                 if (const Parent* source_parent = s.world.GetComponent<Parent>(source)) spec.parent = source_parent->parent;
                 for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
                     if (!s.world.HasComponentRaw(source, id) || id == id_component || id == parent_component) continue;
                     const ComponentInfo& info = GetComponentInfo(id);
                     if (info.reflected == nullptr) throw ToolError("Cannot duplicate an entity with unreflected component " + ComponentName(id));
                     spec.components.emplace_back(id, reflect::ToJson(*info.reflected, s.world.GetComponentRaw(source, id)));
                 }
                 specs.push_back(std::move(spec));
             }

             std::vector<std::optional<usize>> parent_indices(specs.size());
             std::vector<std::vector<usize>> children(specs.size());
             std::vector<usize> indegree(specs.size(), 0);
             if (preserve_hierarchy) {
                 for (usize child_index = 0; child_index < specs.size(); ++child_index) {
                     if (specs[child_index].parent.IsNull()) continue;
                     const auto parent = std::find_if(specs.begin(), specs.end(), [&](const DuplicateSpec& candidate) {
                         return candidate.source_guid == specs[child_index].parent;
                     });
                     if (parent == specs.end()) continue;
                     const usize parent_index = static_cast<usize>(std::distance(specs.begin(), parent));
                     parent_indices[child_index] = parent_index;
                     children[parent_index].push_back(child_index);
                     indegree[child_index] = 1;
                 }
             }

             std::vector<usize> creation_order;
             creation_order.reserve(specs.size());
             for (usize index = 0; index < specs.size(); ++index) {
                 if (indegree[index] == 0) creation_order.push_back(index);
             }
             for (usize cursor = 0; cursor < creation_order.size(); ++cursor) {
                 for (const usize child_index : children[creation_order[cursor]]) {
                     if (--indegree[child_index] == 0) creation_order.push_back(child_index);
                 }
             }
             if (creation_order.size() != specs.size()) throw ToolError("The selected entities contain a cycle in their parent links");

             std::unordered_map<EntityGuid, EntityGuid> guid_remap;
             guid_remap.reserve(specs.size());
             for (DuplicateSpec& spec : specs) {
                 spec.duplicate_guid = NewEntityGuid();
                 guid_remap.emplace(spec.source_guid, spec.duplicate_guid);
             }
             for (usize index = 0; index < specs.size(); ++index) {
                 if (parent_indices[index]) specs[index].parent = specs[*parent_indices[index]].duplicate_guid;
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Duplicate Entities " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             std::vector<Json> result_rows(specs.size());
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const usize spec_index : creation_order) {
                     const DuplicateSpec& spec = specs[spec_index];
                     const EntityGuid guid = spec.duplicate_guid;
                     Entity duplicate = s.world.CreateEntity(IdComponent{guid});
                     s.guids.Add(guid, duplicate);
                     try {
                         Json warnings = Json::array();
                         for (const auto& [id, source_value] : spec.components) {
                             s.world.AddComponentRaw(duplicate, id);
                             reflect::LoadReport report;
                             const ComponentInfo& info = GetComponentInfo(id);
                             Json value = source_value;
                             if (remap_internal_references) RemapEntityGuidValues(*info.reflected, value, guid_remap);
                             if (!reflect::FromJson(*info.reflected, s.world.GetComponentRaw(duplicate, id), value, &report)) {
                                 throw ToolError("Could not copy " + ComponentName(id));
                             }
                             for (const std::string& warning : report.warnings) warnings.push_back(warning);
                         }
                         if (!spec.parent.IsNull()) s.world.AddComponent(duplicate, Parent{spec.parent});
                         Json row = {{"source", ToString(spec.source_guid)}, {"entity", EntityJson(s, duplicate)}};
                         if (!warnings.empty()) row["warnings"] = std::move(warnings);
                         result_rows[spec_index] = std::move(row);
                         auto command = editor::CreateEntityCommand::FromExisting(ctx, duplicate);
                         command->SetAttribution("agent");
                         s.stack.Record(std::move(command));
                     } catch (...) {
                         s.guids.Remove(guid);
                         s.world.DestroyEntity(duplicate);
                         throw;
                     }
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }
             Json results = Json::array();
             for (Json& row : result_rows) results.push_back(std::move(row));
             return {{"duplicated", results.size()}, {"results", std::move(results)}};
         }});

    server.AddTool({"destroy_entity", "Delete an entity. Undoable.", Schema({{"entity", kGuidProp}}, {"entity"}),
                    [&s](const Json& args) -> Json {
                        EntityGuid guid = RequireGuid(args, "entity");
                        RequireEntity(s, args);
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::DestroyEntityCommand>(guid));
                        return {{"destroyed", ToString(guid)}};
                    }});

    server.AddTool(
        {"destroy_entities",
         "Delete up to 64 unique scene entities as one undoable action. Children and entity GUID references are left unchanged by default. Set include_children to true to also delete descendants; the batch deletes children before parents so undo restores parents first.",
         Schema({{"entities", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "1–64 unique entity GUIDs"}}},
                 {"include_children", {{"type", "boolean"}, {"description", "When true, also delete descendants; defaults to false and enforces a 64-entity total limit"}}}},
                {"entities"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before deleting entities");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 GUIDs");
             }
             if (args.contains("include_children") && !args["include_children"].is_boolean()) {
                 throw ToolError("\"include_children\" must be true or false");
             }
             const bool include_children = args.value("include_children", false);

             std::vector<EntityGuid> guids;
             guids.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Each entry in \"entities\" must be an entity GUID string");
                 const EntityGuid guid = RequireGuid(Json{{"entity", value}}, "entity");
                 RequireEntity(s, Json{{"entity", value}});
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 guids.push_back(guid);
             }

             if (include_children) {
                 std::map<EntityGuid, std::vector<EntityGuid>> children_by_parent;
                 std::set<EntityGuid> parents_with_unguided_children;
                 for (const Entity entity : AllEntities(s)) {
                     const Parent* parent = s.world.GetComponent<Parent>(entity);
                     if (parent == nullptr || parent->parent.IsNull()) continue;
                     const IdComponent* child_id = s.world.GetComponent<IdComponent>(entity);
                     if (child_id == nullptr || child_id->guid.IsNull()) {
                         parents_with_unguided_children.insert(parent->parent);
                         continue;
                     }
                     children_by_parent[parent->parent].push_back(child_id->guid);
                 }

                 const std::vector<EntityGuid> roots = guids;
                 guids.clear();
                 std::set<EntityGuid> visited;
                 std::set<EntityGuid> visiting;
                 std::function<void(EntityGuid)> append_postorder = [&](EntityGuid guid) {
                     if (visited.count(guid)) return;
                     if (!visiting.insert(guid).second) throw ToolError("The selected hierarchy contains a parent cycle");
                     if (parents_with_unguided_children.count(guid)) {
                         throw ToolError("Cannot include descendants because a child entity has no GUID");
                     }
                     const auto descendants = children_by_parent.find(guid);
                     if (descendants != children_by_parent.end()) {
                         for (const EntityGuid child : descendants->second) append_postorder(child);
                     }
                     visiting.erase(guid);
                     visited.insert(guid);
                     if (guids.size() >= 64) throw ToolError("Including descendants would delete more than 64 entities");
                     guids.push_back(guid);
                 };
                 for (const EntityGuid root : roots) append_postorder(root);
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Delete Entities " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const EntityGuid guid : guids) {
                     auto command = std::make_unique<editor::DestroyEntityCommand>(guid);
                     command->SetAttribution("agent");
                     s.stack.Execute(ctx, std::move(command));
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }
             Json deleted = Json::array();
             for (const EntityGuid guid : guids) deleted.push_back(ToString(guid));
             return {{"destroyed", deleted.size()}, {"entities", std::move(deleted)}};
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

    server.AddTool(
        {"add_components_many",
         "Add components to 1–64 unique entities as one undoable action. Each edit maps component names to optional initial field objects; all entities, component presence and field values are validated before any component is added.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"components", {{"type", "object"},
                                                                         {"description", "Component names mapped to optional initial field objects"}}}}},
                                        {"required", Json::array({"entity", "components"})}}},
                             {"description", "1–64 unique entities and the components to add"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before adding components");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity edits");
             }

             struct ComponentAddition {
                 ComponentId id;
                 std::vector<u8> defaults;
                 std::vector<u8> initialized;
                 Json warnings = Json::array();
             };
             struct EntityAddition {
                 EntityGuid guid;
                 std::vector<ComponentAddition> components;
             };
             std::vector<EntityAddition> additions;
             additions.reserve(args["edits"].size());
             std::set<EntityGuid> seen_entities;
             const ComponentId id_component = GetComponentId<IdComponent>();
             for (const Json& edit : args["edits"]) {
                 if (!edit.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(edit, "entity");
                 const Entity entity = RequireEntity(s, edit);
                 if (!seen_entities.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!edit.contains("components") || !edit["components"].is_object() || edit["components"].empty()) {
                     throw ToolError("Each edit must include a non-empty \"components\" object");
                 }

                 EntityAddition entity_addition;
                 entity_addition.guid = guid;
                 entity_addition.components.reserve(edit["components"].size());
                 for (const auto& [name, value] : edit["components"].items()) {
                     if (!value.is_object()) throw ToolError("Each component initializer must be a field object");
                     const ComponentId id = RequireComponent(name);
                     if (id == id_component) throw ToolError("An entity's identity cannot be added");
                     if (s.world.HasComponentRaw(entity, id)) {
                         throw ToolError("Entity " + ToString(guid) + " already has " + ComponentName(id));
                     }
                     const ComponentInfo& info = GetComponentInfo(id);
                     reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                     if (!sample.HasValue()) throw ToolError("Cannot default-construct component " + ComponentName(id));
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, sample.Data(), value, &report)) {
                         throw ToolError("Value does not match the shape of " + ComponentName(id));
                     }

                     ComponentAddition component_addition;
                     component_addition.id = id;
                     info.serialize(sample.Data(), component_addition.initialized);
                     reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                     if (!defaults.HasValue()) throw ToolError("Cannot default-construct component " + ComponentName(id));
                     info.serialize(defaults.Data(), component_addition.defaults);
                     for (const std::string& warning : report.warnings) component_addition.warnings.push_back(warning);
                     entity_addition.components.push_back(std::move(component_addition));
                 }
                 additions.push_back(std::move(entity_addition));
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Add Components " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const EntityAddition& entity_addition : additions) {
                     for (const ComponentAddition& component_addition : entity_addition.components) {
                         auto add = std::make_unique<editor::AddComponentCommand>(entity_addition.guid, component_addition.id);
                         add->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(add));
                         if (!component_addition.initialized.empty() && component_addition.initialized != component_addition.defaults) {
                             auto initialize = std::make_unique<SetComponentCommand>(entity_addition.guid, component_addition.id,
                                                                                    component_addition.defaults, component_addition.initialized);
                             initialize->SetAttribution("agent");
                             s.stack.Execute(ctx, std::move(initialize));
                         }
                     }
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json results = Json::array();
             for (const EntityAddition& entity_addition : additions) {
                 Json components = Json::object();
                 Json warnings = Json::object();
                 const Entity entity = s.guids.Find(s.world, entity_addition.guid);
                 for (const ComponentAddition& component_addition : entity_addition.components) {
                     const std::string name = ComponentName(component_addition.id);
                     components[name] = ComponentJson(s, entity, component_addition.id);
                     if (!component_addition.warnings.empty()) warnings[name] = component_addition.warnings;
                 }
                 Json row = {{"entity", ToString(entity_addition.guid)}, {"components", std::move(components)}};
                 if (!warnings.empty()) row["warnings"] = std::move(warnings);
                 results.push_back(std::move(row));
             }
             return {{"added", additions.size()}, {"edits", std::move(results)}};
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

    server.AddTool(
        {"remove_components_many",
         "Remove 1–64 components from scene entities as one undoable action. The complete entity/component selection is validated before any removal.",
         Schema({{"removals", {{"type", "array"},
                                {"items", {{"type", "object"},
                                           {"properties", {{"entity", kGuidProp}, {"component", {{"type", "string"}}}}},
                                           {"required", Json::array({"entity", "component"})}}},
                                {"description", "1–64 entity/component pairs"}}}},
                {"removals"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before removing components");
             if (!args["removals"].is_array() || args["removals"].empty() || args["removals"].size() > 64) {
                 throw ToolError("\"removals\" must be a non-empty array of at most 64 component removals");
             }

             struct Removal {
                 EntityGuid entity;
                 ComponentId component;
                 std::string component_name;
             };
             std::vector<Removal> removals;
             removals.reserve(args["removals"].size());
             std::set<std::pair<EntityGuid, ComponentId>> seen;
             const ComponentId id_component = GetComponentId<IdComponent>();
             for (const Json& entry : args["removals"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"removals\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 const ComponentId id = RequireComponent(RequireString(entry, "component"));
                 if (id == id_component) throw ToolError("An entity's identity can't be removed");
                 if (!s.world.HasComponentRaw(entity, id)) {
                     throw ToolError("Entity " + ToString(guid) + " doesn't have " + ComponentName(id));
                 }
                 if (!seen.emplace(guid, id).second) {
                     throw ToolError("Duplicate component removal for entity " + ToString(guid) + " and " + ComponentName(id));
                 }
                 removals.push_back({guid, id, ComponentName(id)});
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Remove Components " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const Removal& removal : removals) {
                     auto command = std::make_unique<editor::RemoveComponentCommand>(removal.entity, removal.component);
                     command->SetAttribution("agent");
                     s.stack.Execute(ctx, std::move(command));
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json removed = Json::array();
             for (const Removal& removal : removals) {
                 removed.push_back({{"entity", ToString(removal.entity)}, {"component", removal.component_name}});
             }
             return {{"removed", removed.size()}, {"components", std::move(removed)}};
         }});

    server.AddTool(
        {"rename_entities",
         "Rename 1–64 unique scene entities as one undoable action. Entities without an EntityName component receive one; all IDs and names are validated before any rename is applied.",
         Schema({{"renames", {{"type", "array"},
                               {"items", {{"type", "object"},
                                          {"properties", {{"entity", kGuidProp}, {"name", {{"type", "string"}}}}},
                                          {"required", Json::array({"entity", "name"})}}},
                               {"description", "1–64 unique entity GUID and name pairs"}}}},
                {"renames"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before renaming entities");
             if (!args["renames"].is_array() || args["renames"].empty() || args["renames"].size() > 64) {
                 throw ToolError("\"renames\" must be a non-empty array of at most 64 entity renames");
             }

             struct Rename {
                 EntityGuid guid;
                 std::string name;
                 bool add_component = false;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 Json warnings = Json::array();
             };
             std::vector<Rename> renames;
             renames.reserve(args["renames"].size());
             std::set<EntityGuid> seen;
             const ComponentId name_component = GetComponentId<EntityName>();
             for (const Json& entry : args["renames"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"renames\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"renames\": " + ToString(guid));
                 Rename rename;
                 rename.guid = guid;
                 rename.name = RequireString(entry, "name");
                 const Json value = {{"value", rename.name}};
                 if (s.world.HasComponentRaw(entity, name_component)) {
                     rename.after = PatchedBytes(s, entity, name_component, value, rename.before, rename.warnings);
                 } else {
                     const ComponentInfo& info = GetComponentInfo(name_component);
                     reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                     if (!sample.HasValue()) throw ToolError("Cannot default-construct EntityName");
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, sample.Data(), value, &report)) {
                         throw ToolError("Name does not match the EntityName component");
                     }
                     info.serialize(sample.Data(), rename.after);
                     reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                     if (!defaults.HasValue()) throw ToolError("Cannot default-construct EntityName");
                     info.serialize(defaults.Data(), rename.before);
                     rename.add_component = true;
                     for (const std::string& warning : report.warnings) rename.warnings.push_back(warning);
                 }
                 renames.push_back(std::move(rename));
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Rename Entities " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const Rename& rename : renames) {
                     if (rename.add_component) {
                         auto add = std::make_unique<editor::AddComponentCommand>(rename.guid, name_component);
                         add->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(add));
                     }
                     auto set = std::make_unique<SetComponentCommand>(rename.guid, name_component, rename.before, rename.after);
                     set->SetAttribution("agent");
                     s.stack.Execute(ctx, std::move(set));
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json results = Json::array();
             for (const Rename& rename : renames) {
                 Json row = {{"entity", ToString(rename.guid)}, {"name", rename.name}};
                 if (!rename.warnings.empty()) row["warnings"] = rename.warnings;
                 results.push_back(std::move(row));
             }
             return {{"renamed", renames.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entity_tags_many",
         "Set the complete tag list for 1–64 unique scene entities as one undoable action. This replaces each entity's existing tags; entities without a Tags component receive one. Validate every GUID and tag list before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                                        {"required", Json::array({"entity", "tags"})}}},
                             {"description", "1–64 unique entity GUID and complete tag-list pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before editing entity tags");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity tag edits");
             }

             struct TagEdit {
                 EntityGuid guid;
                 Json tags;
                 bool add_component = false;
                 std::vector<u8> before;
                 std::vector<u8> after;
             };
             std::vector<TagEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen_entities;
             std::set<std::string> seen_tags;
             const ComponentId tags_component = GetComponentId<Tags>();
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 if (!seen_entities.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("tags") || !entry["tags"].is_array()) throw ToolError("Each edit must include a \"tags\" array");
                 seen_tags.clear();
                 for (const Json& tag : entry["tags"]) {
                     if (!tag.is_string()) throw ToolError("Each tag must be a string");
                     if (!seen_tags.insert(tag.get<std::string>()).second) throw ToolError("Duplicate tag in entity " + ToString(guid));
                 }

                 TagEdit edit;
                 edit.guid = guid;
                 edit.tags = entry["tags"];
                 const Json value = {{"names", edit.tags}};
                 if (s.world.HasComponentRaw(entity, tags_component)) {
                     Json warnings = Json::array();
                     edit.after = PatchedBytes(s, entity, tags_component, value, edit.before, warnings);
                     if (!warnings.empty()) throw ToolError("Invalid Tags value for entity " + ToString(guid));
                 } else {
                     const ComponentInfo& info = GetComponentInfo(tags_component);
                     reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                     if (!sample.HasValue()) throw ToolError("Cannot default-construct Tags");
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, sample.Data(), value, &report)) {
                         throw ToolError("Invalid tag list for entity " + ToString(guid));
                     }
                     info.serialize(sample.Data(), edit.after);
                     reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                     if (!defaults.HasValue()) throw ToolError("Cannot default-construct Tags");
                     info.serialize(defaults.Data(), edit.before);
                     edit.add_component = true;
                 }
                 edits.push_back(std::move(edit));
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Set Entity Tags " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const TagEdit& edit : edits) {
                     if (edit.add_component) {
                         auto add = std::make_unique<editor::AddComponentCommand>(edit.guid, tags_component);
                         add->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(add));
                     }
                     auto set = std::make_unique<SetComponentCommand>(edit.guid, tags_component, edit.before, edit.after);
                     set->SetAttribution("agent");
                     s.stack.Execute(ctx, std::move(set));
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json results = Json::array();
             for (const TagEdit& edit : edits) results.push_back({{"entity", ToString(edit.guid)}, {"tags", edit.tags}});
             return {{"updated", edits.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"modify_entity_tags_many",
         "Add and/or remove tags on 1–64 unique scene entities as one undoable action. Existing tag order is preserved; adding an existing tag or removing an absent tag is a no-op. Validate every GUID and tag list before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"add", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                                        {"remove", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                                        {"required", Json::array({"entity"})}}},
                             {"description", "1–64 unique entity GUIDs and optional tag arrays to add/remove"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before editing entity tags");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity tag edits");
             }

             struct TagPatch {
                 EntityGuid guid;
                 Json tags;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool add_component = false;
                 bool changed = false;
             };
             std::vector<TagPatch> patches;
             patches.reserve(args["edits"].size());
             std::set<EntityGuid> seen_entities;
             const ComponentId tags_component = GetComponentId<Tags>();
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 if (!seen_entities.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));

                 std::vector<std::string> additions;
                 std::vector<std::string> removals;
                 std::set<std::string> unique_additions;
                 std::set<std::string> unique_removals;
                 for (const auto& [field, destination, unique] : {std::tuple<const char*, std::vector<std::string>*, std::set<std::string>*>("add", &additions, &unique_additions),
                                                                  std::tuple<const char*, std::vector<std::string>*, std::set<std::string>*>("remove", &removals, &unique_removals)}) {
                     if (!entry.contains(field)) continue;
                     if (!entry[field].is_array()) throw ToolError("\"" + std::string(field) + "\" must be an array of strings");
                     for (const Json& tag : entry[field]) {
                         if (!tag.is_string() || tag.get<std::string>().empty()) throw ToolError("Tags must be non-empty strings");
                         if (!unique->insert(tag.get<std::string>()).second) throw ToolError("Duplicate tag in \"" + std::string(field) + "\" for entity " + ToString(guid));
                         destination->push_back(tag.get<std::string>());
                     }
                 }
                 if (additions.empty() && removals.empty()) throw ToolError("Each edit must add or remove at least one tag");
                 for (const std::string& tag : additions) {
                     if (unique_removals.contains(tag)) throw ToolError("A tag cannot be both added and removed for entity " + ToString(guid) + ": " + tag);
                 }

                 TagPatch patch;
                 patch.guid = guid;
                 Json names = Json::array();
                 if (s.world.HasComponentRaw(entity, tags_component)) {
                     names = ComponentJson(s, entity, tags_component).value("names", Json::array());
                     if (!names.is_array()) throw ToolError("Invalid Tags value for entity " + ToString(guid));
                 }
                 std::vector<std::string> updated;
                 std::set<std::string> existing;
                 for (const Json& current : names) {
                     if (!current.is_string()) throw ToolError("Invalid Tags value for entity " + ToString(guid));
                     const std::string value = current.get<std::string>();
                     if (unique_removals.contains(value)) continue;
                     updated.push_back(value);
                     existing.insert(value);
                 }
                 for (const std::string& tag : additions) {
                     if (existing.insert(tag).second) updated.push_back(tag);
                 }
                 patch.tags = updated;
                 if (s.world.HasComponentRaw(entity, tags_component)) {
                     Json warnings = Json::array();
                     patch.after = PatchedBytes(s, entity, tags_component, Json{{"names", patch.tags}}, patch.before, warnings);
                     if (!warnings.empty()) throw ToolError("Invalid Tags value for entity " + ToString(guid));
                     patch.changed = patch.before != patch.after;
                 } else if (!updated.empty()) {
                     const ComponentInfo& info = GetComponentInfo(tags_component);
                     reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                     if (!sample.HasValue()) throw ToolError("Cannot default-construct Tags");
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, sample.Data(), Json{{"names", patch.tags}}, &report)) {
                         throw ToolError("Invalid tag list for entity " + ToString(guid));
                     }
                     info.serialize(sample.Data(), patch.after);
                     reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                     if (!defaults.HasValue()) throw ToolError("Cannot default-construct Tags");
                     info.serialize(defaults.Data(), patch.before);
                     patch.add_component = true;
                     patch.changed = true;
                 }
                 patches.push_back(std::move(patch));
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Modify Entity Tags " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             bool any_changed = false;
             for (const TagPatch& patch : patches) any_changed = any_changed || patch.changed;
             if (any_changed) {
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const TagPatch& patch : patches) {
                         if (!patch.changed) continue;
                         if (patch.add_component) {
                             auto add = std::make_unique<editor::AddComponentCommand>(patch.guid, tags_component);
                             add->SetAttribution("agent");
                             s.stack.Execute(ctx, std::move(add));
                         }
                         auto set = std::make_unique<SetComponentCommand>(patch.guid, tags_component, patch.before, patch.after);
                         set->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(set));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const TagPatch& patch : patches) results.push_back({{"entity", ToString(patch.guid)}, {"tags", patch.tags}, {"changed", patch.changed}});
             return {{"updated", std::count_if(patches.begin(), patches.end(), [](const TagPatch& patch) { return patch.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entity_layers_many",
         "Set the collision layer index on 1–64 unique scene entities as one undoable action. Layer indices are 0–31; entities without a Layer component receive one. All IDs and indices are validated before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp}, {"layer_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxLayers - 1}}}}},
                                        {"required", Json::array({"entity", "layer_index"})}}},
                             {"description", "1–64 unique entity GUID and layer-index pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity layers");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity layer edits");
             }

             struct LayerEdit {
                 EntityGuid guid;
                 u8 index;
                 bool add_component = false;
                 std::vector<u8> before;
                 std::vector<u8> after;
             };
             std::vector<LayerEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId layer_component = GetComponentId<Layer>();
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("layer_index") || !entry["layer_index"].is_number_integer() || entry["layer_index"].is_boolean()) {
                     throw ToolError("\"layer_index\" must be an integer from 0 to 31");
                 }
                 const i64 raw_index = entry["layer_index"].get<i64>();
                 if (raw_index < 0 || raw_index >= kMaxLayers) throw ToolError("\"layer_index\" must be an integer from 0 to 31");

                 LayerEdit edit;
                 edit.guid = guid;
                 edit.index = static_cast<u8>(raw_index);
                 const Json value = {{"index", raw_index}};
                 if (s.world.HasComponentRaw(entity, layer_component)) {
                     Json warnings = Json::array();
                     edit.after = PatchedBytes(s, entity, layer_component, value, edit.before, warnings);
                     if (!warnings.empty()) throw ToolError("Invalid Layer value for entity " + ToString(guid));
                 } else {
                     const ComponentInfo& info = GetComponentInfo(layer_component);
                     reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                     if (!sample.HasValue()) throw ToolError("Cannot default-construct Layer");
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, sample.Data(), value, &report)) {
                         throw ToolError("Invalid layer index for entity " + ToString(guid));
                     }
                     info.serialize(sample.Data(), edit.after);
                     reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                     if (!defaults.HasValue()) throw ToolError("Cannot default-construct Layer");
                     info.serialize(defaults.Data(), edit.before);
                     edit.add_component = true;
                 }
                 edits.push_back(std::move(edit));
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Set Entity Layers " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const LayerEdit& edit : edits) {
                     if (edit.add_component) {
                         auto add = std::make_unique<editor::AddComponentCommand>(edit.guid, layer_component);
                         add->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(add));
                     }
                     auto set = std::make_unique<SetComponentCommand>(edit.guid, layer_component, edit.before, edit.after);
                     set->SetAttribution("agent");
                     s.stack.Execute(ctx, std::move(set));
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json results = Json::array();
             for (const LayerEdit& edit : edits) results.push_back({{"entity", ToString(edit.guid)}, {"layer_index", edit.index}});
             return {{"updated", edits.size()}, {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"rotate_entities_local_many",
         "Apply a per-entity local-space rotation delta to 1–64 unique scene entities as one undoable action. Each entity must have Transform; position and scale are preserved. Deltas are quaternions [x, y, z, w], composed as current rotation × delta and normalized. Validate the full selection before applying any changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"delta", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                    {"minItems", 4}, {"maxItems", 4}}}}},
                                        {"required", Json::array({"entity", "delta"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and local rotation-delta pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity rotation changes");
             }

             struct RotationEdit {
                 EntityGuid guid;
                 Quaternion rotation;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<RotationEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("delta") || !entry["delta"].is_array() || entry["delta"].size() != 4) {
                     throw ToolError("Each \"delta\" must be [x, y, z, w]");
                 }
                 for (const Json& value : entry["delta"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Rotation deltas must be finite numbers");
                     }
                 }
                 Quaternion delta{entry["delta"][0].get<f32>(), entry["delta"][1].get<f32>(),
                                  entry["delta"][2].get<f32>(), entry["delta"][3].get<f32>()};
                 if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || !std::isfinite(delta.z) || !std::isfinite(delta.w)) {
                     throw ToolError("Rotation delta values must fit finite engine coordinates");
                 }
                 const f32 delta_length_sq = delta.LengthSq();
                 if (!std::isfinite(delta_length_sq) || delta_length_sq <= 1.0e-12f) {
                     throw ToolError("Rotation delta quaternion must be non-zero");
                 }
                 delta = delta.Normalized();
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 const Quaternion& current_rotation = current->rotation;
                 if (!std::isfinite(current_rotation.x) || !std::isfinite(current_rotation.y) ||
                     !std::isfinite(current_rotation.z) || !std::isfinite(current_rotation.w)) {
                     throw ToolError("Entity " + ToString(guid) + " has a non-finite rotation");
                 }
                 const f32 current_length_sq = current_rotation.LengthSq();
                 if (!std::isfinite(current_length_sq) || current_length_sq <= 1.0e-12f) {
                     throw ToolError("Entity " + ToString(guid) + " has a zero rotation quaternion");
                 }
                 RotationEdit edit;
                 edit.guid = guid;
                 edit.rotation = (current_rotation.Normalized() * delta).Normalized();
                 if (!std::isfinite(edit.rotation.x) || !std::isfinite(edit.rotation.y) ||
                     !std::isfinite(edit.rotation.z) || !std::isfinite(edit.rotation.w)) {
                     throw ToolError("Rotation delta would produce a non-finite rotation for entity " + ToString(guid));
                 }
                 Transform updated = *current;
                 updated.rotation = edit.rotation;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Rotate Entities Locally " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const RotationEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)},
                                    {"rotation", {edit.rotation.x, edit.rotation.y, edit.rotation.z, edit.rotation.w}},
                                    {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_local_rotations_many",
         "Set local rotations individually for 1–64 unique scene entities as one undoable action. Each entity must have Transform; position and scale are preserved. Validate each finite, non-zero quaternion and normalize it before applying any changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                       {"minItems", 4}, {"maxItems", 4}}}}},
                                        {"required", Json::array({"entity", "rotation"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and local-quaternion pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity rotation changes");
             }

             struct RotationEdit {
                 EntityGuid guid;
                 Quaternion rotation;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<RotationEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("rotation") || !entry["rotation"].is_array() || entry["rotation"].size() != 4) {
                     throw ToolError("Each \"rotation\" must be [x, y, z, w]");
                 }
                 for (const Json& value : entry["rotation"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Rotation values must be finite numbers");
                     }
                 }
                 Quaternion rotation{entry["rotation"][0].get<f32>(), entry["rotation"][1].get<f32>(),
                                     entry["rotation"][2].get<f32>(), entry["rotation"][3].get<f32>()};
                 if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w)) {
                     throw ToolError("Rotation values must fit finite engine coordinates");
                 }
                 const f32 length_sq = rotation.LengthSq();
                 if (!std::isfinite(length_sq) || length_sq <= 1.0e-12f) throw ToolError("Rotation quaternion must be non-zero");
                 rotation = rotation.Normalized();
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 RotationEdit edit;
                 edit.guid = guid;
                 edit.rotation = rotation;
                 Transform updated = *current;
                 updated.rotation = rotation;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity Rotations " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const RotationEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)},
                                    {"rotation", {edit.rotation.x, edit.rotation.y, edit.rotation.z, edit.rotation.w}},
                                    {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_local_scales_many",
         "Set local scales individually for 1–64 unique scene entities as one undoable action. Each entity must have Transform; position and rotation are preserved. Validate the complete selection and finite scale values before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"scale", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                    {"minItems", 3}, {"maxItems", 3}}}}},
                                        {"required", Json::array({"entity", "scale"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and local-scale pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity scale changes");
             }

             struct ScaleEdit {
                 EntityGuid guid;
                 Vec3 scale;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("scale") || !entry["scale"].is_array() || entry["scale"].size() != 3) {
                     throw ToolError("Each \"scale\" must be [x, y, z]");
                 }
                 for (const Json& value : entry["scale"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Scale values must be finite numbers");
                     }
                 }
                 const Vec3 scale{entry["scale"][0].get<f32>(), entry["scale"][1].get<f32>(), entry["scale"][2].get<f32>()};
                 if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z)) {
                     throw ToolError("Scale values must fit finite engine coordinates");
                 }
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 ScaleEdit edit;
                 edit.guid = guid;
                 edit.scale = scale;
                 Transform updated = *current;
                 updated.scale = scale;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity Scales " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const ScaleEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"scale", {edit.scale.x, edit.scale.y, edit.scale.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_local_rotation_many",
         "Set the local rotation of 1–64 unique scene entities to one shared quaternion [x, y, z, w] as one undoable action. Each entity must have Transform; position and scale are preserved. The quaternion must be finite and non-zero, and is normalized before use.",
         Schema({{"entities", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                               {"description", "1–64 unique entity GUIDs"}}},
                 {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 4}, {"maxItems", 4},
                               {"description", "Shared local rotation quaternion [x, y, z, w]"}}}},
                {"entities", "rotation"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
             }
             if (!args["rotation"].is_array() || args["rotation"].size() != 4) throw ToolError("\"rotation\" must be [x, y, z, w]");
             for (const Json& value : args["rotation"]) {
                 if (!value.is_number() || !std::isfinite(value.get<double>())) throw ToolError("Rotation values must be finite numbers");
             }
             Quaternion rotation{args["rotation"][0].get<f32>(), args["rotation"][1].get<f32>(),
                                 args["rotation"][2].get<f32>(), args["rotation"][3].get<f32>()};
             if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w)) {
                 throw ToolError("Rotation values must fit finite engine coordinates");
             }
             const f32 length_sq = rotation.LengthSq();
             if (!std::isfinite(length_sq) || length_sq <= 1.0e-12f) throw ToolError("Rotation quaternion must be non-zero");
             rotation = rotation.Normalized();

             struct RotationEdit {
                 EntityGuid guid;
                 Quaternion rotation;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<RotationEdit> edits;
             edits.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"entities\" must be an entity GUID string");
                 const std::string text = value.get<std::string>();
                 EntityGuid guid;
                 if (!ParseEntityGuid(text, guid)) throw ToolError("Not a valid entity GUID: " + text);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 RotationEdit edit;
                 edit.guid = guid;
                 edit.rotation = rotation;
                 Transform updated = *current;
                 updated.rotation = rotation;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity Rotations " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const RotationEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)},
                                    {"rotation", {edit.rotation.x, edit.rotation.y, edit.rotation.z, edit.rotation.w}},
                                    {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_world_rotations_many",
         "Set world rotations individually for 1–64 unique scene entities as one undoable action. Each entity must have Transform; position and scale are preserved. World orientations compose normalized local quaternions parent-first; rotations are converted against final parent orientations, including parents changed in this batch. Singular quaternions, cycles and hierarchy chains deeper than the supported limit are rejected. Validate the complete selection before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"rotation", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                       {"minItems", 4}, {"maxItems", 4}}}}},
                                        {"required", Json::array({"entity", "rotation"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and world-quaternion pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity rotation changes");
             }

             struct RotationEdit {
                 EntityGuid guid;
                 Entity entity;
                 Quaternion world_rotation;
                 Quaternion local_rotation;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<RotationEdit> edits;
             edits.reserve(args["edits"].size());
             std::map<EntityGuid, usize> edit_indices;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (edit_indices.contains(guid)) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("rotation") || !entry["rotation"].is_array() || entry["rotation"].size() != 4) {
                     throw ToolError("Each \"rotation\" must be [x, y, z, w]");
                 }
                 for (const Json& value : entry["rotation"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("World-rotation values must be finite numbers");
                     }
                 }
                 Quaternion world_rotation{entry["rotation"][0].get<f32>(), entry["rotation"][1].get<f32>(),
                                          entry["rotation"][2].get<f32>(), entry["rotation"][3].get<f32>()};
                 if (!std::isfinite(world_rotation.x) || !std::isfinite(world_rotation.y) ||
                     !std::isfinite(world_rotation.z) || !std::isfinite(world_rotation.w)) {
                     throw ToolError("World-rotation values must fit finite engine coordinates");
                 }
                 const f32 length_sq = world_rotation.LengthSq();
                 if (!std::isfinite(length_sq) || length_sq <= 1.0e-12f) {
                     throw ToolError("World-rotation quaternion must be non-zero");
                 }
                 world_rotation = world_rotation.Normalized();
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 if (s.world.GetComponent<Transform>(entity) == nullptr) {
                     throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 }
                 edit_indices.emplace(guid, edits.size());
                 RotationEdit edit;
                 edit.guid = guid;
                 edit.entity = entity;
                 edit.world_rotation = world_rotation;
                 edits.push_back(std::move(edit));
             }

             std::function<Quaternion(Entity, int)> compute_final_world_rotation = [&](Entity entity, int depth) -> Quaternion {
                 if (entity.IsNull()) return Quaternion::Identity();
                 if (depth >= kMaxHierarchyDepth) throw ToolError("World-rotation batch encountered a hierarchy cycle or excessive depth");
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) return Quaternion::Identity();
                 Quaternion parent_rotation = Quaternion::Identity();
                 const Entity parent = GetParent(s.world, s.guids, entity);
                 if (!parent.IsNull()) parent_rotation = compute_final_world_rotation(parent, depth + 1);

                 const IdComponent* id = s.world.GetComponent<IdComponent>(entity);
                 if (id != nullptr) {
                     const auto selected = edit_indices.find(id->guid);
                     if (selected != edit_indices.end()) {
                         const Quaternion inverse_parent{-parent_rotation.x, -parent_rotation.y, -parent_rotation.z, parent_rotation.w};
                         RotationEdit& edit = edits[selected->second];
                         edit.local_rotation = (inverse_parent * edit.world_rotation).Normalized();
                         if (!std::isfinite(edit.local_rotation.x) || !std::isfinite(edit.local_rotation.y) ||
                             !std::isfinite(edit.local_rotation.z) || !std::isfinite(edit.local_rotation.w)) {
                             throw ToolError("World rotation would produce a non-finite local rotation for entity " + ToString(id->guid));
                         }
                         return edit.world_rotation;
                     }
                 }

                 const Quaternion& local = current->rotation;
                 if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z) || !std::isfinite(local.w)) {
                     throw ToolError("Ancestor entity has a non-finite rotation");
                 }
                 const f32 local_length_sq = local.LengthSq();
                 if (!std::isfinite(local_length_sq) || local_length_sq <= 1.0e-12f) {
                     throw ToolError("Ancestor entity has a zero rotation quaternion");
                 }
                 return (parent_rotation * local.Normalized()).Normalized();
             };

             for (const RotationEdit& edit : edits) (void)compute_final_world_rotation(edit.entity, 0);
             for (RotationEdit& edit : edits) {
                 const Transform* current = s.world.GetComponent<Transform>(edit.entity);
                 Transform updated = *current;
                 updated.rotation = edit.local_rotation;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity World Rotations " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const RotationEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const RotationEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)},
                                    {"world_rotation", {edit.world_rotation.x, edit.world_rotation.y, edit.world_rotation.z, edit.world_rotation.w}},
                                    {"local_rotation", {edit.local_rotation.x, edit.local_rotation.y, edit.local_rotation.z, edit.local_rotation.w}},
                                    {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const RotationEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_world_position_many",
         "Set 1–64 unique scene entities to one shared world position as one undoable action. Each entity must have Transform; rotation and scale are preserved. Positions are converted through the final parent transforms, including parents changed in this batch. Singular parent transforms, cycles, and hierarchy chains deeper than the supported limit are rejected. Validate the complete selection before applying changes.",
         Schema({{"entities", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                               {"description", "1–64 unique entity GUIDs"}}},
                 {"position", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                               {"description", "Shared world position [x, y, z]"}}}},
                {"entities", "position"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
             }
             if (!args["position"].is_array() || args["position"].size() != 3) throw ToolError("\"position\" must be [x, y, z]");
             for (const Json& value : args["position"]) {
                 if (!value.is_number() || !std::isfinite(value.get<double>())) {
                     throw ToolError("World-position values must be finite numbers");
                 }
             }
             const Vec3 world_position{args["position"][0].get<f32>(), args["position"][1].get<f32>(), args["position"][2].get<f32>()};
             if (!std::isfinite(world_position.x) || !std::isfinite(world_position.y) || !std::isfinite(world_position.z)) {
                 throw ToolError("World-position values must fit finite engine coordinates");
             }

             struct PositionEdit {
                 EntityGuid guid;
                 Entity entity;
                 Vec3 local_position;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<PositionEdit> edits;
             edits.reserve(args["entities"].size());
             std::map<EntityGuid, usize> edit_indices;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"entities\" must be an entity GUID string");
                 const std::string text = value.get<std::string>();
                 EntityGuid guid;
                 if (!ParseEntityGuid(text, guid)) throw ToolError("Not a valid entity GUID: " + text);
                 if (edit_indices.contains(guid)) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 if (s.world.GetComponent<Transform>(entity) == nullptr) {
                     throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 }
                 edit_indices.emplace(guid, edits.size());
                 PositionEdit edit;
                 edit.guid = guid;
                 edit.entity = entity;
                 edits.push_back(std::move(edit));
             }

             std::function<Mat4(Entity, int)> compute_final_world = [&](Entity entity, int depth) -> Mat4 {
                 if (entity.IsNull()) return Mat4::Identity();
                 if (depth >= kMaxHierarchyDepth) throw ToolError("World-position batch encountered a hierarchy cycle or excessive depth");
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) return Mat4::Identity();
                 Mat4 parent_world = Mat4::Identity();
                 const Entity parent = GetParent(s.world, s.guids, entity);
                 if (!parent.IsNull()) parent_world = compute_final_world(parent, depth + 1);

                 Transform updated = *current;
                 const IdComponent* id = s.world.GetComponent<IdComponent>(entity);
                 if (id != nullptr) {
                     const auto selected = edit_indices.find(id->guid);
                     if (selected != edit_indices.end()) {
                         Mat4 inverse_parent;
                         if (!parent_world.TryInverse(inverse_parent)) {
                             throw ToolError("Cannot set world position for entity " + ToString(id->guid) + " because its parent transform is singular");
                         }
                         const Vec4 local_homogeneous = inverse_parent * Vec4(world_position.x, world_position.y, world_position.z, 1.0f);
                         if (!std::isfinite(local_homogeneous.x) || !std::isfinite(local_homogeneous.y) ||
                             !std::isfinite(local_homogeneous.z) || !std::isfinite(local_homogeneous.w) ||
                             std::abs(local_homogeneous.w) <= 1.0e-12f) {
                             throw ToolError("Parent transform cannot produce a finite local position for entity " + ToString(id->guid));
                         }
                         const Vec3 local_position{local_homogeneous.x / local_homogeneous.w,
                                                   local_homogeneous.y / local_homogeneous.w,
                                                   local_homogeneous.z / local_homogeneous.w};
                         if (!std::isfinite(local_position.x) || !std::isfinite(local_position.y) || !std::isfinite(local_position.z)) {
                             throw ToolError("World position would produce a non-finite local position for entity " + ToString(id->guid));
                         }
                         edits[selected->second].local_position = local_position;
                         updated.position = local_position;
                     }
                 }
                 return parent_world * LocalTransformMatrix(updated);
             };

             for (const PositionEdit& edit : edits) (void)compute_final_world(edit.entity, 0);
             for (PositionEdit& edit : edits) {
                 const Transform* current = s.world.GetComponent<Transform>(edit.entity);
                 Transform updated = *current;
                 updated.position = edit.local_position;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity World Position " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const PositionEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)},
                                    {"world_position", {world_position.x, world_position.y, world_position.z}},
                                    {"local_position", {edit.local_position.x, edit.local_position.y, edit.local_position.z}},
                                    {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_world_positions_many",
         "Set world positions individually for 1–64 unique scene entities as one undoable action. Each entity must have Transform; rotation and scale are preserved. Child targets are converted through the live parent world transform into local positions. A singular parent transform or selecting both an entity and one of its ancestors is rejected. Validate the complete selection before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"position", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                       {"minItems", 3}, {"maxItems", 3}}}}},
                                        {"required", Json::array({"entity", "position"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and world-position pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity position changes");
             }

             struct PositionEdit {
                 EntityGuid guid;
                 Entity entity;
                 Vec3 world_position;
                 Vec3 local_position;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<PositionEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("position") || !entry["position"].is_array() || entry["position"].size() != 3) {
                     throw ToolError("Each \"position\" must be [x, y, z]");
                 }
                 for (const Json& value : entry["position"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("World-position values must be finite numbers");
                     }
                 }
                 const Vec3 world_position{entry["position"][0].get<f32>(), entry["position"][1].get<f32>(), entry["position"][2].get<f32>()};
                 if (!std::isfinite(world_position.x) || !std::isfinite(world_position.y) || !std::isfinite(world_position.z)) {
                     throw ToolError("World-position values must fit finite engine coordinates");
                 }
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");

                 Vec3 local_position = world_position;
                 const Entity parent = GetParent(s.world, s.guids, entity);
                 if (!parent.IsNull()) {
                     const Mat4 parent_world = ComputeWorldTransform(s.world, s.guids, parent);
                     Mat4 inverse_parent;
                     if (!parent_world.TryInverse(inverse_parent)) {
                         throw ToolError("Cannot set world position for entity " + ToString(guid) + " because its parent transform is singular");
                     }
                     const Vec4 local_homogeneous = inverse_parent * Vec4(world_position.x, world_position.y, world_position.z, 1.0f);
                     if (!std::isfinite(local_homogeneous.x) || !std::isfinite(local_homogeneous.y) ||
                         !std::isfinite(local_homogeneous.z) || !std::isfinite(local_homogeneous.w) ||
                         std::abs(local_homogeneous.w) <= 1.0e-12f) {
                         throw ToolError("Parent transform cannot produce a finite local position for entity " + ToString(guid));
                     }
                     local_position = {local_homogeneous.x / local_homogeneous.w,
                                       local_homogeneous.y / local_homogeneous.w,
                                       local_homogeneous.z / local_homogeneous.w};
                 }
                 if (!std::isfinite(local_position.x) || !std::isfinite(local_position.y) || !std::isfinite(local_position.z)) {
                     throw ToolError("World position would produce a non-finite local position for entity " + ToString(guid));
                 }

                 PositionEdit edit;
                 edit.guid = guid;
                 edit.entity = entity;
                 edit.world_position = world_position;
                 edit.local_position = local_position;
                 Transform updated = *current;
                 updated.position = local_position;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             for (const PositionEdit& edit : edits) {
                 Entity ancestor = GetParent(s.world, s.guids, edit.entity);
                 for (int depth = 0; depth < kMaxHierarchyDepth && !ancestor.IsNull(); ++depth) {
                     const IdComponent* id = s.world.GetComponent<IdComponent>(ancestor);
                     if (id != nullptr && seen.contains(id->guid)) {
                         throw ToolError("World-position batch cannot include both an entity and one of its ancestors");
                     }
                     ancestor = GetParent(s.world, s.guids, ancestor);
                 }
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity World Positions " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const PositionEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)},
                                    {"world_position", {edit.world_position.x, edit.world_position.y, edit.world_position.z}},
                                    {"local_position", {edit.local_position.x, edit.local_position.y, edit.local_position.z}},
                                    {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_local_positions_many",
         "Set local positions individually for 1–64 unique scene entities as one undoable action. Each entity must have Transform; rotation and scale are preserved. Validate the complete selection and finite position values before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"position", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                       {"minItems", 3}, {"maxItems", 3}}}}},
                                        {"required", Json::array({"entity", "position"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and local-position pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity position changes");
             }

             struct PositionEdit {
                 EntityGuid guid;
                 Vec3 position;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<PositionEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("position") || !entry["position"].is_array() || entry["position"].size() != 3) {
                     throw ToolError("Each \"position\" must be [x, y, z]");
                 }
                 for (const Json& value : entry["position"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Position values must be finite numbers");
                     }
                 }
                 const Vec3 position{entry["position"][0].get<f32>(), entry["position"][1].get<f32>(), entry["position"][2].get<f32>()};
                 if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                     throw ToolError("Position values must fit finite engine coordinates");
                 }
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 PositionEdit edit;
                 edit.guid = guid;
                 edit.position = position;
                 Transform updated = *current;
                 updated.position = position;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity Positions " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const PositionEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"position", {edit.position.x, edit.position.y, edit.position.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_local_position_many",
         "Set the local position of 1–64 unique scene entities to one shared absolute position as one undoable action. Each entity must have Transform; rotation and scale are preserved. Validate every target and finite engine-coordinate value before applying changes.",
         Schema({{"entities", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                               {"description", "1–64 unique entity GUIDs"}}},
                 {"position", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                               {"description", "Shared local-space position [x, y, z]"}}}},
                {"entities", "position"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
             }
             if (!args["position"].is_array() || args["position"].size() != 3) throw ToolError("\"position\" must be [x, y, z]");
             for (const Json& value : args["position"]) {
                 if (!value.is_number() || !std::isfinite(value.get<double>())) throw ToolError("Position values must be finite numbers");
             }
             const Vec3 position{args["position"][0].get<f32>(), args["position"][1].get<f32>(), args["position"][2].get<f32>()};
             if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
                 throw ToolError("Position values must fit finite engine coordinates");
             }

             struct PositionEdit {
                 EntityGuid guid;
                 Vec3 position;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<PositionEdit> edits;
             edits.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"entities\" must be an entity GUID string");
                 const std::string text = value.get<std::string>();
                 EntityGuid guid;
                 if (!ParseEntityGuid(text, guid)) throw ToolError("Not a valid entity GUID: " + text);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 PositionEdit edit;
                 edit.guid = guid;
                 edit.position = position;
                 Transform updated = *current;
                 updated.position = position;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity Positions " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const PositionEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"position", {edit.position.x, edit.position.y, edit.position.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"scale_entities_local_many",
         "Multiply local scale for 1–64 unique scene entities using a per-entity component-wise factor as one undoable action. Each entity must have Transform; position and rotation are preserved. Validate the complete selection and finite resulting scales before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"factor", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                    {"minItems", 3}, {"maxItems", 3}}}}},
                                        {"required", Json::array({"entity", "factor"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and scale-factor pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity scale changes");
             }

             struct ScaleEdit {
                 EntityGuid guid;
                 Vec3 scale;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("factor") || !entry["factor"].is_array() || entry["factor"].size() != 3) {
                     throw ToolError("Each \"factor\" must be [x, y, z]");
                 }
                 for (const Json& value : entry["factor"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Scale factors must be finite numbers");
                     }
                 }
                 const Vec3 factor{entry["factor"][0].get<f32>(), entry["factor"][1].get<f32>(), entry["factor"][2].get<f32>()};
                 if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z)) {
                     throw ToolError("Scale factors must fit finite engine coordinates");
                 }
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 ScaleEdit edit;
                 edit.guid = guid;
                 edit.scale = {current->scale.x * factor.x, current->scale.y * factor.y, current->scale.z * factor.z};
                 if (!std::isfinite(edit.scale.x) || !std::isfinite(edit.scale.y) || !std::isfinite(edit.scale.z)) {
                     throw ToolError("Scale factor would produce a non-finite scale for entity " + ToString(guid));
                 }
                 Transform updated = *current;
                 updated.scale = edit.scale;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Scale Entities " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const ScaleEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"scale", {edit.scale.x, edit.scale.y, edit.scale.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"multiply_entities_local_scale_per_entity_many",
         "Multiply local scale for 1–64 unique scene entities using a per-entity component-wise factor as one undoable action. Each entity must have Transform; position and rotation are preserved. Validate the complete selection and finite resulting scales before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"factor", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                    {"minItems", 3}, {"maxItems", 3}}}}},
                                        {"required", Json::array({"entity", "factor"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and scale-factor pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity scale changes");
             }

             struct ScaleEdit {
                 EntityGuid guid;
                 Vec3 scale;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("factor") || !entry["factor"].is_array() || entry["factor"].size() != 3) {
                     throw ToolError("Each \"factor\" must be [x, y, z]");
                 }
                 for (const Json& value : entry["factor"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Scale factors must be finite numbers");
                     }
                 }
                 const Vec3 factor{entry["factor"][0].get<f32>(), entry["factor"][1].get<f32>(), entry["factor"][2].get<f32>()};
                 if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z)) {
                     throw ToolError("Scale factors must fit finite engine coordinates");
                 }
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 ScaleEdit edit;
                 edit.guid = guid;
                 edit.scale = {current->scale.x * factor.x, current->scale.y * factor.y, current->scale.z * factor.z};
                 if (!std::isfinite(edit.scale.x) || !std::isfinite(edit.scale.y) || !std::isfinite(edit.scale.z)) {
                     throw ToolError("Scale factor would produce a non-finite scale for entity " + ToString(guid));
                 }
                 Transform updated = *current;
                 updated.scale = edit.scale;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Scale Entities by Factors " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const ScaleEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"scale", {edit.scale.x, edit.scale.y, edit.scale.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"multiply_entities_local_scale_many",
         "Multiply the local scale of 1–64 unique scene entities by one shared component-wise factor as one undoable action. Each entity must have Transform; position and rotation are preserved. Validate every target and finite resulting scale before applying changes.",
         Schema({{"entities", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                               {"description", "1–64 unique entity GUIDs"}}},
                 {"factor", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                             {"description", "Shared component-wise scale factor [x, y, z]"}}}},
                {"entities", "factor"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
             }
             if (!args["factor"].is_array() || args["factor"].size() != 3) throw ToolError("\"factor\" must be [x, y, z]");
             for (const Json& value : args["factor"]) {
                 if (!value.is_number() || !std::isfinite(value.get<double>())) throw ToolError("Scale factors must be finite numbers");
             }
             const Vec3 factor{args["factor"][0].get<f32>(), args["factor"][1].get<f32>(), args["factor"][2].get<f32>()};
             if (!std::isfinite(factor.x) || !std::isfinite(factor.y) || !std::isfinite(factor.z)) {
                 throw ToolError("Scale factors must fit finite engine coordinates");
             }

             struct ScaleEdit {
                 EntityGuid guid;
                 Vec3 scale;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<ScaleEdit> edits;
             edits.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"entities\" must be an entity GUID string");
                 const std::string text = value.get<std::string>();
                 EntityGuid guid;
                 if (!ParseEntityGuid(text, guid)) throw ToolError("Not a valid entity GUID: " + text);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 ScaleEdit edit;
                 edit.guid = guid;
                 edit.scale = {current->scale.x * factor.x, current->scale.y * factor.y, current->scale.z * factor.z};
                 if (!std::isfinite(edit.scale.x) || !std::isfinite(edit.scale.y) || !std::isfinite(edit.scale.z)) {
                     throw ToolError("Scale factor would produce a non-finite scale for entity " + ToString(guid));
                 }
                 Transform updated = *current;
                 updated.scale = edit.scale;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Multiply Entity Scales " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const ScaleEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const ScaleEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"scale", {edit.scale.x, edit.scale.y, edit.scale.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const ScaleEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"offset_entities_local_per_entity_many",
         "Apply a local-position offset to each of 1–64 unique scene entities as one undoable action. Each entity must have Transform; rotation and scale are preserved. Validate the complete selection and finite resulting positions before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"offset", {{"type", "array"}, {"items", {{"type", "number"}}},
                                                                    {"minItems", 3}, {"maxItems", 3}}}}},
                                        {"required", Json::array({"entity", "offset"})}}},
                             {"minItems", 1}, {"maxItems", 64},
                             {"description", "1–64 unique entity GUID and local-offset pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity position changes");
             }

             struct PositionEdit {
                 EntityGuid guid;
                 Vec3 position;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<PositionEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("offset") || !entry["offset"].is_array() || entry["offset"].size() != 3) {
                     throw ToolError("Each \"offset\" must be [x, y, z]");
                 }
                 for (const Json& value : entry["offset"]) {
                     if (!value.is_number() || !std::isfinite(value.get<double>())) {
                         throw ToolError("Position offsets must be finite numbers");
                     }
                 }
                 const Vec3 offset{entry["offset"][0].get<f32>(), entry["offset"][1].get<f32>(), entry["offset"][2].get<f32>()};
                 if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
                     throw ToolError("Position offsets must fit finite engine coordinates");
                 }
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 PositionEdit edit;
                 edit.guid = guid;
                 edit.position = current->position + offset;
                 if (!std::isfinite(edit.position.x) || !std::isfinite(edit.position.y) || !std::isfinite(edit.position.z)) {
                     throw ToolError("Position offset would produce a non-finite position for entity " + ToString(guid));
                 }
                 Transform updated = *current;
                 updated.position = edit.position;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             const bool any_changed = std::any_of(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; });
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Offset Entity Positions " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const PositionEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"position", {edit.position.x, edit.position.y, edit.position.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"offset_entities_local_many",
         "Add one local-position offset to 1–64 unique scene entities as one undoable action. Each entity must have Transform; rotation and scale are preserved. Validate every target and resulting position before applying changes.",
         Schema({{"entities", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                               {"description", "1–64 unique entity GUIDs"}}},
                 {"offset", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                             {"description", "Local-space position offset [x, y, z]"}}}},
                {"entities", "offset"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity transforms");
             if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                 throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
             }
             if (!args["offset"].is_array() || args["offset"].size() != 3) throw ToolError("\"offset\" must be [x, y, z]");
             for (const Json& value : args["offset"]) {
                 if (!value.is_number() || !std::isfinite(value.get<double>())) throw ToolError("Offset values must be finite numbers");
             }
             const Vec3 offset{args["offset"][0].get<f32>(), args["offset"][1].get<f32>(), args["offset"][2].get<f32>()};
             if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
                 throw ToolError("Offset values must fit finite engine coordinates");
             }

             struct PositionEdit {
                 EntityGuid guid;
                 Vec3 position;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<PositionEdit> edits;
             edits.reserve(args["entities"].size());
             std::set<EntityGuid> seen;
             const ComponentId transform_component = GetComponentId<Transform>();
             const ComponentInfo& info = GetComponentInfo(transform_component);
             for (const Json& value : args["entities"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"entities\" must be an entity GUID string");
                 const std::string text = value.get<std::string>();
                 EntityGuid guid;
                 if (!ParseEntityGuid(text, guid)) throw ToolError("Not a valid entity GUID: " + text);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"entities\": " + ToString(guid));
                 const Entity entity = s.guids.Find(s.world, guid);
                 if (entity.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
                 const Transform* current = s.world.GetComponent<Transform>(entity);
                 if (current == nullptr) throw ToolError("Entity " + ToString(guid) + " doesn't have Transform");
                 PositionEdit edit;
                 edit.guid = guid;
                 edit.position = current->position + offset;
                 if (!std::isfinite(edit.position.x) || !std::isfinite(edit.position.y) || !std::isfinite(edit.position.z)) {
                     throw ToolError("Offset would produce a non-finite position for entity " + ToString(guid));
                 }
                 Transform updated = *current;
                 updated.position = edit.position;
                 info.serialize(current, edit.before);
                 info.serialize(&updated, edit.after);
                 edit.changed = edit.before != edit.after;
                 edits.push_back(std::move(edit));
             }

             bool any_changed = false;
             for (const PositionEdit& edit : edits) any_changed = any_changed || edit.changed;
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Offset Entity Positions " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const PositionEdit& edit : edits) {
                         if (!edit.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(edit.guid, transform_component, edit.before, edit.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const PositionEdit& edit : edits) {
                 results.push_back({{"entity", ToString(edit.guid)}, {"position", {edit.position.x, edit.position.y, edit.position.z}}, {"changed", edit.changed}});
             }
             return {{"updated", std::count_if(edits.begin(), edits.end(), [](const PositionEdit& edit) { return edit.changed; })},
                     {"entities", std::move(results)}};
         }});

    server.AddTool(
        {"set_entities_active_many",
         "Set the active state of 1–64 unique scene entities as one undoable action. Missing Active components mean active; disabling an entity adds Active{false}. Validate all GUIDs and boolean values before applying changes.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp}, {"active", {{"type", "boolean"}}}}},
                                        {"required", Json::array({"entity", "active"})}}},
                             {"description", "1–64 unique entity GUID and active-state pairs"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before changing entity activity");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity activity changes");
             }

             struct ActiveEdit {
                 EntityGuid guid;
                 bool active;
                 bool add_component = false;
                 bool changed = true;
                 std::vector<u8> before;
                 std::vector<u8> after;
             };
             std::vector<ActiveEdit> edits;
             edits.reserve(args["edits"].size());
             std::set<EntityGuid> seen;
             const ComponentId active_component = GetComponentId<Active>();
             usize change_count = 0;
             for (const Json& entry : args["edits"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 if (!seen.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!entry.contains("active") || !entry["active"].is_boolean()) throw ToolError("\"active\" must be true or false");
                 ActiveEdit edit;
                 edit.guid = guid;
                 edit.active = entry["active"].get<bool>();
                 if (s.world.HasComponentRaw(entity, active_component)) {
                     Json warnings = Json::array();
                     edit.after = PatchedBytes(s, entity, active_component, Json{{"active", edit.active}}, edit.before, warnings);
                     const Active* current = s.world.GetComponent<Active>(entity);
                     edit.changed = current == nullptr || current->active != edit.active;
                 } else if (!edit.active) {
                     const ComponentInfo& info = GetComponentInfo(active_component);
                     reflect::Any sample = reflect::Any::DefaultOf(*info.reflected);
                     if (!sample.HasValue()) throw ToolError("Cannot default-construct Active");
                     reflect::LoadReport report;
                     if (!reflect::FromJson(*info.reflected, sample.Data(), Json{{"active", false}}, &report)) {
                         throw ToolError("Invalid active state");
                     }
                     info.serialize(sample.Data(), edit.after);
                     reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                     if (!defaults.HasValue()) throw ToolError("Cannot default-construct Active");
                     info.serialize(defaults.Data(), edit.before);
                     edit.add_component = true;
                 } else {
                     edit.changed = false; // no component means active by default
                 }
                 if (edit.changed) ++change_count;
                 edits.push_back(std::move(edit));
             }

             if (change_count > 0) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Set Entity Activity " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const ActiveEdit& edit : edits) {
                         if (!edit.changed) continue;
                         if (edit.add_component) {
                             auto add = std::make_unique<editor::AddComponentCommand>(edit.guid, active_component);
                             add->SetAttribution("agent");
                             s.stack.Execute(ctx, std::move(add));
                         }
                         auto set = std::make_unique<SetComponentCommand>(edit.guid, active_component, edit.before, edit.after);
                         set->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(set));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const ActiveEdit& edit : edits) results.push_back({{"entity", ToString(edit.guid)}, {"active", edit.active}});
             return {{"changed", change_count}, {"entities", std::move(results)}};
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

    server.AddTool(
        {"reset_components_many",
         "Reset up to 64 unique entity/component pairs to their reflected defaults as one undoable action. Components stay attached, and the entire selection is validated before changes are applied.",
         Schema({{"resets", {{"type", "array"},
                              {"items", {{"type", "object"},
                                         {"properties", {{"entity", kGuidProp},
                                                         {"component", {{"type", "string"}}}}},
                                         {"required", Json::array({"entity", "component"})}}},
                              {"description", "1–64 unique entity/component pairs to reset"}}}},
                {"resets"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before resetting components");
             if (!args["resets"].is_array() || args["resets"].empty() || args["resets"].size() > 64) {
                 throw ToolError("\"resets\" must be a non-empty array of at most 64 component resets");
             }

             struct Reset {
                 EntityGuid entity;
                 ComponentId component;
                 std::string component_name;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<Reset> resets;
             resets.reserve(args["resets"].size());
             std::set<std::pair<EntityGuid, ComponentId>> seen;
             const ComponentId id_component = GetComponentId<IdComponent>();
             for (const Json& entry : args["resets"]) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"resets\" must be an object");
                 const EntityGuid guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry);
                 const ComponentId id = RequireComponent(RequireString(entry, "component"));
                 if (id == id_component) throw ToolError("An entity's identity can't be reset");
                 if (!s.world.HasComponentRaw(entity, id)) throw ToolError("Entity " + ToString(guid) + " doesn't have " + ComponentName(id));
                 if (!seen.emplace(guid, id).second) throw ToolError("Duplicate component reset for entity " + ToString(guid) + " and " + ComponentName(id));

                 const ComponentInfo& info = GetComponentInfo(id);
                 reflect::Any defaults = reflect::Any::DefaultOf(*info.reflected);
                 if (!defaults.HasValue()) throw ToolError("Cannot default-construct component " + ComponentName(id));
                 Reset reset;
                 reset.entity = guid;
                 reset.component = id;
                 reset.component_name = ComponentName(id);
                 info.serialize(s.world.GetComponentRaw(entity, id), reset.before);
                 info.serialize(defaults.Data(), reset.after);
                 reset.changed = reset.before != reset.after;
                 resets.push_back(std::move(reset));
             }

             bool any_changed = false;
             for (const Reset& reset : resets) any_changed = any_changed || reset.changed;
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Reset Components " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const Reset& reset : resets) {
                         if (!reset.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(reset.entity, reset.component, reset.before, reset.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const Reset& reset : resets) results.push_back({{"entity", ToString(reset.entity)}, {"component", reset.component_name}, {"changed", reset.changed}});
             return {{"reset", std::count_if(resets.begin(), resets.end(), [](const Reset& reset) { return reset.changed; })},
                     {"components", std::move(results)}};
         }});

    server.AddTool(
        {"copy_components_many",
         "Copy selected reflected component values from one source entity to 1–64 target entities as one undoable action. Source and target components must already exist; validates every selected pair before applying any changes.",
         Schema({{"source", kGuidProp},
                 {"targets", {{"type", "array"}, {"items", kGuidProp}, {"minItems", 1}, {"maxItems", 64},
                              {"description", "Unique target entity GUIDs"}}},
                 {"components", {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}, {"maxItems", 64},
                                 {"description", "Unique reflected component names to copy from the source"}}}},
                {"source", "targets", "components"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before copying components");
             const Entity source = RequireEntity(s, args, "source");
             if (!args["targets"].is_array() || args["targets"].empty() || args["targets"].size() > 64) {
                 throw ToolError("\"targets\" must be a non-empty array of at most 64 entity GUIDs");
             }
             if (!args["components"].is_array() || args["components"].empty() || args["components"].size() > 64) {
                 throw ToolError("\"components\" must be a non-empty array of at most 64 component names");
             }
             if (args["targets"].size() * args["components"].size() > 64) {
                 throw ToolError("The target/component selection may contain at most 64 pairs");
             }

             struct Copy {
                 EntityGuid target;
                 ComponentId component;
                 std::string component_name;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 bool changed = false;
             };
             std::vector<Copy> copies;
             copies.reserve(args["targets"].size() * args["components"].size());
             std::vector<std::pair<EntityGuid, Entity>> targets;
             targets.reserve(args["targets"].size());
             std::set<EntityGuid> seen_targets;
             for (const Json& value : args["targets"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"targets\" must be an entity GUID string");
                 EntityGuid guid;
                 const std::string text = value.get<std::string>();
                 if (!ParseEntityGuid(text, guid)) throw ToolError("Not a valid entity GUID: " + text);
                 if (!seen_targets.insert(guid).second) throw ToolError("Duplicate target entity: " + ToString(guid));
                 targets.emplace_back(guid, RequireEntity(s, Json{{"entity", ToString(guid)}}));
             }

             std::vector<std::pair<ComponentId, std::string>> components;
             components.reserve(args["components"].size());
             std::set<ComponentId> seen_components;
             const ComponentId id_component = GetComponentId<IdComponent>();
             for (const Json& value : args["components"]) {
                 if (!value.is_string()) throw ToolError("Every entry in \"components\" must be a component name");
                 const ComponentId id = RequireComponent(value.get<std::string>());
                 if (id == id_component) throw ToolError("An entity's identity cannot be copied");
                 if (!seen_components.insert(id).second) throw ToolError("Duplicate component in \"components\": " + ComponentName(id));
                 if (!s.world.HasComponentRaw(source, id)) throw ToolError("Source entity doesn't have " + ComponentName(id));
                 components.emplace_back(id, ComponentName(id));
             }

             for (const auto& [target_guid, target] : targets) {
                 for (const auto& [id, name] : components) {
                     if (!s.world.HasComponentRaw(target, id)) {
                         throw ToolError("Target entity " + ToString(target_guid) + " doesn't have " + name + "; use add_components_many first");
                     }
                     const ComponentInfo& info = GetComponentInfo(id);
                     Copy copy;
                     copy.target = target_guid;
                     copy.component = id;
                     copy.component_name = name;
                     info.serialize(s.world.GetComponentRaw(target, id), copy.before);
                     info.serialize(s.world.GetComponentRaw(source, id), copy.after);
                     copy.changed = copy.before != copy.after;
                     copies.push_back(std::move(copy));
                 }
             }

             bool any_changed = false;
             for (const Copy& copy : copies) any_changed = any_changed || copy.changed;
             if (any_changed) {
                 static std::atomic<std::uint64_t> next_batch_id{0};
                 const std::string transaction_label = "Copy Components " + std::to_string(++next_batch_id);
                 const usize undo_count_before = s.stack.UndoCount();
                 editor::CommandContext ctx = s.Context();
                 s.stack.BeginTransaction(transaction_label);
                 try {
                     for (const Copy& copy : copies) {
                         if (!copy.changed) continue;
                         auto command = std::make_unique<SetComponentCommand>(copy.target, copy.component, copy.before, copy.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                     s.stack.EndTransaction();
                 } catch (...) {
                     s.stack.EndTransaction();
                     if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                     throw;
                 }
             }

             Json results = Json::array();
             for (const Copy& copy : copies) {
                 results.push_back({{"entity", ToString(copy.target)}, {"component", copy.component_name}, {"changed", copy.changed}});
             }
             return {{"copied", std::count_if(copies.begin(), copies.end(), [](const Copy& copy) { return copy.changed; })},
                     {"components", std::move(results)}};
         }});

    server.AddTool(
        {"set_components_many",
         "Update components on 1–64 unique entities as one undoable action. Each edit maps component names to partial field objects; the complete selection and every component patch are validated before any changes are applied.",
         Schema({{"edits", {{"type", "array"},
                             {"items", {{"type", "object"},
                                        {"properties", {{"entity", kGuidProp},
                                                        {"components", {{"type", "object"},
                                                                         {"description", "Component names mapped to partial field objects"}}}}},
                                        {"required", Json::array({"entity", "components"})}}},
                             {"description", "1–64 unique entities and their component patches"}}}},
                {"edits"}),
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before editing components");
             if (!args["edits"].is_array() || args["edits"].empty() || args["edits"].size() > 64) {
                 throw ToolError("\"edits\" must be a non-empty array of at most 64 entity edits");
             }

             struct ComponentPatch {
                 ComponentId id;
                 std::vector<u8> before;
                 std::vector<u8> after;
                 Json warnings = Json::array();
             };
             struct EntityPatch {
                 EntityGuid guid;
                 std::vector<ComponentPatch> components;
             };
             std::vector<EntityPatch> patches;
             patches.reserve(args["edits"].size());
             std::set<EntityGuid> seen_entities;
             const ComponentId id_component = GetComponentId<IdComponent>();
             for (const Json& edit : args["edits"]) {
                 if (!edit.is_object()) throw ToolError("Each entry in \"edits\" must be an object");
                 const EntityGuid guid = RequireGuid(edit, "entity");
                 const Entity entity = RequireEntity(s, edit);
                 if (!seen_entities.insert(guid).second) throw ToolError("Duplicate entity in \"edits\": " + ToString(guid));
                 if (!edit.contains("components") || !edit["components"].is_object() || edit["components"].empty()) {
                     throw ToolError("Each edit must include a non-empty \"components\" object");
                 }

                 EntityPatch entity_patch;
                 entity_patch.guid = guid;
                 entity_patch.components.reserve(edit["components"].size());
                 for (const auto& [name, value] : edit["components"].items()) {
                     if (!value.is_object()) throw ToolError("Each component patch must be a field object");
                     const ComponentId id = RequireComponent(name);
                     if (id == id_component) throw ToolError("An entity's identity is read-only");
                     if (!s.world.HasComponentRaw(entity, id)) {
                         throw ToolError("Entity " + ToString(guid) + " doesn't have " + ComponentName(id) + "; use add_component");
                     }
                     ComponentPatch component_patch;
                     component_patch.id = id;
                     component_patch.after = PatchedBytes(s, entity, id, value, component_patch.before, component_patch.warnings);
                     entity_patch.components.push_back(std::move(component_patch));
                 }
                 patches.push_back(std::move(entity_patch));
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Set Components " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 for (const EntityPatch& entity_patch : patches) {
                     for (const ComponentPatch& component_patch : entity_patch.components) {
                         auto command = std::make_unique<SetComponentCommand>(entity_patch.guid, component_patch.id,
                                                                              component_patch.before, component_patch.after);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                     }
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json results = Json::array();
             for (const EntityPatch& entity_patch : patches) {
                 Json components = Json::object();
                 Json warnings = Json::object();
                 const Entity entity = s.guids.Find(s.world, entity_patch.guid);
                 for (const ComponentPatch& component_patch : entity_patch.components) {
                     const std::string name = ComponentName(component_patch.id);
                     components[name] = ComponentJson(s, entity, component_patch.id);
                     if (!component_patch.warnings.empty()) warnings[name] = component_patch.warnings;
                 }
                 Json row = {{"entity", ToString(entity_patch.guid)}, {"components", std::move(components)}};
                 if (!warnings.empty()) row["warnings"] = std::move(warnings);
                 results.push_back(std::move(row));
             }
             return {{"updated", patches.size()}, {"edits", std::move(results)}};
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

    Json reparent_many_schema = Schema(Json::object());
    reparent_many_schema["properties"]["moves"] = {
        {"type", "array"},
        {"items", {{"type", "object"},
                   {"properties", {{"entity", kGuidProp}, {"parent", {{"type", Json::array({"string", "null"})}}}}},
                   {"required", Json::array({"entity"})}}},
        {"description", "1–64 per-entity moves; omitted or null parent detaches that entity"}};
    reparent_many_schema["properties"]["entities"] = {
        {"type", "array"}, {"items", {{"type", "string"}}},
        {"description", "1–64 entity GUIDs to reparent together; use instead of moves"}};
    reparent_many_schema["properties"]["parent"] = {
        {"type", Json::array({"string", "null"})},
        {"description", "Shared parent for entities; omit or set null to detach all"}};

    server.AddTool(
        {"reparent_many",
         "Reparent or detach up to 64 unique entities as one undoable action. Use `moves` for a different parent per entity, or `entities` with a shared optional `parent` for a selection; omit or set the shared parent to null to detach all. The final hierarchy is checked for cycles before changes are applied.",
         reparent_many_schema,
         [&s](const Json& args) -> Json {
             if (s.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before reparenting entities");
             const bool has_moves = args.contains("moves");
             const bool has_entities = args.contains("entities");
             if (has_moves == has_entities) throw ToolError("Specify either \"moves\" or \"entities\" with a shared \"parent\"");
             Json requested_moves = args.value("moves", Json::array());
             if (has_entities) {
                 if (!args["entities"].is_array() || args["entities"].empty() || args["entities"].size() > 64) {
                     throw ToolError("\"entities\" must be a non-empty array of at most 64 entity GUIDs");
                 }
                 Json shared_parent = args.value("parent", Json(nullptr));
                 if (!shared_parent.is_null() && !shared_parent.is_string()) {
                     throw ToolError("\"parent\" must be an entity GUID string or null");
                 }
                 requested_moves = Json::array();
                 for (const Json& entity : args["entities"]) {
                     requested_moves.push_back({{"entity", entity}, {"parent", shared_parent}});
                 }
             } else if (args.contains("parent")) {
                 throw ToolError("Use the per-entity \"parent\" inside each move, or use \"entities\" with a shared parent");
             }
             if (!requested_moves.is_array() || requested_moves.empty() || requested_moves.size() > 64) {
                 throw ToolError("\"moves\" must be a non-empty array of at most 64 entity/parent pairs");
             }

             struct Move {
                 EntityGuid entity;
                 EntityGuid old_parent;
                 EntityGuid parent;
             };
             std::vector<Move> moves;
             moves.reserve(requested_moves.size());
             std::set<EntityGuid> seen;
             for (const Json& entry : requested_moves) {
                 if (!entry.is_object()) throw ToolError("Each entry in \"moves\" must be an object");
                 const EntityGuid entity_guid = RequireGuid(entry, "entity");
                 const Entity entity = RequireEntity(s, entry, "entity");
                 if (!seen.insert(entity_guid).second) throw ToolError("Duplicate entity in \"moves\": " + ToString(entity_guid));
                 EntityGuid parent_guid;
                 if (entry.contains("parent") && !entry["parent"].is_null()) {
                     parent_guid = RequireGuid(entry, "parent");
                     RequireEntity(s, entry, "parent");
                 }
                 const Parent* current = s.world.GetComponent<Parent>(entity);
                 const EntityGuid old_parent = current == nullptr ? EntityGuid{} : current->parent;
                 moves.push_back({entity_guid, old_parent, parent_guid});
             }

             auto DesiredParent = [&](const EntityGuid& guid) -> EntityGuid {
                 for (const Move& move : moves) if (move.entity == guid) return move.parent;
                 const Entity entity = s.guids.Find(s.world, guid);
                 const Parent* parent = entity.IsNull() ? nullptr : s.world.GetComponent<Parent>(entity);
                 return parent == nullptr ? EntityGuid{} : parent->parent;
             };
             for (const Move& move : moves) {
                 std::set<EntityGuid> visited;
                 EntityGuid cursor = move.entity;
                 while (!cursor.IsNull()) {
                     if (!visited.insert(cursor).second) throw ToolError("The requested moves would create a hierarchy cycle");
                     cursor = DesiredParent(cursor);
                 }
             }

             static std::atomic<std::uint64_t> next_batch_id{0};
             const std::string transaction_label = "Reparent Entities " + std::to_string(++next_batch_id);
             const usize undo_count_before = s.stack.UndoCount();
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction(transaction_label);
             try {
                 // Detach changed entities first. This permits valid batch rotations without
                 // transient cycles, then attach children after their moved parents.
                 for (const Move& move : moves) {
                     if (move.old_parent.IsNull() || move.old_parent == move.parent) continue;
                     auto command = std::make_unique<editor::ReparentCommand>(move.entity, EntityGuid{});
                     command->SetAttribution("agent");
                     s.stack.Execute(ctx, std::move(command));
                 }

                 std::set<EntityGuid> attached;
                 for (const Move& move : moves) {
                     if (move.parent.IsNull() || move.parent == move.old_parent) attached.insert(move.entity);
                 }
                 std::size_t remaining = 0;
                 for (const Move& move : moves) {
                     if (!move.parent.IsNull() && move.parent != move.old_parent) ++remaining;
                 }
                 while (remaining > 0) {
                     bool progressed = false;
                     for (const Move& move : moves) {
                         if (move.parent.IsNull() || move.parent == move.old_parent || attached.count(move.entity)) continue;
                         const bool parent_is_moved = seen.count(move.parent) != 0;
                         if (parent_is_moved && !attached.count(move.parent)) continue;
                         auto command = std::make_unique<editor::ReparentCommand>(move.entity, move.parent);
                         command->SetAttribution("agent");
                         s.stack.Execute(ctx, std::move(command));
                         attached.insert(move.entity);
                         --remaining;
                         progressed = true;
                     }
                     if (!progressed) throw ToolError("Could not order the requested hierarchy changes");
                 }
                 s.stack.EndTransaction();
             } catch (...) {
                 s.stack.EndTransaction();
                 if (s.stack.UndoCount() != undo_count_before || s.stack.UndoLabel() == transaction_label) s.stack.Undo(ctx);
                 throw;
             }

             Json results = Json::array();
             for (const Move& move : moves) {
                 results.push_back({{"entity", ToString(move.entity)},
                                    {"parent", move.parent.IsNull() ? Json(nullptr) : Json(ToString(move.parent))}});
             }
             return {{"reparented", moves.size()}, {"results", std::move(results)}};
         }});

    server.AddTool({"undo", "Undo the last editor action.", Schema(Json::object()), [&s](const Json&) -> Json {
                        std::string label = s.stack.UndoLabel();
                        std::string attribution = s.stack.UndoAttribution();
                        editor::CommandContext ctx = s.Context();
                        if (!s.stack.Undo(ctx)) {
                            throw ToolError("Nothing to undo");
                        }
                        return {{"undone", label}, {"attribution", attribution}};
                    }});
    server.AddTool({"redo", "Redo the last undone editor action.", Schema(Json::object()), [&s](const Json&) -> Json {
                        std::string label = s.stack.RedoLabel();
                        std::string attribution = s.stack.RedoAttribution();
                        editor::CommandContext ctx = s.Context();
                        if (!s.stack.Redo(ctx)) {
                            throw ToolError("Nothing to redo");
                        }
                        return {{"redone", label}, {"attribution", attribution}};
                    }});

    server.AddTool({"save_scene", "Save the scene to a file: JSON (.json, human-readable) or binary (.aesc).",
                    Schema({{"path", {{"type", "string"}}}}, {"path"}), [&s](const Json& args) -> Json {
                        std::string error;
                        const std::string path = ResolveScenePath(s.scene_root, RequireString(args, "path"), error).string();
                        if (path.empty()) throw ToolError(error);
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
                        std::string error;
                        const std::string path = ResolveScenePath(s.scene_root, RequireString(args, "path"), error).string();
                        if (path.empty()) throw ToolError(error);
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
    RegisterParticleEditorTools(server, s);
    RegisterNavEditorTools(server, s);
    RegisterSequenceEditorTools(server, s);
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


// ---------------------------------------------------------------------------
// Particle systems. The ParticleSystem component plays a particle effect (.avfx) at an entity; it is saved in scenes
// and prefabs. particle_set edits it as one validated, undoable step.
// ---------------------------------------------------------------------------

namespace {

void ValidateParticleFields(const Json& fields) {
    auto number = [&](const std::string& key, const Json& v, double lo, double hi) {
        if (!v.is_number() || !(v.get<double>() >= lo) || !(v.get<double>() <= hi)) {
            throw ToolError("\"" + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
        }
    };
    for (auto& [key, value] : fields.items()) {
        if (key == "active" || key == "commands") throw ToolError("\"" + key + "\" is runtime state, not saved");
        if (key == "asset") { if (!value.is_string()) throw ToolError("\"asset\" must be an .avfx path (a string)"); }
        else if (key == "auto_activate" || key == "destroy_when_finished") { if (!value.is_boolean()) throw ToolError("\"" + key + "\" must be true or false"); }
        else if (key == "time_scale") { number(key, value, 0.0, 100.0); }
        else if (key == "cull_distance" || key == "lod_distance") number(key, value, 0.0, 1.0e6);
        else if (key == "lod_spawn_scale") number(key, value, 0.0, 1.0);
        else throw ToolError("Unknown ParticleSystem field \"" + key + "\" (asset, auto_activate, time_scale, cull_distance, lod_distance, lod_spawn_scale, destroy_when_finished)");
    }
}

} // namespace

void RegisterParticleEditorTools(McpServer& server, EditorSession& s) {
    const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID"}};

    server.AddTool({"particle_list", "Entities in the editor scene with a ParticleSystem component, and its settings.", Schema(Json::object()),
                    [&s](const Json&) -> Json {
                        const ComponentId id = FindComponentIdByName("ParticleSystem");
                        Json out = Json::array();
                        if (id == kInvalidComponentId) return out;
                        for (Entity e : AllEntities(s)) {
                            if (!s.world.HasComponentRaw(e, id)) continue;
                            const IdComponent* idc = s.world.GetComponent<IdComponent>(e);
                            out.push_back({{"entity", idc ? ToString(idc->guid) : ""}, {"particle_system", ComponentJson(s, e, id)}});
                        }
                        return out;
                    }});

    server.AddTool(
        {"particle_set",
         "Make an entity play a particle effect, or change how: adds a ParticleSystem if it has none and applies the fields (a partial update), as one "
         "undoable step. Fields: asset (an .avfx path under Content/, e.g. VFX/Sparks.avfx), auto_activate, time_scale, cull_distance and lod_distance "
         "(0 = never), lod_spawn_scale (0..1, spawn share beyond lod_distance), destroy_when_finished (one-shots).",
         Schema({{"entity", kGuidProp}, {"fields", {{"type", "object"}, {"description", "e.g. {\"asset\": \"VFX/Sparks.avfx\", \"destroy_when_finished\": true}"}}}}, {"entity", "fields"}),
         [&s](const Json& args) -> Json {
             Entity e = RequireEntity(s, args);
             EntityGuid guid = RequireGuid(args, "entity");
             if (!args["fields"].is_object()) throw ToolError("\"fields\" must be an object");
             ValidateParticleFields(args["fields"]);
             const ComponentId id = RequireComponent("ParticleSystem");
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction("Set ParticleSystem");
             if (!s.world.HasComponentRaw(e, id)) s.stack.Execute(ctx, std::make_unique<editor::AddComponentCommand>(guid, id));
             Json warnings = Json::array();
             if (!args["fields"].empty()) {
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
             }
             s.stack.EndTransaction();
             Json out = {{"particle_system", ComponentJson(s, e, id)}};
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool({"particle_remove", "Remove an entity's ParticleSystem component. Undoable.", Schema({{"entity", kGuidProp}}, {"entity"}),
                    [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        const ComponentId id = RequireComponent("ParticleSystem");
                        if (!s.world.HasComponentRaw(e, id)) throw ToolError("Entity has no ParticleSystem");
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::RemoveComponentCommand>(guid, id));
                        return {{"removed", true}};
                    }});
}


// ---------------------------------------------------------------------------
// Navigation components. NavAgent (walks the mesh), NavObstacle (carves a hole), NavModifierVolume (relabels the ground)
// and NavLinkProxy (an off-mesh link) are saved in scenes and prefabs; nav_component_set edits them as one validated,
// undoable step.
// ---------------------------------------------------------------------------

namespace {

bool IsNavComponent(const std::string& name) { return name == "NavAgent" || name == "NavObstacle" || name == "NavModifierVolume" || name == "NavLinkProxy"; }

void ValidateNavFields(const std::string& component, const Json& fields) {
    auto number = [&](const std::string& key, const Json& v, double lo, double hi) {
        if (!v.is_number() || !(v.get<double>() >= lo) || !(v.get<double>() <= hi)) throw ToolError("\"" + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
    };
    auto integer = [&](const std::string& key, const Json& v, long long lo, long long hi) {
        if (!v.is_number_integer() || v.get<long long>() < lo || v.get<long long>() > hi) throw ToolError("\"" + key + "\" must be an integer from " + std::to_string(lo) + " to " + std::to_string(hi));
    };
    auto boolean = [&](const std::string& key, const Json& v) { if (!v.is_boolean()) throw ToolError("\"" + key + "\" must be true or false"); };
    auto vec = [&](const std::string& key, const Json& v, bool positive) {
        if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) throw ToolError("\"" + key + "\" must be [x, y, z]");
        if (positive && !(v[0].get<double>() > 0 && v[1].get<double>() > 0 && v[2].get<double>() > 0)) throw ToolError("\"" + key + "\" must be above 0 on every axis");
    };
    for (auto& [key, value] : fields.items()) {
        if (component == "NavAgent") {
            if (key == "status" || key == "goal" || key == "velocity" || key == "remaining" || key == "commands") throw ToolError("\"" + key + "\" is runtime state, not saved");
            if (key == "radius") number(key, value, 0.05, 4.0);
            else if (key == "height") number(key, value, 0.1, 20.0);
            else if (key == "max_speed") number(key, value, 0.0, 100.0);
            else if (key == "max_acceleration") number(key, value, 0.0, 1000.0);
            else if (key == "stopping_distance" || key == "goal_tolerance") number(key, value, 0.0, 100.0);
            else if (key == "base_offset") number(key, value, -10.0, 10.0);
            else if (key == "avoidance_quality") integer(key, value, 0, 3);
            else if (key == "separation_weight") number(key, value, 0.0, 20.0);
            else if (key == "turn_speed") number(key, value, 0.0, 10000.0);
            else if (key == "update_rotation" || key == "allow_partial") boolean(key, value);
            else throw ToolError("Unknown NavAgent field \"" + key + "\"");
        } else if (component == "NavObstacle") {
            if (key == "shape") { if (!value.is_string() || (value != "Box" && value != "Cylinder")) throw ToolError("\"shape\" must be Box or Cylinder"); }
            else if (key == "center") vec(key, value, false);
            else if (key == "half_extents") vec(key, value, true);
            else if (key == "radius" || key == "height") number(key, value, 0.0, 1000.0);
            else if (key == "move_threshold") number(key, value, 0.0, 100.0);
            else if (key == "carve") boolean(key, value);
            else throw ToolError("Unknown NavObstacle field \"" + key + "\"");
        } else if (component == "NavModifierVolume") {
            if (key == "half_extents") vec(key, value, true);
            else if (key == "area") integer(key, value, 0, 63);
            else throw ToolError("Unknown NavModifierVolume field \"" + key + "\" (half_extents, area)");
        } else {
            if (key == "start" || key == "end") vec(key, value, false);
            else if (key == "radius") number(key, value, 0.0, 100.0);
            else if (key == "area") integer(key, value, 1, 63);
            else if (key == "user_id") integer(key, value, -2147483647, 2147483647);
            else if (key == "bidirectional" || key == "enabled") boolean(key, value);
            else throw ToolError("Unknown NavLinkProxy field \"" + key + "\"");
        }
    }
}

} // namespace

void RegisterNavEditorTools(McpServer& server, EditorSession& s) {
    const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID"}};

    server.AddTool({"nav_component_list", "Entities in the editor scene with a NavAgent, NavObstacle, NavModifierVolume or NavLinkProxy, and their settings.", Schema(Json::object()),
                    [&s](const Json&) -> Json {
                        Json out = Json::array();
                        for (Entity e : AllEntities(s)) {
                            Json row = Json::object();
                            for (const char* name : {"NavAgent", "NavObstacle", "NavModifierVolume", "NavLinkProxy"}) {
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
        {"nav_component_set",
         "Add a navigation component to an entity if it has none and apply fields (a partial update), as one undoable step. NavAgent: radius, height, max_speed, "
         "max_acceleration, stopping_distance, base_offset, avoidance_quality (0-3), separation_weight, update_rotation, turn_speed, allow_partial, goal_tolerance. "
         "NavObstacle: shape (Box | Cylinder), center, half_extents, radius, height, move_threshold, carve. NavModifierVolume: half_extents, area (0 blocks, 63 ground). "
         "NavLinkProxy: start, end (in the entity's space), radius, bidirectional, area, user_id, enabled.",
         Schema({{"entity", kGuidProp},
                 {"component", {{"type", "string"}, {"description", "NavAgent, NavObstacle, NavModifierVolume or NavLinkProxy"}}},
                 {"fields", {{"type", "object"}}}},
                {"entity", "component"}),
         [&s](const Json& args) -> Json {
             Entity e = RequireEntity(s, args);
             EntityGuid guid = RequireGuid(args, "entity");
             const std::string name = RequireString(args, "component");
             if (!IsNavComponent(name)) throw ToolError("\"component\" must be NavAgent, NavObstacle, NavModifierVolume or NavLinkProxy");
             const Json fields = args.contains("fields") ? args["fields"] : Json::object();
             if (!fields.is_object()) throw ToolError("\"fields\" must be an object");
             ValidateNavFields(name, fields);
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

    server.AddTool({"nav_component_remove", "Remove a NavAgent, NavObstacle, NavModifierVolume or NavLinkProxy from an entity. Undoable.",
                    Schema({{"entity", kGuidProp}, {"component", {{"type", "string"}}}}, {"entity", "component"}), [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        const std::string name = RequireString(args, "component");
                        if (!IsNavComponent(name)) throw ToolError("\"component\" must be NavAgent, NavObstacle, NavModifierVolume or NavLinkProxy");
                        const ComponentId id = RequireComponent(name);
                        if (!s.world.HasComponentRaw(e, id)) throw ToolError("Entity has no " + name);
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::RemoveComponentCommand>(guid, id));
                        return {{"removed", name}};
                    }});
}


// ---------------------------------------------------------------------------
// Sequences. The SequenceComponent plays a level sequence (.asequence) from an entity; it is saved in scenes and
// prefabs. sequence_component_set edits it as one validated, undoable step.
// ---------------------------------------------------------------------------

namespace {

void ValidateSequenceFields(const Json& fields) {
    for (auto& [key, value] : fields.items()) {
        if (key == "time" || key == "playing" || key == "commands") throw ToolError("\"" + key + "\" is runtime state, not saved; use game_sequence_control in a running game");
        if (key == "sequence") { if (!value.is_string()) throw ToolError("\"sequence\" must be a sequence asset path (a string)"); }
        else if (key == "auto_play" || key == "loop" || key == "destroy_when_finished") { if (!value.is_boolean()) throw ToolError("\"" + key + "\" must be true or false"); }
        else if (key == "rate") { if (!value.is_number() || std::fabs(value.get<double>()) > 100.0) throw ToolError("\"rate\" must be a number from -100 to 100 (negative plays backward)"); }
        else throw ToolError("Unknown SequenceComponent field \"" + key + "\" (sequence, auto_play, loop, rate, destroy_when_finished)");
    }
}

} // namespace

void RegisterSequenceEditorTools(McpServer& server, EditorSession& s) {
    const Json kGuidProp = {{"type", "string"}, {"description", "Entity GUID"}};

    server.AddTool({"sequence_component_list", "Entities in the editor scene with a SequenceComponent, and its settings.", Schema(Json::object()), [&s](const Json&) -> Json {
                        const ComponentId id = FindComponentIdByName("SequenceComponent");
                        Json out = Json::array();
                        if (id == kInvalidComponentId) return out;
                        for (Entity e : AllEntities(s)) {
                            if (!s.world.HasComponentRaw(e, id)) continue;
                            const IdComponent* idc = s.world.GetComponent<IdComponent>(e);
                            out.push_back({{"entity", idc ? ToString(idc->guid) : ""}, {"sequence_component", ComponentJson(s, e, id)}});
                        }
                        return out;
                    }});

    server.AddTool(
        {"sequence_component_set",
         "Make an entity play a level sequence, or change how: adds a SequenceComponent if it has none and applies the fields (a partial update), as one undoable step. "
         "Fields: sequence (an .asequence asset path, e.g. Sequences/Intro.asequence), auto_play, loop, rate (negative plays backward), destroy_when_finished.",
         Schema({{"entity", kGuidProp}, {"fields", {{"type", "object"}, {"description", "e.g. {\"sequence\": \"Sequences/Intro.asequence\", \"auto_play\": true}"}}}}, {"entity", "fields"}),
         [&s](const Json& args) -> Json {
             Entity e = RequireEntity(s, args);
             EntityGuid guid = RequireGuid(args, "entity");
             if (!args["fields"].is_object()) throw ToolError("\"fields\" must be an object");
             ValidateSequenceFields(args["fields"]);
             const ComponentId id = RequireComponent("SequenceComponent");
             editor::CommandContext ctx = s.Context();
             s.stack.BeginTransaction("Set SequenceComponent");
             if (!s.world.HasComponentRaw(e, id)) s.stack.Execute(ctx, std::make_unique<editor::AddComponentCommand>(guid, id));
             Json warnings = Json::array();
             if (!args["fields"].empty()) {
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
             }
             s.stack.EndTransaction();
             Json out = {{"sequence_component", ComponentJson(s, e, id)}};
             if (!warnings.empty()) out["warnings"] = warnings;
             return out;
         }});

    server.AddTool({"sequence_component_remove", "Remove an entity's SequenceComponent. Undoable.", Schema({{"entity", kGuidProp}}, {"entity"}), [&s](const Json& args) -> Json {
                        Entity e = RequireEntity(s, args);
                        EntityGuid guid = RequireGuid(args, "entity");
                        const ComponentId id = RequireComponent("SequenceComponent");
                        if (!s.world.HasComponentRaw(e, id)) throw ToolError("Entity has no SequenceComponent");
                        editor::CommandContext ctx = s.Context();
                        s.stack.Execute(ctx, std::make_unique<editor::RemoveComponentCommand>(guid, id));
                        return {{"removed", true}};
                    }});

    // Work undo cannot bring back: the built-in permission rule asks the person before these run (permissions.h).
    for (const char* name : {"destroy_entity", "destroy_entities", "remove_component", "remove_components_many", "save_scene", "load_scene"}) server.MarkDestructive(name);
}

} // namespace aether::mcp
