#pragma once

#include "aether/scene/hierarchy.h"

#include <functional>

namespace aether::player {

// Composes visual-only local poses through the parent chain. The sampler may
// override position/rotation; scale and all authoritative ECS/Jolt data stay
// untouched. Without a sampler this is exactly ComputeWorldTransform.
inline Mat4 ComputeRenderWorldTransform(
    const World& world, const GuidIndex& guids, Entity entity,
    const std::function<bool(Entity, Transform&)>& sample) {
    if (!sample) return ComputeWorldTransform(world, guids, entity);
    Mat4 result = Mat4::Identity();
    Entity current = entity;
    for (int depth = 0; depth < kMaxHierarchyDepth && world.IsAlive(current); ++depth) {
        const Transform* authoritative = world.GetComponent<Transform>(current);
        if (!authoritative) break;
        Transform visual = *authoritative;
        Transform sampled = visual;
        if (sample(current, sampled)) {
            visual.position = sampled.position;
            visual.rotation = sampled.rotation;
        }
        result = LocalTransformMatrix(visual) * result;
        current = GetParent(world, guids, current);
    }
    return result;
}

} // namespace aether::player
