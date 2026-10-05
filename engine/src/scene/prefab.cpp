#include "aether/scene/prefab.h"
#include "aether/core/json_util.h"

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

// Builds the smallest JSON holding `value` at `path` (objects for names,
// null-padded arrays for indices). False if the path is malformed.
bool SetAtPath(Json& root, const std::string& path, const Json& value) {
    Json* node = &root;
    usize i = 0;
    while (i < path.size()) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        if (path[i] == '[') {
            const usize close = path.find(']', i);
            if (close == std::string::npos || close == i + 1) {
                return false;
            }
            const std::string digits = path.substr(i + 1, close - i - 1);
            if (!std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }) ||
                digits.size() > 6) {
                return false;
            }
            const usize index = std::stoul(digits);
            if (!node->is_array()) {
                *node = Json::array();
            }
            while (node->size() <= index) {
                node->push_back(nullptr);
            }
            node = &(*node)[index];
            i = close + 1;
            continue;
        }
        usize end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') {
            ++end;
        }
        if (!node->is_object()) {
            *node = Json::object();
        }
        node = &(*node)[path.substr(i, end - i)];
        i = end;
    }
    *node = value;
    return true;
}

// The path (in override syntax) at which `target` occurs in `json`.
bool FindPathOf(const Json& json, const Json& target, const std::string& path, std::string& out) {
    if (json == target) {
        out = path;
        return true;
    }
    if (json.is_object()) {
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (it.key() != "$v" && FindPathOf(it.value(), target, path.empty() ? it.key() : path + "." + it.key(), out)) {
                return true;
            }
        }
    } else if (json.is_array()) {
        for (usize i = 0; i < json.size(); ++i) {
            if (FindPathOf(json[i], target, path + "[" + std::to_string(i) + "]", out)) {
                return true;
            }
        }
    }
    return false;
}

const reflect::TypeInfo* ReflectedComponent(const std::string& name) {
    const ComponentId id = FindComponentIdByName(name);
    return id != kInvalidComponentId ? GetComponentInfo(id).reflected : nullptr;
}

// Migrates every known component's JSON to its type's current version.
void UpgradeComponents(Json& components) {
    for (auto it = components.begin(); it != components.end(); ++it) {
        if (const reflect::TypeInfo* type = ReflectedComponent(it.key())) {
            reflect::MigrateJson(*type, it.value());
        }
    }
}

} // namespace

bool MigrateOverride(PropertyOverride& override_) {
    const reflect::TypeInfo* type = ReflectedComponent(override_.component);
    if (type == nullptr || override_.version == 0 || override_.version >= type->version) {
        return false;
    }
    const Json value = Json::parse(override_.value, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        return false;
    }
    if (override_.field_path.empty()) {
        // The whole component: migrate it as saved.
        Json whole = value;
        if (!whole.is_object()) {
            return false;
        }
        whole["$v"] = override_.version;
        reflect::MigrateJson(*type, whole);
        override_.value = whole.dump();
        override_.version = type->version;
        return true;
    }
    // Run the migration on just this field, twice: once with a marker, to see
    // where the field moved, and once with the value, to see what it became.
    static const Json kMarker = "\u0001aether-override-marker\u0001";
    Json marked = Json::object();
    Json valued = Json::object();
    if (!SetAtPath(marked, override_.field_path, kMarker) || !SetAtPath(valued, override_.field_path, value)) {
        return false;
    }
    marked["$v"] = override_.version;
    valued["$v"] = override_.version;
    reflect::MigrateJson(*type, marked);
    reflect::MigrateJson(*type, valued);
    std::string new_path;
    if (!FindPathOf(marked, kMarker, "", new_path) || new_path.empty()) {
        return false; // dropped by the migration
    }
    const Json* migrated = FindField(valued, new_path);
    if (migrated == nullptr) {
        return false;
    }
    override_.field_path = new_path;
    override_.value = migrated->dump();
    override_.version = type->version;
    return true;
}

