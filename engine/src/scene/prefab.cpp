#include "aether/scene/prefab.h"

#include "aether/core/log.h"
#include "aether/platform/filesystem.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

namespace aether {

using Json = nlohmann::json;

namespace {

constexpr int kPrefabVersion = 1;

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

// Components the prefab system manages itself: never stored in prefab data,
// never removed from an instance's entities by resolving.
bool IsManaged(ComponentId id) {
    return id == GetComponentId<IdComponent>() || id == GetComponentId<Parent>() ||
           id == GetComponentId<PrefabInstance>() || id == GetComponentId<PrefabLink>();
}

std::vector<ComponentId> ComponentsOf(const World& world, Entity entity) {
    std::vector<ComponentId> ids;
    for (ComponentId id = 0; id < kMaxComponentTypes; ++id) {
        if (world.HasComponentRaw(entity, id)) {
            ids.push_back(id);
        }
    }
    return ids;
}

// Every live entity with component `id`, in entity-index order.
std::vector<Entity> EntitiesWith(const World& world, ComponentId id) {
    std::vector<Entity> result;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            result.insert(result.end(), entities, entities + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(result.begin(), result.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return result;
}

// "stats.max", "items[2].tint", "position[1]" -> the JSON value at that path, or null if
// any step is missing. "" is the whole component.
Json* FindField(Json& root, const std::string& path) {
    Json* node = &root;
    usize i = 0;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        if (path[i] == '[') {
            const usize close = path.find(']', i);
            if (close == std::string::npos || close == i + 1 || !node->is_array()) {
                return nullptr;
            }
            const std::string digits = path.substr(i + 1, close - i - 1);
            if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
                return nullptr;
            }
            const usize index = std::stoul(digits);
            if (index >= node->size()) {
                return nullptr;
            }
            node = &(*node)[index];
            i = close + 1;
            continue;
        }
        usize end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') {
            ++end;
        }
        const std::string name = path.substr(i, end - i);
        if (!node->is_object()) {
            return nullptr;
        }
        auto it = node->find(name);
        if (it == node->end()) {
            return nullptr;
        }
        node = &*it;
        i = end;
    }
    return node;
}

} // namespace

// ---------------------------------------------------------------------------
// Prefab data and files
// ---------------------------------------------------------------------------

const PrefabEntity* PrefabData::Find(PrefabLocalId id) const {
    for (const PrefabEntity& entity : entities) {
        if (entity.id == id) {
            return &entity;
        }
    }
    return nullptr;
}

PrefabEntity* PrefabData::Find(PrefabLocalId id) {
    return const_cast<PrefabEntity*>(static_cast<const PrefabData*>(this)->Find(id));
}

Json PrefabToJson(const PrefabData& prefab) {
    Json entities = Json::array();
    for (const PrefabEntity& entity : prefab.entities) {
        entities.push_back({{"id", entity.id}, {"parent", entity.parent}, {"components", entity.components}});
    }
    return {{"$type", "Prefab"}, {"$version", kPrefabVersion}, {"entities", std::move(entities)}};
}

bool PrefabFromJson(const Json& json, PrefabData& out, std::string* error) {
    if (!json.is_object() || json.value("$type", "") != "Prefab") {
        SetError(error, "Not an Aether prefab");
        return false;
    }
    if (json.value("$version", 0) != kPrefabVersion) {
        SetError(error, "Unsupported prefab version");
        return false;
    }
    auto entities = json.find("entities");
    if (entities == json.end() || !entities->is_array() || entities->empty()) {
        SetError(error, "The prefab has no entities");
        return false;
    }
    PrefabData prefab;
    std::set<PrefabLocalId> seen;
    for (const Json& item : *entities) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_number_unsigned()) {
            SetError(error, "A prefab entity has no valid id");
            return false;
        }
        PrefabEntity entity;
        entity.id = item["id"].get<PrefabLocalId>();
        entity.parent = item.value("parent", 0u);
        if (auto components = item.find("components"); components != item.end() && components->is_object()) {
            entity.components = *components;
        }
        const bool is_root = prefab.entities.empty();
        if (entity.id == 0 || seen.count(entity.id) != 0) {
            SetError(error, "Prefab entity ids must be unique and non-zero (" + std::to_string(entity.id) + ")");
            return false;
        }
        if (is_root != (entity.parent == 0) || (!is_root && seen.count(entity.parent) == 0)) {
            SetError(error, "Prefab entity " + std::to_string(entity.id) +
                                ": the first entity must be the only root, and parents must come before children");
            return false;
        }
        seen.insert(entity.id);
        prefab.entities.push_back(std::move(entity));
    }
    out = std::move(prefab);
    return true;
}

