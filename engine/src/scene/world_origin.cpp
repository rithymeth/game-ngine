#include "aether/scene/world_origin.h"

#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

namespace aether {

usize RebaseTransforms(World& world, const Vec3d& delta, const GuidIndex* guids) {
    const Vec3 d = delta.ToVec3();
    const ComponentId transform_id = GetComponentId<Transform>();
    const ComponentId parent_id = GetComponentId<Parent>();
    usize moved = 0;
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(transform_id)) return;
        const bool may_have_parent = archetype.Mask().test(parent_id);
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            auto* transforms = static_cast<Transform*>(archetype.ComponentArray(chunk, transform_id));
            Entity* entities = archetype.EntityArray(chunk);
            const usize count = archetype.ChunkEntityCount(chunk);
            for (usize i = 0; i < count; ++i) {
                // A child's Transform is relative to its parent; only roots move.
                // A Parent that no longer resolves leaves the entity a root.
                if (may_have_parent && (guids == nullptr || !GetParent(world, *guids, entities[i]).IsNull())) continue;
                transforms[i].position = transforms[i].position - d;
                ++moved;
            }
        }
    });
    return moved;
}

usize ShiftTransforms(World& world, const Vec3d& delta, const GuidIndex* guids) {
    return RebaseTransforms(world, Vec3d(-delta.x, -delta.y, -delta.z), guids);
}

} // namespace aether
