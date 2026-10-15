#include "prefab_tools.h"

#include "project_host.h"

#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/prefab.h"
#include "aether/scene/prefab_library.h"
#include "core/commands.h"

#include <algorithm>
#include <fstream>

namespace aether::mcp {

namespace {

using namespace detail;

struct PrefabLookupCache {
    std::unique_ptr<PrefabLibrary> library;
};

class ApplyPrefabOverridesCommand final : public editor::ICommand {
public:
    ApplyPrefabOverridesCommand(std::shared_ptr<AssetHost> host, EditorSession& session, std::filesystem::path project_file,
                                assets::AssetGuid prefab, EntityGuid instance_root, PrefabData before_prefab,
                                PrefabData after_prefab, PrefabInstance before_instance, PrefabInstance after_instance)
        : host_(std::move(host)), session_(&session), project_file_(std::move(project_file)), prefab_(prefab),
          instance_root_(instance_root), before_prefab_(std::move(before_prefab)), after_prefab_(std::move(after_prefab)),
          before_instance_(std::move(before_instance)), after_instance_(std::move(after_instance)) {
        memory_bytes_ = sizeof(*this) + PrefabToJson(before_prefab_).dump().size() + PrefabToJson(after_prefab_).dump().size();
        for (const PrefabInstance* instance : {&before_instance_, &after_instance_}) {
            memory_bytes_ += instance->removed_entities.size() * sizeof(PrefabLocalId);
            for (const PropertyOverride& override_value : instance->overrides) {
                memory_bytes_ += sizeof(override_value) + override_value.component.size() + override_value.field_path.size() +
                                 override_value.value.size();
            }
        }
    }

    void Do(editor::CommandContext&) override { SaveAndPropagate(after_prefab_, after_instance_); }
    void Undo(editor::CommandContext&) override { SaveAndPropagate(before_prefab_, before_instance_); }
    std::string Label() const override { return "Apply Overrides to Prefab"; }
    usize MemoryBytes() const override { return memory_bytes_; }
    const PropagationReport& LastReport() const { return last_report_; }

private:
    void SaveAndPropagate(const PrefabData& data, const PrefabInstance& instance) {
        if (!host_->project || host_->project->paths.file.lexically_normal() != project_file_.lexically_normal()) {
            throw ToolError("Open the project that owns this prefab before undoing or redoing this action");
        }
        const Entity root = session_->guids.Find(session_->world, instance_root_);
        if (root.IsNull() || !session_->world.HasComponent<PrefabInstance>(root)) {
            throw ToolError("The prefab instance no longer exists");
        }
        PrefabLibrary library(*host_->project->database);
        std::string error;
        const auto affected = library.Save(prefab_, data, &error);
        if (affected.empty() && !error.empty()) throw ToolError("Couldn't save the prefab: " + error);
        *session_->world.GetComponent<PrefabInstance>(root) = instance;
        last_report_ = PropagatePrefabChange(session_->world, session_->guids, library, prefab_);
    }

