#include "aether/scene/level_instance.h"

#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"
#include "aether/scene/serialization.h"

namespace aether {

LevelInstanceReport ExpandLevelInstances(World& world, GuidIndex& guids, const SceneToPrefab& to_prefab) {
    LevelInstanceReport report;
    if (!to_prefab) return report;

    // Collect first: instantiating adds entities, which moves archetypes and would invalidate a walk
    // that were still in progress.
    std::vector<Entity> instances;
    world.ForEachArchetype([&instances](Archetype& archetype) {
        if ((archetype.Mask() & ComponentMaskOf<LevelInstance>()) != ComponentMaskOf<LevelInstance>()) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            if (count == 0) continue;
            Entity* entities = archetype.EntityArray(chunk);
            auto* levels = static_cast<LevelInstance*>(archetype.ComponentArray(chunk, GetComponentId<LevelInstance>()));
            for (u32 i = 0; i < count; ++i) {
                if (!levels[i].expanded && !levels[i].scene.empty()) instances.push_back(entities[i]);
            }
        }
    });

    for (const Entity entity : instances) {
        LevelInstance* level = world.GetComponent<LevelInstance>(entity);
        if (level == nullptr || level->expanded) continue;
        PrefabData prefab;
        std::string error;
        if (!to_prefab(level->scene, prefab, error)) {
            report.failed.push_back(level->scene + ": " + (error.empty() ? "could not be read" : error));
            continue;
        }
        // The instance root keeps the placement's Transform and Parent (see ResolvePrefabInstance), so
        // the instance is created at the placement rather than wherever the scene's root was.
        const Transform* placement = world.GetComponent<Transform>(entity);
        ResolveReport resolved;
        assets::AssetGuid source{}; // a scene is not a prefab asset; the instance is identified by its root
        const Entity root = InstantiatePrefab(world, guids, source, prefab, placement, &resolved);
        if (root.IsNull()) {
            report.failed.push_back(level->scene + ": nothing was instantiated");
            continue;
        }
        // Put the scene's root under the placement so moving the placement moves the instance.
        // Parenting is by GUID in this engine, so the placement needs one before it can be a parent.
        if (!prefab.entities.empty()) {
            const IdComponent* placement_id = world.GetComponent<IdComponent>(entity);
            if (placement_id != nullptr) world.AddComponent(root, Parent{placement_id->guid});
        }
        level = world.GetComponent<LevelInstance>(entity);
        if (level != nullptr) level->expanded = true;
        ++report.expanded;
    }
    return report;
}


bool LoadSceneAsPrefab(const std::string& path, PrefabData& out, std::string& error) {
    World temp;
    if (!LoadScene(temp, path)) {
        error = "could not load scene \"" + path + "\"";
        return false;
    }
    GuidIndex guids;
    guids.Rebuild(temp);

    // Roots are the entities with no Parent. They are collected before anything is added, because adding
    // an entity moves archetypes and would invalidate a walk still in progress.
    std::vector<Entity> roots;
    temp.ForEachArchetype([&roots](Archetype& archetype) {
        if ((archetype.Mask() & ComponentMaskOf<Parent>()) != 0) return; // these have parents already
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            Entity* entities = archetype.EntityArray(chunk);
            for (u32 i = 0; i < count; ++i) roots.push_back(entities[i]);
        }
    });
    if (roots.empty()) {
        error = "the scene \"" + path + "\" has no entities to place";
        return false;
    }

    // The synthetic root: a prefab needs exactly one, so this stands in and the scene's roots hang off it.
    const Entity synthetic = temp.CreateEntity(IdComponent{NewEntityGuid()});
    const EntityGuid synthetic_guid = temp.GetComponent<IdComponent>(synthetic)->guid;
    guids.Add(synthetic_guid, synthetic);
    for (const Entity root : roots) {
        const IdComponent* id = temp.GetComponent<IdComponent>(root);
        if (id == nullptr) continue; // nothing to point a Parent at
        temp.AddComponent(root, Parent{synthetic_guid});
    }
    out = MakePrefab(temp, guids, synthetic);
    if (out.entities.empty()) {
        error = "the scene \"" + path + "\" produced no prefab data";
        return false;
    }
    return true;
}

} // namespace aether