namespace {

// Steps 1-3 of §9.2 on a flat entity list (root first, parents before
// children): drop `removed` entities (and their children), then apply
// `overrides`, whole-component ones first so a field override on the same
// component wins. Overrides that can't be applied go to `orphaned`; ones on
// removed entities are moot and dropped quietly.
// Component data is migrated to the current schema versions first, and so
// are overrides written for older ones (`migrated`, if given, receives the
// overrides as migrated, when any changed).
std::vector<PrefabEntity> ApplyPrefabEdits(const std::vector<PrefabEntity>& entities, PrefabLocalId root_id,
                                           const std::vector<PropertyOverride>& original_overrides,
                                           const std::vector<PrefabLocalId>& removed_ids,
                                           std::vector<PropertyOverride>& orphaned,
                                           std::vector<PropertyOverride>* migrated = nullptr) {
    std::vector<PropertyOverride> overrides = original_overrides;
    bool any_migrated = false;
    for (PropertyOverride& override_ : overrides) {
        any_migrated |= MigrateOverride(override_);
    }
    if (any_migrated && migrated != nullptr) {
        *migrated = overrides;
    }
    std::vector<PrefabEntity> data;
    std::set<PrefabLocalId> kept;
    std::set<PrefabLocalId> all;
    const std::set<PrefabLocalId> removed(removed_ids.begin(), removed_ids.end());
    for (const PrefabEntity& entity : entities) {
        all.insert(entity.id);
        const bool is_root = entity.id == root_id;
        if (!is_root && (removed.count(entity.id) != 0 || kept.count(entity.parent) == 0)) {
            continue;
        }
        kept.insert(entity.id);
        data.push_back(entity);
        UpgradeComponents(data.back().components);
    }

    std::vector<const PropertyOverride*> ordered;
    for (const PropertyOverride& override_ : overrides) {
        ordered.push_back(&override_);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const PropertyOverride* a, const PropertyOverride* b) {
        return a->field_path.empty() && !b->field_path.empty();
    });
    for (const PropertyOverride* override_ptr : ordered) {
        const PropertyOverride& override_ = *override_ptr;
        if (all.count(override_.entity) != 0 && kept.count(override_.entity) == 0) {
            continue; // on a removed entity: moot, not orphaned
        }
        Json* field = nullptr;
        for (PrefabEntity& entity : data) {
            if (entity.id != override_.entity) {
                continue;
            }
            if (auto component = entity.components.find(override_.component); component != entity.components.end()) {
                field = FindField(*component, override_.field_path);
            }
            break;
        }
        Json value = Json::parse(override_.value, nullptr, /*allow_exceptions=*/false);
        if (field == nullptr || value.is_discarded()) {
            orphaned.push_back(override_);
            continue;
        }
        *field = std::move(value);
    }
    return data;
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

namespace {

Json NestedToJson(const assets::AssetGuid& source, const std::vector<PropertyOverride>& overrides,
                  const std::vector<PrefabLocalId>& removed) {
    return {{"source", assets::ToString(source)}, {"overrides", reflect::ToJson(overrides)}, {"removed", removed}};
}

bool NestedFromJson(const Json& json, assets::AssetGuid& source, std::vector<PropertyOverride>& overrides,
                    std::vector<PrefabLocalId>& removed) {
    if (!json.is_object() || !json.contains("source") || !json["source"].is_string() ||
        !assets::ParseAssetGuid(json["source"].get_ref<const std::string&>(), source) || source.IsNull()) {
        return false;
    }
    if (auto it = json.find("overrides"); it != json.end() && !reflect::FromJson(overrides, *it)) {
        return false;
    }
    if (auto it = json.find("removed"); it != json.end()) {
        if (!it->is_array()) {
            return false;
        }
        for (const Json& id : *it) {
            if (!id.is_number_unsigned()) {
                return false;
            }
            removed.push_back(id.get<PrefabLocalId>());
        }
    }
    return true;
}

} // namespace

bool PrefabData::IsFlat() const {
    return !IsVariant() &&
           std::none_of(entities.begin(), entities.end(), [](const PrefabEntity& e) { return e.nested.has_value(); });
}

Json PrefabToJson(const PrefabData& prefab) {
    Json entities = Json::array();
    for (const PrefabEntity& entity : prefab.entities) {
        Json item{{"id", entity.id}, {"parent", entity.parent}, {"components", entity.components}};
        if (entity.nested) {
            item["prefab"] = NestedToJson(entity.nested->source, entity.nested->overrides, entity.nested->removed_entities);
        }
        entities.push_back(std::move(item));
    }
    Json json = {{"$type", "Prefab"}, {"$version", kPrefabVersion}, {"entities", std::move(entities)}};
    if (prefab.IsVariant()) {
        json["base"] = NestedToJson(prefab.base, prefab.base_overrides, prefab.base_removed);
    }
    return json;
}

bool PrefabFromJson(const Json& json, PrefabData& out, std::string* error) {
    if (!json.is_object() || json.value("$type", "") != "Prefab") {
        SetError(error, "Not an Aether prefab");
        return false;
    }
    if (JsonVersion(json) != kPrefabVersion) {
        SetError(error, "Unsupported prefab version");
        return false;
    }
    PrefabData prefab;
    if (auto base = json.find("base"); base != json.end() &&
                                       !NestedFromJson(*base, prefab.base, prefab.base_overrides, prefab.base_removed)) {
        SetError(error, "The prefab's \"base\" is malformed");
        return false;
    }
    auto entities = json.find("entities");
    if (entities == json.end() || !entities->is_array() || (entities->empty() && !prefab.IsVariant())) {
        SetError(error, "The prefab has no entities");
        return false;
    }
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
        if (auto nested = item.find("prefab"); nested != item.end()) {
            NestedPrefab data;
            if (!NestedFromJson(*nested, data.source, data.overrides, data.removed_entities)) {
                SetError(error, "Prefab entity " + std::to_string(entity.id) + " has a malformed \"prefab\"");
                return false;
            }
            entity.nested = std::move(data);
        }
        if (prefab.IsVariant()) {
            // A variant's entities hang off the base's (not known here) or
            // earlier added ones; FlattenPrefab checks the parents exist.
            if (entity.id == 0 || seen.count(entity.id) != 0 || entity.parent == 0) {
                SetError(error, "Variant entity " + std::to_string(entity.id) +
                                    ": ids must be unique and non-zero, and every entity needs a parent");
                return false;
            }
            seen.insert(entity.id);
            prefab.entities.push_back(std::move(entity));
            continue;
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
        if (const PrefabInstance* instance = world.GetComponent<PrefabInstance>(item.entity)) {
            // A prefab instance: nest its prefab instead of copying its entities.
            entity.nested = NestedPrefab{instance->source.guid, instance->overrides, instance->removed_entities};
            if (const Transform* transform = world.GetComponent<Transform>(item.entity)) {
                entity.components["Transform"] = reflect::ToJson(*transform);
            }
            prefab.entities.push_back(std::move(entity));
            continue; // its entities come from its prefab
        }
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

PrefabLocalId NestedLocalId(PrefabLocalId outer, PrefabLocalId inner) {
    u32 hash = 2166136261u;
    for (PrefabLocalId part : {outer, inner}) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= (part >> shift) & 0xffu;
            hash *= 16777619u;
        }
    }
    return hash == 0 ? 1 : hash;
}

namespace {

std::string ChainText(const std::vector<assets::AssetGuid>& chain, const assets::AssetGuid& last) {
    std::string text;
    for (const assets::AssetGuid& guid : chain) {
        text += assets::ToString(guid) + " -> ";
    }
    return text + assets::ToString(last);
}

struct Flattener {
    const PrefabLookup& find;
    std::vector<PropertyOverride>& orphaned;
    std::string error;
    std::vector<assets::AssetGuid> chain; // prefabs being flattened, outermost first (null = unnamed)

