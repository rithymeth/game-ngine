#include "aether/scene/world_origin.h"

#include "aether/scene/components.h"
#include "aether/scene/hierarchy.h"

namespace aether {

usize RebaseTransforms(World& world, const Vec3d& delta) {
    const Vec3 d = delta.ToVec3();
    const ComponentId transform_id = GetComponentId<Transform>();
    const ComponentId parent_id = GetComponentId<Parent>();
    usize moved = 0;
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(transform_id)) return;
        // Only roots: a child's Transform is relative to its parent, so shifting
        // it too would move it twice.
        if (archetype.Mask().test(parent_id)) return;
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            auto* transforms = static_cast<Transform*>(archetype.ComponentArray(chunk, transform_id));
            const usize count = archetype.ChunkEntityCount(chunk);
            for (usize i = 0; i < count; ++i) transforms[i].position = transforms[i].position - d;
            moved += count;
        }
    });
    return moved;
}

usize ShiftTransforms(World& world, const Vec3d& delta) {
    return RebaseTransforms(world, Vec3d(-delta.x, -delta.y, -delta.z));
}

} // namespace aether
