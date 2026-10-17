#include "aether/scene/prefab_library.h"

#include "aether/scene/serialization.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <unordered_set>

namespace aether {

PrefabLibrary::Entry& PrefabLibrary::EntryFor(const assets::AssetGuid& guid) {
    Entry& entry = entries_[guid];
    if (entry.loaded) {
        return entry;
    }
    entry.loaded = true;
    const assets::AssetRecord* record = database_.Find(guid);
    if (record == nullptr || record->importer != "Prefab" || record->missing) {
        entry.error = "Asset " + assets::ToString(guid) + " isn't an available prefab";
        return entry;
    }
    PrefabData data;
    if (!LoadPrefab(database_.SourcePath(guid), data, &entry.error)) {
        return entry;
    }
    entry.data = std::move(data);
    return entry;
}

const PrefabData* PrefabLibrary::Get(const assets::AssetGuid& guid) {
    Entry& entry = EntryFor(guid);
    return entry.data ? &*entry.data : nullptr;
}

const PrefabData* PrefabLibrary::Flattened(const assets::AssetGuid& guid) {
    Entry& entry = EntryFor(guid);
    if (!entry.data) {
        return nullptr;
    }
    if (!entry.flat) {
        // Flattening loads other prefabs into entries_; references to
        // existing entries stay valid (unordered_map never moves elements).
        PrefabData flat;
        if (!FlattenPrefab(guid, Lookup(), flat, &entry.error)) {
            return nullptr;
        }
        entry.flat = std::move(flat);
    }
    return &*entry.flat;
}

std::string PrefabLibrary::LastError(const assets::AssetGuid& guid) const {
    auto it = entries_.find(guid);
    return it != entries_.end() ? it->second.error : std::string();
}

PrefabLookup PrefabLibrary::Lookup() {
    return [this](const assets::AssetGuid& guid) { return Get(guid); };
}

PrefabLookup PrefabLibrary::FlatLookup() {
    return [this](const assets::AssetGuid& guid) { return Flattened(guid); };
}

std::vector<assets::AssetGuid> PrefabLibrary::Dependents(const assets::AssetGuid& guid) {
    // Which prefabs refer to which (nesting or base), from their data.
    std::unordered_map<assets::AssetGuid, std::vector<assets::AssetGuid>> users;
    const std::vector<const assets::AssetRecord*> prefabs = database_.FindByImporter("Prefab");
    users.reserve(prefabs.size());
    for (const assets::AssetRecord* record : prefabs) {
        if (record->IsSubAsset()) {
            continue;
        }
        const assets::AssetGuid& prefab = record->guid;
        const PrefabData* data = Get(prefab);
        if (data == nullptr) {
            continue;
        }
        if (data->IsVariant()) {
            users[data->base].push_back(prefab);
        }
        for (const PrefabEntity& entity : data->entities) {
            if (entity.nested) {
                users[entity.nested->source].push_back(prefab);
            }
        }
    }
    std::unordered_set<assets::AssetGuid> seen;
    seen.reserve(prefabs.size());
    seen.insert(guid);
    std::vector<assets::AssetGuid> pending;
    pending.reserve(prefabs.size());
    pending.push_back(guid);
    while (!pending.empty()) {
        const assets::AssetGuid next = pending.back();
        pending.pop_back();
        if (auto it = users.find(next); it != users.end()) {
            for (const assets::AssetGuid& user : it->second) {
                if (seen.insert(user).second) {
                    pending.push_back(user);
                }
            }
        }
    }
    std::vector<assets::AssetGuid> result(seen.begin(), seen.end());
    std::sort(result.begin(), result.end());
    return result;
}

void PrefabLibrary::Invalidate(const std::vector<assets::AssetGuid>& guids) {
    for (const assets::AssetGuid& guid : guids) {
        if (auto it = entries_.find(guid); it != entries_.end()) {
            it->second.flat.reset();
            it->second.error.clear();
        }
    }
}

std::vector<assets::AssetGuid> PrefabLibrary::Save(const assets::AssetGuid& guid, PrefabData data, std::string* error) {
    const assets::AssetRecord* record = database_.Find(guid);
    if (record == nullptr || record->importer != "Prefab") {
        if (error != nullptr) {
            *error = "Asset " + assets::ToString(guid) + " isn't a prefab";
        }
        return {};
    }
    if (!SavePrefab(data, database_.SourcePath(guid), error)) {
        return {};
    }
    Entry& entry = entries_[guid];
    entry.loaded = true;
    entry.data = std::move(data);
    entry.error.clear();
    std::vector<assets::AssetGuid> affected = Dependents(guid);
    Invalidate(affected);
    return affected;
}

std::vector<assets::AssetGuid> PrefabLibrary::Reload(const assets::AssetGuid& guid) {
    entries_.erase(guid);
    std::vector<assets::AssetGuid> affected = Dependents(guid);
    Invalidate(affected);
    return affected;
}

PropagationReport PropagatePrefabChange(World& world, GuidIndex& guids, PrefabLibrary& library,
                                        const assets::AssetGuid& changed) {
    const std::vector<assets::AssetGuid> affected = library.Dependents(changed);
    std::unordered_set<assets::AssetGuid> affected_set;
    affected_set.reserve(affected.size());
    affected_set.insert(affected.begin(), affected.end());
    // Resolve only instances of affected prefabs; others are skipped by
    // handing them no data.
    PrefabLookup lookup = library.Lookup();
    std::vector<Entity> roots;
    const ComponentId prefab_instance_id = GetComponentId<PrefabInstance>();
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(prefab_instance_id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            const auto* instances = static_cast<const PrefabInstance*>(
                archetype.ComponentArray(c, prefab_instance_id));
            for (u32 row = 0; row < archetype.ChunkEntityCount(c); ++row) {
                if (affected_set.count(instances[row].source.guid) != 0) {
                    roots.push_back(entities[row]);
                }
            }
        }
    });
    std::sort(roots.begin(), roots.end(), [](Entity a, Entity b) { return a.index < b.index; });

    PropagationReport report;
    for (Entity root : roots) {
        if (!world.IsAlive(root)) {
            continue;
        }
        const assets::AssetGuid source = world.GetComponent<PrefabInstance>(root)->source.guid;
        if (affected_set.count(source) == 0) {
            continue;
        }
        ResolveReport result;
        if (const PrefabData* flat = library.Flattened(source)) {
            result = ResolvePrefabInstance(world, guids, root, *flat);
        } else {
            result.error = library.LastError(source);
        }
        ++report.instances;
        report.failed += result.ok ? 0 : 1;
        report.orphaned += result.orphaned.size();
        report.results.emplace_back(root, std::move(result));
    }
    return report;
}