bool SavePrefab(const PrefabData& prefab, const std::filesystem::path& path, std::string* error) {
    std::string text = PrefabToJson(prefab).dump(2);
    text.push_back('\n');
    if (!fs::WriteFileBytes(path.string(), text.data(), text.size())) {
        SetError(error, "Couldn't write " + path.string());
        return false;
    }
    return true;
}

bool LoadPrefab(const std::filesystem::path& path, PrefabData& out, std::string* error) {
    std::vector<u8> bytes;
    if (!fs::ReadFileBytes(path.string(), bytes)) {
        SetError(error, "Couldn't read " + path.string());
        return false;
    }
    Json json = Json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded()) {
        SetError(error, path.string() + " isn't valid JSON");
        return false;
    }
    std::string message;
    if (!PrefabFromJson(json, out, &message)) {
        SetError(error, path.string() + ": " + message);
        return false;
    }
    return true;
}

PrefabData MakePrefab(const World& world, const GuidIndex& guids, Entity root) {
    PrefabData prefab;
    PrefabLocalId next_id = 1;
    struct Item {
        Entity entity;
        PrefabLocalId parent;
    };
    std::vector<Item> stack{{root, 0}};
    while (!stack.empty()) {
        const Item item = stack.back();
        stack.pop_back();
        PrefabEntity entity;
        entity.id = next_id++;
        entity.parent = item.parent;
        for (ComponentId id : ComponentsOf(world, item.entity)) {
            const ComponentInfo& info = GetComponentInfo(id);
            if (IsManaged(id) || info.reflected == nullptr) {
                continue;
            }
            entity.components[info.reflected->name] = reflect::ToJson(*info.reflected, world.GetComponentRaw(item.entity, id));
        }
        prefab.entities.push_back(std::move(entity));
        // Children in entity order; pushed reversed so they pop in order.
        std::vector<Entity> children = ChildrenOf(world, guids, item.entity);
        std::sort(children.begin(), children.end(), [](Entity a, Entity b) { return a.index > b.index; });
        for (Entity child : children) {
            stack.push_back({child, prefab.entities.back().id});
        }
    }
    return prefab;
}

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

void SetOverride(PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                 const std::string& field_path, const Json& value) {
    for (PropertyOverride& existing : instance.overrides) {
        if (existing.entity == entity && existing.component == component && existing.field_path == field_path) {
            existing.value = value.dump();
            return;
        }
    }
    instance.overrides.push_back({entity, component, field_path, value.dump()});
}

bool RemoveOverride(PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                    const std::string& field_path) {
    auto it = std::find_if(instance.overrides.begin(), instance.overrides.end(), [&](const PropertyOverride& o) {
        return o.entity == entity && o.component == component && o.field_path == field_path;
    });
    if (it == instance.overrides.end()) {
        return false;
    }
    instance.overrides.erase(it);
    return true;
}

Entity FindInstanceRoot(const World& world, const GuidIndex& guids, Entity entity) {
    if (!world.IsAlive(entity)) {
        return kNullEntity;
    }
    if (world.HasComponent<PrefabInstance>(entity)) {
        return entity;
    }
    const PrefabLink* link = world.GetComponent<PrefabLink>(entity);
    if (link == nullptr) {
        return kNullEntity;
    }
    const Entity root = guids.Find(world, link->instance);
    return !root.IsNull() && world.HasComponent<PrefabInstance>(root) ? root : kNullEntity;
}

