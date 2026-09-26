#include "aether/scene/prefab_library.h"

#include "aether/scene/serialization.h"

#include <algorithm>
#include <filesystem>
#include <set>

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
    std::vector<assets::AssetGuid> prefabs;
    for (const assets::AssetRecord* record : database_.All()) {
        if (record->importer == "Prefab" && !record->IsSubAsset()) {
            prefabs.push_back(record->guid);
        }
    }
    for (const assets::AssetGuid& prefab : prefabs) {
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
    std::set<assets::AssetGuid> seen{guid};
    std::vector<assets::AssetGuid> pending{guid};
    while (!pending.empty()) {
        const assets::AssetGuid next = pending.back();
        pending.pop_back();
        for (const assets::AssetGuid& user : users[next]) {
            if (seen.insert(user).second) {
                pending.push_back(user);
            }
        }
    }
    return {seen.begin(), seen.end()};
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
    const std::set<assets::AssetGuid> affected_set(affected.begin(), affected.end());
    // Resolve only instances of affected prefabs; others are skipped by
    // handing them no data.
    PrefabLookup lookup = library.Lookup();
    std::vector<Entity> roots;
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<PrefabInstance>())) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            roots.insert(roots.end(), entities, entities + archetype.ChunkEntityCount(c));
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
    const std::set<assets::AssetGuid> affected_set(affected.begin(), affected.end());
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
