#include "aether/scene/data_layers.h"

#include "aether/scene/gameplay.h"

#include <algorithm>

namespace aether {

namespace {

// World::ForEach gives component references but not the entity, and switching a layer has to touch
// the entity's Active, so this walks the archetypes the same way the editor's own helper does.
template <typename Func>
void ForEachDataLayer(const World& world, Func&& func) {
    world.ForEachArchetype([&](Archetype& archetype) {
        if ((archetype.Mask() & ComponentMaskOf<DataLayer>()) != ComponentMaskOf<DataLayer>()) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const u32 count = archetype.ChunkEntityCount(chunk);
            if (count == 0) continue;
            Entity* entities = archetype.EntityArray(chunk);
            auto* layers = static_cast<DataLayer*>(archetype.ComponentArray(chunk, GetComponentId<DataLayer>()));
            for (u32 i = 0; i < count; ++i) func(entities[i], layers[i]);
        }
    });
}

} // namespace

std::vector<std::string> DataLayerNames(const World& world) {
    std::vector<std::string> names;
    ForEachDataLayer(world, [&names](Entity, DataLayer& l) {
        if (l.layer.empty()) return;
        if (std::find(names.begin(), names.end(), l.layer) == names.end()) names.push_back(l.layer);
    });
    std::sort(names.begin(), names.end());
    return names;
}

usize DataLayerSize(const World& world, std::string_view layer) {
    usize count = 0;
    ForEachDataLayer(world, [&](Entity, DataLayer& l) {
        if (l.layer == layer) ++count;
    });
    return count;
}

usize SetDataLayerActive(World& world, std::string_view layer, bool active) {
    // Adding an Active component moves the entity to another archetype, which invalidates the walk
    // that found it -- so the members are collected first and the world is changed afterwards.
    std::vector<Entity> members;
    ForEachDataLayer(world, [&](Entity entity, DataLayer& l) {
        if (l.layer == layer) members.push_back(entity);
    });

    usize changed = 0;
    for (const Entity entity : members) {
        DataLayer* layer_component = world.GetComponent<DataLayer>(entity);
        if (layer_component == nullptr) continue; // gone: something else removed it mid-call
        DataLayer& l = *layer_component;
        if (l.layer != layer) continue;
        const Active* current = world.GetComponent<Active>(entity);
        const bool is_active = current == nullptr || current->active;

        if (!active) {
            // Record what the entity was BEFORE the layer goes off, once. Capturing on every disable
            // would overwrite a true with the false this very call set, and the entity would never
            // come back.
            if (!l.captured) {
                l.active_when_layer_on = is_active;
                l.captured = true;
            }
            if (!is_active) continue; // already off; the captured value is what matters
        } else {
            if (!l.captured) continue; // this layer was never switched off
            const bool restore = l.active_when_layer_on;
            l.captured = false;
            if (is_active == restore) continue; // already in the state it should return to
        }

        const bool target = active ? l.active_when_layer_on : false;
        if (current == nullptr) {
            world.AddComponent<Active>(entity, Active{target});
        } else {
            world.GetComponent<Active>(entity)->active = target;
        }
        ++changed;
    }
    return changed;
}
bool IsDataLayerActive(const World& world, std::string_view layer) {
    bool any = false;
    bool all_active = true;
    ForEachDataLayer(world, [&](Entity entity, DataLayer& l) {
        if (l.layer != layer) return;
        any = true;
        const Active* current = world.GetComponent<Active>(entity);
        if (current != nullptr && !current->active) all_active = false;
    });
    return any && all_active;
}

} // namespace aether