namespace {

std::string Join(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }

// Leaf paths where `current` differs from `base`. Objects are compared key by
// key (keys only in `current` can't be overridden: the field isn't in the
// prefab), same-size arrays element by element, anything else as a whole.
void DiffJson(const Json& base, const Json& current, const std::string& path,
              std::vector<std::pair<std::string, Json>>& out) {
    if (base == current) {
        return;
    }
    if (base.is_object() && current.is_object()) {
        for (auto it = base.begin(); it != base.end(); ++it) {
            if (!it.key().empty() && it.key().front() == '$') {
                continue; // "$v" and other archive metadata
            }
            if (auto other = current.find(it.key()); other != current.end()) {
                DiffJson(it.value(), *other, Join(path, it.key()), out);
            }
        }
        return;
    }
    if (base.is_array() && current.is_array() && base.size() == current.size()) {
        for (usize i = 0; i < base.size(); ++i) {
            DiffJson(base[i], current[i], path + "[" + std::to_string(i) + "]", out);
        }
        return;
    }
    out.emplace_back(path, current);
}

bool IsInsidePath(const std::string& path, const std::string& outer) {
    if (outer.empty() || path == outer) {
        return true;
    }
    return path.size() > outer.size() && path.compare(0, outer.size(), outer) == 0 &&
           (path[outer.size()] == '.' || path[outer.size()] == '[');
}

} // namespace

bool RecordPrefabOverrides(World& world, const GuidIndex& guids, Entity entity, ComponentId component,
                           const PrefabData& prefab) {
    const Entity root = FindInstanceRoot(world, guids, entity);
    const PrefabLink* link = world.IsAlive(entity) ? world.GetComponent<PrefabLink>(entity) : nullptr;
    const ComponentInfo& info = GetComponentInfo(component);
    if (root.IsNull() || link == nullptr || info.reflected == nullptr || IsManaged(component)) {
        return false;
    }
    if (entity == root && component == GetComponentId<Transform>()) {
        return true; // the instance's placement
    }
    const PrefabEntity* base_entity = prefab.Find(link->local_id);
    if (base_entity == nullptr) {
        return false;
    }
    const std::string name = info.reflected->name;
    auto base = base_entity->components.find(name);
    if (base == base_entity->components.end() || !world.HasComponentRaw(entity, component)) {
        return false;
    }
    const PrefabLocalId local_id = link->local_id;
    const Json current = reflect::ToJson(*info.reflected, world.GetComponentRaw(entity, component));
    std::vector<std::pair<std::string, Json>> diffs;
    DiffJson(*base, current, "", diffs);

    PrefabInstance& instance = *world.GetComponent<PrefabInstance>(root);
    Json base_copy = *base;
    auto& overrides = instance.overrides;
    overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
                                   [&](const PropertyOverride& o) {
                                       // Keep orphans: their field isn't in the prefab to compare with.
                                       return o.entity == local_id && o.component == name &&
                                              FindField(base_copy, o.field_path) != nullptr;
                                   }),
                    overrides.end());
    for (const auto& [path, value] : diffs) {
        SetOverride(instance, local_id, name, path, value);
    }
    return true;
}

bool OverrideMatches(const PropertyOverride& override_, PrefabLocalId entity, const std::string& component,
                     const std::string& field_path) {
    return (entity == 0 || override_.entity == entity) && (component.empty() || override_.component == component) &&
           IsInsidePath(override_.field_path, field_path);
}

bool IsFieldOverridden(const PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                       const std::string& field_path) {
    return std::any_of(instance.overrides.begin(), instance.overrides.end(), [&](const PropertyOverride& o) {
        return o.entity == entity && o.component == component &&
               (IsInsidePath(o.field_path, field_path) || IsInsidePath(field_path, o.field_path));
    });
}

usize ApplyOverridesToPrefab(PrefabData& prefab, PrefabInstance& instance, PrefabLocalId entity,
                             const std::string& component, const std::string& field_path) {
    usize applied = 0;
    auto& overrides = instance.overrides;
    for (auto it = overrides.begin(); it != overrides.end();) {
        Json* field = nullptr;
        if (OverrideMatches(*it, entity, component, field_path)) {
            if (PrefabEntity* target = prefab.Find(it->entity)) {
                if (auto comp = target->components.find(it->component); comp != target->components.end()) {
                    field = FindField(*comp, it->field_path);
                }
            }
        }
        Json value = field != nullptr ? Json::parse(it->value, nullptr, /*allow_exceptions=*/false) : Json();
        if (field == nullptr || value.is_discarded()) {
            ++it;
            continue;
        }
        *field = std::move(value);
        it = overrides.erase(it);
        ++applied;
    }
    return applied;
}

