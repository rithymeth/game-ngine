#include "aether/scene/world_origin.h"

#include "aether/scene/components.h"

namespace aether {

usize RebaseTransforms(World& world, const Vec3d& delta) {
    const Vec3 d = delta.ToVec3();
    usize moved = 0;
    world.ForEach<Transform>([&](Transform& t) {
        t.position = t.position - d;
        ++moved;
    });
    return moved;
}

usize ShiftTransforms(World& world, const Vec3d& delta) {
    return RebaseTransforms(world, Vec3d(-delta.x, -delta.y, -delta.z));
}

} // namespace aether