    // The flat entities of the prefab `source`, or false.
    bool FlattenSource(const assets::AssetGuid& source, std::vector<PrefabEntity>& out) {
        if (std::find(chain.begin(), chain.end(), source) != chain.end()) {
            error = "Prefab cycle: " + ChainText(chain, source);
            return false;
        }
        if (chain.size() >= static_cast<usize>(kMaxPrefabNesting)) {
            error = "Prefabs nested more than " + std::to_string(kMaxPrefabNesting) + " deep: " + ChainText(chain, source);
            return false;
        }
        const PrefabData* prefab = find ? find(source) : nullptr;
        if (prefab == nullptr) {
            error = "Prefab " + assets::ToString(source) + " isn't available (used by " +
                    (chain.empty() ? std::string("this prefab") : ChainText({}, chain.back())) + ")";
            return false;
        }
        chain.push_back(source);
        const bool ok = Flatten(*prefab, out);
        chain.pop_back();
        return ok;
    }

    bool Flatten(const PrefabData& prefab, std::vector<PrefabEntity>& out) {
        std::vector<PrefabEntity> result;
        if (prefab.IsVariant()) {
            std::vector<PrefabEntity> base;
            if (!FlattenSource(prefab.base, base)) {
                return false;
            }
            result = ApplyPrefabEdits(base, base.front().id, prefab.base_overrides, prefab.base_removed, orphaned);
        }
        for (const PrefabEntity& entity : prefab.entities) {
            if (entity.parent != 0 &&
                std::none_of(result.begin(), result.end(), [&](const PrefabEntity& e) { return e.id == entity.parent; })) {
                error = "Prefab entity " + std::to_string(entity.id) + "'s parent " + std::to_string(entity.parent) +
                        " doesn't exist (removed from the base, or listed after it)";
                return false;
            }
            if (!entity.nested) {
                result.push_back(entity);
                continue;
            }
            std::vector<PrefabEntity> sub;
            if (!FlattenSource(entity.nested->source, sub)) {
                return false;
            }
            const PrefabLocalId sub_root = sub.front().id;
            sub = ApplyPrefabEdits(sub, sub_root, entity.nested->overrides, entity.nested->removed_entities, orphaned);
            for (PrefabEntity& inner : sub) {
                if (inner.id == sub_root) {
                    inner.id = entity.id;
                    inner.parent = entity.parent;
                    for (auto it = entity.components.begin(); it != entity.components.end(); ++it) {
                        inner.components[it.key()] = it.value(); // e.g. where it's placed
                    }
                } else {
                    inner.id = NestedLocalId(entity.id, inner.id);
                    inner.parent = inner.parent == sub_root ? entity.id : NestedLocalId(entity.id, inner.parent);
                }
                result.push_back(std::move(inner));
            }
        }
        if (result.empty() || result.front().parent != 0) {
            error = "The prefab has no root entity";
            return false;
        }
        std::set<PrefabLocalId> ids;
        for (const PrefabEntity& entity : result) {
            if (!ids.insert(entity.id).second) {
                error = "Two prefab entities got local id " + std::to_string(entity.id) +
                        "; renumber the entities of one of the nested prefabs";
                return false;
            }
        }
        out = std::move(result);
        return true;
    }
};

} // namespace

bool FlattenPrefab(const PrefabData& prefab, const PrefabLookup& find, PrefabData& out, std::string* error,
                   std::vector<PropertyOverride>* orphaned, const assets::AssetGuid& self) {
    std::vector<PropertyOverride> ignored;
    Flattener flattener{find, orphaned != nullptr ? *orphaned : ignored, {}, {}};
    if (!self.IsNull()) {
        flattener.chain.push_back(self);
    }
    PrefabData flat;
    if (!flattener.Flatten(prefab, flat.entities)) {
        SetError(error, flattener.error);
        return false;
    }
    out = std::move(flat);
    return true;
}

bool FlattenPrefab(const assets::AssetGuid& source, const PrefabLookup& find, PrefabData& out, std::string* error,
                   std::vector<PropertyOverride>* orphaned) {
    const PrefabData* prefab = find ? find(source) : nullptr;
    if (prefab == nullptr) {
        SetError(error, "Prefab " + assets::ToString(source) + " isn't available");
        return false;
    }
    return FlattenPrefab(*prefab, find, out, error, orphaned, source);
}

void SetOverride(PrefabInstance& instance, PrefabLocalId entity, const std::string& component,
                 const std::string& field_path, const Json& value) {
    const reflect::TypeInfo* type = ReflectedComponent(component);
    const u16 version = type != nullptr ? type->version : 0;
    for (PropertyOverride& existing : instance.overrides) {
        if (existing.entity == entity && existing.component == component && existing.field_path == field_path) {
            existing.value = value.dump();
            existing.version = version;
            return;
        }
    }
    instance.overrides.push_back({entity, component, field_path, value.dump(), version});
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
    Json base_copy = *base;
    reflect::MigrateJson(*info.reflected, base_copy); // compare like with like
    std::vector<std::pair<std::string, Json>> diffs;
    DiffJson(base_copy, current, "", diffs);

    PrefabInstance& instance = *world.GetComponent<PrefabInstance>(root);
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
        if (OverrideMatches(*it, entity, component, field_path) && prefab.IsVariant() &&
            prefab.Find(it->entity) == nullptr) {
            // An entity from the variant's base: record it as a variant override.
            bool replaced = false;
            for (PropertyOverride& existing : prefab.base_overrides) {
                if (existing.entity == it->entity && existing.component == it->component &&
                    existing.field_path == it->field_path) {
                    existing.value = it->value;
                    replaced = true;
                }
            }
            if (!replaced) {
                prefab.base_overrides.push_back(*it);
            }
            it = overrides.erase(it);
            ++applied;
            continue;
        }
        if (OverrideMatches(*it, entity, component, field_path)) {
            if (PrefabEntity* target = prefab.Find(it->entity); target != nullptr && !target->nested) {
                if (auto comp = target->components.find(it->component); comp != target->components.end()) {
                    // Both in the current schema version (the prefab is saved upgraded).
                    if (const reflect::TypeInfo* type = ReflectedComponent(it->component)) {
                        reflect::MigrateJson(*type, *comp);
                    }
                    MigrateOverride(*it);
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
    if (!prefab.IsFlat()) {
        report.error = "The prefab has nested prefabs or a base; flatten it first (FlattenPrefab)";
        return report;
    }
    // A copy: adding components below moves the root's storage.
    const PrefabInstance instance = *world.GetComponent<PrefabInstance>(root);
    const EntityGuid root_guid = EnsureGuid(world, root, &guids);
    const PrefabLocalId root_id = prefab.Root();

    std::vector<PropertyOverride> migrated;
    std::vector<PrefabEntity> data = ApplyPrefabEdits(prefab.entities, root_id, instance.overrides,
                                                      instance.removed_entities, report.orphaned, &migrated);
    if (!migrated.empty()) {
        // Keep the overrides in their migrated form (saved with the scene).
        world.GetComponent<PrefabInstance>(root)->overrides = std::move(migrated);
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
    struct Flat {
        bool ok = false;
        std::string error;
        PrefabData data;
        std::vector<PropertyOverride> orphaned; // inside the prefab itself
    };
    std::unordered_map<assets::AssetGuid, Flat> flattened; // each prefab once
    for (Entity root : EntitiesWith(world, GetComponentId<PrefabInstance>())) {
        if (!world.IsAlive(root)) {
            continue; // destroyed while resolving an earlier instance
        }
        const assets::AssetGuid source = world.GetComponent<PrefabInstance>(root)->source.guid;
        auto [it, inserted] = flattened.try_emplace(source);
        Flat& flat = it->second;
        if (inserted) {
            flat.ok = FlattenPrefab(source, find, flat.data, &flat.error, &flat.orphaned);
        }
        if (!flat.ok) {
            ResolveReport failed;
            failed.error = flat.error;
            reports.emplace_back(root, std::move(failed));
            continue;
        }
        ResolveReport report = ResolvePrefabInstance(world, guids, root, flat.data);
        report.orphaned.insert(report.orphaned.end(), flat.orphaned.begin(), flat.orphaned.end());
        reports.emplace_back(root, std::move(report));
    }
    return reports;
}

} // namespace aether