    std::shared_ptr<AssetHost> host_;
    EditorSession* session_;
    std::filesystem::path project_file_;
    assets::AssetGuid prefab_;
    EntityGuid instance_root_;
    PrefabData before_prefab_;
    PrefabData after_prefab_;
    PrefabInstance before_instance_;
    PrefabInstance after_instance_;
    PropagationReport last_report_;
    usize memory_bytes_ = 0;
};

const AssetRecord& RequirePrefab(OpenProject& p, const std::string& key) {
    const AssetRecord& r = FindAsset(p, key);
    if (r.importer != "Prefab") throw ToolError(r.path + " is not a prefab (it is a " + r.importer + ")");
    return r;
}

Entity RequireEntity(EditorSession& s, const Json& args, const char* key = "entity") {
    if (!args.contains(key) || !args[key].is_string()) throw ToolError(std::string("Missing string argument \"") + key + "\" (an entity GUID)");
    EntityGuid guid;
    if (!ParseEntityGuid(args[key].get<std::string>(), guid)) throw ToolError("Not a valid entity GUID: " + args[key].get<std::string>());
    const Entity e = s.guids.Find(s.world, guid);
    if (e.IsNull()) throw ToolError("No entity with GUID " + ToString(guid));
    return e;
}

Json OverrideJson(const PropertyOverride& o) {
    Json j = {{"entity", o.entity}, {"component", o.component}, {"field_path", o.field_path}};
    const Json value = Json::parse(o.value, nullptr, false);
    j["value"] = value.is_discarded() ? Json(o.value) : value;
    if (o.version != 0) j["version"] = o.version;
    return j;
}

Json OverridesJson(const std::vector<PropertyOverride>& list) {
    Json out = Json::array();
    for (const PropertyOverride& o : list) out.push_back(OverrideJson(o));
    return out;
}

std::string PathOfGuid(OpenProject& p, const assets::AssetGuid& guid) { return PathOf(p, guid); }

Json ResolveReportJson(const ResolveReport& r) {
    Json j = {{"ok", r.ok}, {"created", r.created}, {"updated", r.updated}, {"destroyed", r.destroyed}, {"orphaned", OverridesJson(r.orphaned)}};
    if (!r.error.empty()) j["error"] = r.error;
    return j;
}

// The instance root of `entity` and its PrefabInstance, or an error.
struct InstanceRef {
    Entity root;
    PrefabInstance* instance = nullptr;
};
InstanceRef RequireInstance(EditorSession& s, Entity entity) {
    const Entity root = FindInstanceRoot(s.world, s.guids, entity);
    if (root.IsNull()) throw ToolError("That entity is not part of a prefab instance");
    PrefabInstance* inst = s.world.GetComponent<PrefabInstance>(root);
    if (inst == nullptr) throw ToolError("That entity is not part of a prefab instance");
    return {root, inst};
}

PrefabLocalId OptLocalId(const Json& args) {
    if (!args.contains("local_id")) return 0;
    if (!args["local_id"].is_number_integer() || args["local_id"].get<i64>() < 0) throw ToolError("\"local_id\" must be a prefab entity id (0 = every entity)");
    return static_cast<PrefabLocalId>(args["local_id"].get<i64>());
}

} // namespace

void RegisterPrefabTools(McpServer& server, std::shared_ptr<AssetHost> host, EditorSession& session) {
    auto lookup_cache = std::make_shared<PrefabLookupCache>();
    session.prefab_lookup = [host, lookup_cache](const assets::AssetGuid& guid) -> const PrefabData* {
        OpenProject& project = host->Require();
        // Rebuild the lightweight cache for each lookup so undo/redo resolves
        // against the current on-disk prefab, including nested dependencies.
        lookup_cache->library = std::make_unique<PrefabLibrary>(*project.database);
        return lookup_cache->library->Flattened(guid);
    };

    server.AddTool(
        {"prefab_info",
         "Look inside a prefab asset: its entities (local id, parent, component names), the prefabs it nests and the base it is a variant of, what it flattens to (entity count, or "
         "the error: a missing prefab, a cycle, clashing ids), the orphaned overrides flattening found, and the prefabs built on it (nesting it or varianting it). With `scenes` "
         "true it also loads every scene that uses it into a scratch world and reports instances, orphaned overrides and errors per scene (slower).",
         Schema({{"prefab", {{"type", "string"}, {"description", "A prefab asset path under Content/, or its GUID"}}}, {"scenes", {{"type", "boolean"}}}}, {"prefab"}),
         [host](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const AssetRecord& r = RequirePrefab(p, RequireString(args, "prefab"));
             const assets::AssetGuid guid = r.guid;
             const std::string path = r.path;
             PrefabLibrary lib(*p.database);
             const PrefabData* data = lib.Get(guid);
             if (data == nullptr) throw ToolError("Can't read " + path + ": " + lib.LastError(guid));
             Json entities = Json::array();
             for (const PrefabEntity& e : data->entities) {
                 std::vector<std::string> names;
                 for (const auto& item : e.components.items()) names.push_back(item.key());
                 Json j = {{"id", e.id}, {"parent", e.parent}, {"components", names}};
                 if (e.nested) {
                     j["nested"] = {{"prefab", PathOfGuid(p, e.nested->source)}, {"overrides", e.nested->overrides.size()}, {"removed", e.nested->removed_entities}};
                 }
                 entities.push_back(j);
             }
             Json out = {{"prefab", path}, {"guid", assets::ToString(guid)}, {"entities", entities}};
             if (data->IsVariant()) {
                 out["variant_of"] = {{"prefab", PathOfGuid(p, data->base)}, {"overrides", OverridesJson(data->base_overrides)}, {"removed", data->base_removed}};
             }
             PrefabData flat;
             std::string error;
             std::vector<PropertyOverride> orphaned;
             if (FlattenPrefab(*data, lib.Lookup(), flat, &error, &orphaned, guid)) {
                 out["flattened"] = {{"ok", true}, {"entities", flat.entities.size()}, {"orphaned_overrides", OverridesJson(orphaned)}};
             } else {
                 out["flattened"] = {{"ok", false}, {"error", error}};
             }
             Json dependents = Json::array();
             for (const assets::AssetGuid& d : lib.Dependents(guid)) {
                 if (d != guid) dependents.push_back(PathOfGuid(p, d));
             }
             out["built_on_it"] = dependents;
             if (OptBool(args, "scenes", false)) {
                 Json scenes = Json::array();
                 for (const SceneImpact& s : CheckScenesUsingPrefab(lib, guid)) {
                     scenes.push_back({{"scene", s.scene}, {"instances", s.instances}, {"orphaned", OverridesJson(s.orphaned)}, {"errors", s.errors}});
                 }
                 out["scenes"] = scenes;
             }
             return out;
         }});

    server.AddTool(
        {"prefab_make",
         "Capture an entity and all its descendants (by Parent) from the editor scene as a new prefab asset, written under Content/ (`path` ends in .aprefab) and scanned so it has a GUID. "
         "Only reflected components are kept; identity and hierarchy components are not. An instance among them becomes a nested prefab entry. The scene entity is left as it is "
         "(it does not become an instance of the new prefab); place instances with prefab_instantiate.",
         Schema({{"entity", {{"type", "string"}, {"description", "Root entity GUID"}}},
                 {"path", {{"type", "string"}, {"description", "Where to save it, e.g. Prefabs/Crate.aprefab"}}},
                 {"overwrite", {{"type", "boolean"}}}},
                {"entity", "path"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const Entity root = RequireEntity(session, args);
             const std::string rel = CleanRelative(RequireString(args, "path"));
             if (fs::path(rel).extension() != ".aprefab") throw ToolError("A prefab's path ends in .aprefab");
             const fs::path file = p.paths.content / rel;
             std::error_code ec;
             if (fs::exists(file, ec) && !OptBool(args, "overwrite", false)) throw ToolError(rel + " exists; pass overwrite true to replace it");
             const PrefabData data = MakePrefab(session.world, session.guids, root);
             if (data.entities.empty()) throw ToolError("Nothing to capture");
             fs::create_directories(file.parent_path(), ec);
             std::string error;
             if (!SavePrefab(data, file, &error)) throw ToolError("Couldn't write " + rel + ": " + error);
             p.database->Scan();
             Json out = {{"path", rel}, {"entities", data.entities.size()}};
             if (const AssetRecord* r = p.database->FindByPath(rel)) out["guid"] = assets::ToString(r->guid);
             return out;
         }});

    server.AddTool(
        {"prefab_instantiate",
         "Place an instance of a prefab in the editor scene as one undoable, agent-attributed action. The root goes at `position` ([x, y, z]) if given, otherwise where the prefab's root is. Returns the root "
         "entity's GUID and the resolve report (entities created, orphaned overrides).",
         Schema({{"prefab", {{"type", "string"}}}, {"position", {{"type", "array"}, {"items", {{"type", "number"}}}}}}, {"prefab"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before instantiating a prefab");
             const AssetRecord& r = RequirePrefab(p, RequireString(args, "prefab"));
             const assets::AssetGuid guid = r.guid;
             const std::string path = r.path;
             PrefabLibrary lib(*p.database);
             const PrefabData* flat = lib.Flattened(guid);
             if (flat == nullptr) throw ToolError("Can't resolve " + path + ": " + lib.LastError(guid));
             Transform placed{};
             const Transform* at = nullptr;
             if (args.contains("position")) {
                 const Json& v = args["position"];
                 if (!v.is_array() || v.size() != 3) throw ToolError("position must be [x, y, z]");
                 placed.position = Vec3(v[0].get<f32>(), v[1].get<f32>(), v[2].get<f32>());
                 placed.rotation = Quaternion::Identity();
                 at = &placed;
             }
             ResolveReport report;
             const Entity root = InstantiatePrefab(session.world, session.guids, guid, *flat, at, &report);
             if (root.IsNull()) {
                 throw ToolError("Prefab instance couldn't be resolved: " + report.error);
             }
             const EntityGuid root_guid = session.world.GetComponent<IdComponent>(root)->guid;
             std::vector<EntityGuid> created;
             session.world.ForEach<PrefabLink, IdComponent>([&](const PrefabLink& link, const IdComponent& id) {
                 if (link.instance == root_guid) created.push_back(id.guid);
             });
             if (std::find(created.begin(), created.end(), root_guid) == created.end()) created.push_back(root_guid);
             editor::CommandContext context = session.Context();
             session.stack.BeginTransaction("Instantiate Prefab");
             for (const EntityGuid& entity : created) {
                 const Entity existing = session.guids.Find(session.world, entity);
                 if (!existing.IsNull()) {
                     session.stack.Record(editor::CreateEntityCommand::FromExisting(context, existing));
                 }
             }
             session.stack.EndTransaction();
             if (!report.ok) {
                 session.stack.Undo(context);
                 throw ToolError("Prefab instance couldn't be resolved: " + report.error);
             }
             Json out = {{"prefab", path}, {"report", ResolveReportJson(report)}};
             if (!root.IsNull()) {
                 if (const IdComponent* id = session.world.GetComponent<IdComponent>(root)) out["entity"] = ToString(id->guid);
             }
             return out;
         }});

    server.AddTool(
        {"prefab_overrides",
         "The overrides on a prefab instance: given any entity in it, the instance's prefab, its overrides (prefab entity local id, component, field path, value) and the prefab "
         "entities deleted in this instance.",
         Schema({{"entity", {{"type", "string"}, {"description", "Any entity GUID inside the instance"}}}}, {"entity"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const InstanceRef ref = RequireInstance(session, RequireEntity(session, args));
             Json out = {{"prefab", PathOfGuid(p, ref.instance->source.guid)}, {"overrides", OverridesJson(ref.instance->overrides)}, {"removed_entities", ref.instance->removed_entities}};
             if (const IdComponent* id = session.world.GetComponent<IdComponent>(ref.root)) out["instance"] = ToString(id->guid);
             return out;
         }});

    server.AddTool(
        {"prefab_revert",
         "Remove an instance's overrides and re-resolve it so the entities return to the prefab's values. Narrow with `local_id` (a prefab entity; 0 or omitted = every entity), "
         "`component` and `field_path` (a path inside the component, e.g. \"position[1]\"; omitted = the whole component). Undoable through the editor history. Returns how many were removed.",
         Schema({{"entity", {{"type", "string"}}}, {"local_id", {{"type", "integer"}}}, {"component", {{"type", "string"}}}, {"field_path", {{"type", "string"}}}}, {"entity"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             const InstanceRef ref = RequireInstance(session, RequireEntity(session, args));
             PrefabLibrary validation_library(*p.database);
             const PrefabData* flat = validation_library.Flattened(ref.instance->source.guid);
             if (flat == nullptr) throw ToolError("Can't resolve the prefab: " + validation_library.LastError(ref.instance->source.guid));
             const PrefabLocalId local_id = OptLocalId(args);
             const std::string component_name = OptString(args, "component");
             const ComponentId component = component_name.empty() ? kInvalidComponentId : RequireComponent(component_name);
             const std::string field_path = OptString(args, "field_path");
             if (!field_path.empty() && component == kInvalidComponentId) {
                 throw ToolError("A field_path requires a component");
             }
             const EntityGuid root_guid = session.world.GetComponent<IdComponent>(ref.root)->guid;
             const usize before = ref.instance->overrides.size();
             editor::CommandContext context = session.Context();
             const EntityGuid entity_guid = RequireGuid(args, "entity");
             auto command = std::make_unique<editor::RevertPrefabOverrideCommand>(entity_guid, local_id, component, field_path);
             editor::RevertPrefabOverrideCommand* command_result = command.get();
             session.stack.Execute(context, std::move(command));
             const Entity root_after = session.guids.Find(session.world, root_guid);
             const PrefabInstance* instance_after = root_after.IsNull() ? nullptr : session.world.GetComponent<PrefabInstance>(root_after);
             const usize remaining = instance_after == nullptr ? 0 : instance_after->overrides.size();
             const usize removed = before >= remaining ? before - remaining : 0;
             return {{"removed", removed}, {"remaining", remaining}, {"report", ResolveReportJson(command_result->LastReport())}};
         }});

    server.AddTool(
        {"prefab_apply",
         "\"Apply to Prefab\": write an instance's overrides into the prefab asset (same selection arguments as prefab_revert), remove them from the instance, save the prefab, "
         "and re-resolve every instance of it or of prefabs built on it in the editor scene. This is one undoable, agent-attributed action. "
         "Orphaned overrides can't be applied and stay. Overrides inside nested prefabs stay "
         "on the instance. Returns how many were applied, how many instances were re-resolved, and any failures.",
         Schema({{"entity", {{"type", "string"}}}, {"local_id", {{"type", "integer"}}}, {"component", {{"type", "string"}}}, {"field_path", {{"type", "string"}}}}, {"entity"}),
         [host, &session](const Json& args) -> Json {
             OpenProject& p = host->Require();
             if (session.stack.IsFrozen()) throw ToolError("Stop Play-in-Editor before applying prefab overrides");
             const InstanceRef ref = RequireInstance(session, RequireEntity(session, args));
             const assets::AssetGuid guid = ref.instance->source.guid;
             PrefabLibrary lib(*p.database);
             const PrefabData* current = lib.Get(guid);
             if (current == nullptr) throw ToolError("Can't read the prefab: " + lib.LastError(guid));
             PrefabData before_prefab = *current;
             PrefabData edited = before_prefab;
             const PrefabInstance before_instance = *ref.instance;
             PrefabInstance edited_instance = before_instance;
             const usize applied = ApplyOverridesToPrefab(edited, edited_instance, OptLocalId(args), OptString(args, "component"), OptString(args, "field_path"));
             Json out = {{"applied", applied}, {"remaining", edited_instance.overrides.size()}};
             if (applied == 0) return out;
             const EntityGuid root_guid = session.world.GetComponent<IdComponent>(ref.root)->guid;
             auto command = std::make_unique<ApplyPrefabOverridesCommand>(host, session, p.paths.file, guid, root_guid,
                                                                           std::move(before_prefab), std::move(edited),
                                                                           before_instance, std::move(edited_instance));
             ApplyPrefabOverridesCommand* command_result = command.get();
             editor::CommandContext context = session.Context();
             session.stack.Execute(context, std::move(command));
             const PropagationReport& propagated = command_result->LastReport();
             out["instances_resolved"] = propagated.instances;
             out["orphaned"] = propagated.orphaned;
             out["failed"] = propagated.failed;
             return out;
         }});
}

} // namespace aether::mcp