usize RevertOverrides(PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                      const std::string& field_path) {
    const usize before = instance.overrides.size();
    auto& overrides = instance.overrides;
    overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
                                   [&](const PropertyOverride& o) {
                                       return OverrideMatches(o, entity, component, field_path);
                                   }),
                    overrides.end());
    return before - overrides.size();
}

ResolveReport ResolvePrefabInstance(World& world, GuidIndex& guids, Entity root, const PrefabData& prefab) {
    ResolveReport report;
    if (!world.IsAlive(root) || !world.HasComponent<PrefabInstance>(root)) {
        report.error = "Not a prefab instance";
        return report;
    }
    if (prefab.entities.empty()) {
        report.error = "The prefab has no entities";
        return report;
    }
    // A copy: adding components below moves the root's storage.
    const PrefabInstance instance = *world.GetComponent<PrefabInstance>(root);
    const EntityGuid root_guid = EnsureGuid(world, root, &guids);
    const PrefabLocalId root_id = prefab.Root();

    // 1-2. The prefab's entities, minus the ones this instance removed (and
    // their children; parents come first, so one pass is enough).
    std::vector<PrefabEntity> data;
    std::set<PrefabLocalId> kept;
    const std::set<PrefabLocalId> removed(instance.removed_entities.begin(), instance.removed_entities.end());
    for (const PrefabEntity& entity : prefab.entities) {
        const bool is_root = entity.id == root_id;
        if (!is_root && (removed.count(entity.id) != 0 || kept.count(entity.parent) == 0)) {
            continue;
        }
        kept.insert(entity.id);
        data.push_back(entity);
    }
    auto find_data = [&](PrefabLocalId id) -> PrefabEntity* {
        for (PrefabEntity& entity : data) {
            if (entity.id == id) {
                return &entity;
            }
        }
        return nullptr;
    };

    // 3. Overrides: whole-component ones first, so a field override on the
    // same component wins (the more specific override wins, §9.2).
    std::vector<const PropertyOverride*> ordered;
    for (const PropertyOverride& override_ : instance.overrides) {
        ordered.push_back(&override_);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const PropertyOverride* a, const PropertyOverride* b) {
        return a->field_path.empty() && !b->field_path.empty();
    });
    for (const PropertyOverride* override_ptr : ordered) {
        const PropertyOverride& override_ = *override_ptr;
        if (prefab.Find(override_.entity) != nullptr && kept.count(override_.entity) == 0) {
            continue; // on a removed entity: moot, not orphaned
        }
        PrefabEntity* entity = find_data(override_.entity);
        Json* field = nullptr;
        if (entity != nullptr) {
            if (auto component = entity->components.find(override_.component); component != entity->components.end()) {
                field = FindField(*component, override_.field_path);
            }
        }
        Json value = Json::parse(override_.value, nullptr, /*allow_exceptions=*/false);
        if (field == nullptr || value.is_discarded()) {
            report.orphaned.push_back(override_);
            continue;
        }
        *field = std::move(value);
    }

    // 4. Entities that already belong to this instance, by local id.
    std::unordered_map<PrefabLocalId, Entity> existing;
    for (Entity entity : EntitiesWith(world, GetComponentId<PrefabLink>())) {
        const PrefabLink* link = world.GetComponent<PrefabLink>(entity);
        if (link->instance == root_guid && entity != root) {
            existing[link->local_id] = entity;
        }
    }
    existing[root_id] = root;

    // 5. Create or update each entity, parents first.
    std::unordered_map<PrefabLocalId, Entity> resolved;
    for (const PrefabEntity& entity_data : data) {
        const bool is_root = entity_data.id == root_id;
        Entity entity;
        if (auto it = existing.find(entity_data.id); it != existing.end() && world.IsAlive(it->second)) {
            entity = it->second;
            ++report.updated;
        } else {
            entity = world.CreateEntity(IdComponent{NewEntityGuid()});
            guids.Add(world.GetComponent<IdComponent>(entity)->guid, entity);
            ++report.created;
        }
        resolved[entity_data.id] = entity;
        world.AddComponent<PrefabLink>(entity, PrefabLink{root_guid, entity_data.id});
        if (!is_root) {
            const Entity parent = resolved.at(entity_data.parent);
            world.AddComponent<Parent>(entity, Parent{EnsureGuid(world, parent, &guids)});
        }

        // The root keeps its own placement once it has one.
        const bool keep_root_transform = is_root && world.HasComponent<Transform>(entity);
        const std::string transform_name = GetComponentInfo(GetComponentId<Transform>()).reflected->name;
        std::set<ComponentId> wanted;
        for (auto it = entity_data.components.begin(); it != entity_data.components.end(); ++it) {
            const ComponentId id = FindComponentIdByName(it.key());
            if (id == kInvalidComponentId || GetComponentInfo(id).reflected == nullptr || IsManaged(id)) {
                AETHER_LOG_WARN("Prefab", "Skipping unknown component '%s' in a prefab", it.key().c_str());
                continue;
            }
            wanted.insert(id);
            if (keep_root_transform && it.key() == transform_name) {
                continue;
            }
            world.AddComponentRaw(entity, id);
            reflect::FromJson(*GetComponentInfo(id).reflected, world.GetComponentRaw(entity, id), it.value());
        }
        // Reflected components the prefab no longer has go away (the root's
        // Transform and non-reflected runtime components stay).
        for (ComponentId id : ComponentsOf(world, entity)) {
            const ComponentInfo& info = GetComponentInfo(id);
            if (!IsManaged(id) && info.reflected != nullptr && wanted.count(id) == 0 &&
                !(is_root && id == GetComponentId<Transform>())) {
                world.RemoveComponentRaw(entity, id);
            }
        }
    }

    // 6. Entities of this instance that the prefab (or the instance) no longer has.
    for (const auto& [local_id, entity] : existing) {
        if (resolved.count(local_id) == 0 && world.IsAlive(entity)) {
            if (const IdComponent* id = world.GetComponent<IdComponent>(entity)) {
                guids.Remove(id->guid);
            }
            world.DestroyEntity(entity);
            ++report.destroyed;
        }
    }
    report.ok = true;
    return report;
}