std::vector<SceneImpact> CheckScenesUsingPrefab(PrefabLibrary& library, const assets::AssetGuid& prefab) {
    assets::AssetDatabase& database = library.Database();
    const std::vector<assets::AssetGuid> affected = library.Dependents(prefab);
    std::unordered_set<assets::AssetGuid> affected_set;
    affected_set.reserve(affected.size());
    affected_set.insert(affected.begin(), affected.end());
    std::set<std::string> scenes;
    for (const assets::AssetGuid& guid : affected) {
        for (const assets::AssetRecord* user : database.Referencers(guid)) {
            if (user->importer == "Scene") {
                scenes.insert(user->path);
            }
        }
    }

    std::vector<SceneImpact> impacts;
    for (const std::string& scene : scenes) {
        SceneImpact impact;
        impact.scene = scene;
        World world;
        GuidIndex guids;
        const std::string file = (database.ContentRoot() / scene).string();
        const bool json = std::filesystem::path(scene).extension() == ".ascene";
        if (!(json ? LoadSceneJson(world, file) : LoadScene(world, file))) {
            impact.errors.push_back("Couldn't load the scene");
            impacts.push_back(std::move(impact));
            continue;
        }
        guids.Rebuild(world);
        for (auto& [root, result] : ResolveAllPrefabInstances(world, guids, library.Lookup())) {
            const PrefabInstance* instance = world.GetComponent<PrefabInstance>(root);
            if (instance == nullptr || affected_set.count(instance->source.guid) == 0) {
                continue;
            }
            ++impact.instances;
            if (!result.ok) {
                impact.errors.push_back(result.error);
            }
            impact.orphaned.insert(impact.orphaned.end(), result.orphaned.begin(), result.orphaned.end());
        }
        impacts.push_back(std::move(impact));
    }
    return impacts;
}

} // namespace aether