Entity InstantiatePrefab(World& world, GuidIndex& guids, const assets::AssetGuid& source, const PrefabData& prefab,
                         const Transform* transform, ResolveReport* report) {
    PrefabInstance instance;
    instance.source.guid = source;
    Entity root = world.CreateEntity(IdComponent{NewEntityGuid()}, std::move(instance));
    guids.Add(world.GetComponent<IdComponent>(root)->guid, root);
    if (transform != nullptr) {
        world.AddComponent<Transform>(root, *transform);
    }
    ResolveReport result = ResolvePrefabInstance(world, guids, root, prefab);
    if (report != nullptr) {
        *report = std::move(result);
    }
    return root;
}

std::vector<std::pair<Entity, ResolveReport>> ResolveAllPrefabInstances(World& world, GuidIndex& guids,
                                                                         const PrefabLookup& find) {
    std::vector<std::pair<Entity, ResolveReport>> reports;
    for (Entity root : EntitiesWith(world, GetComponentId<PrefabInstance>())) {
        if (!world.IsAlive(root)) {
            continue; // destroyed while resolving an earlier instance
        }
        const assets::AssetGuid source = world.GetComponent<PrefabInstance>(root)->source.guid;
        const PrefabData* prefab = find ? find(source) : nullptr;
        if (prefab == nullptr) {
            ResolveReport missing;
            missing.error = "Prefab " + assets::ToString(source) + " isn't available";
            reports.emplace_back(root, std::move(missing));
            continue;
        }
        reports.emplace_back(root, ResolvePrefabInstance(world, guids, root, *prefab));
    }
    return reports;
}

} // namespace aether
