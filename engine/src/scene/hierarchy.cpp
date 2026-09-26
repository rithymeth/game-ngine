#include "aether/scene/hierarchy.h"

#include <algorithm>

namespace aether {

Entity GetParent(const World& world, const GuidIndex& guids, Entity entity) {
    const Parent* parent = world.GetComponent<Parent>(entity);
    if (parent == nullptr || parent->parent.IsNull()) {
        return kNullEntity;
    }
    return guids.Find(world, parent->parent);
}

Mat4 LocalTransformMatrix(const Transform& transform) {
    return Mat4::Translation(transform.position) * transform.rotation.ToMat4();
}

Mat4 ComputeWorldTransform(const World& world, const GuidIndex& guids, Entity entity) {
    Mat4 result = Mat4::Identity();
    Entity current = entity;
    for (int depth = 0; depth < kMaxHierarchyDepth && world.IsAlive(current); ++depth) {
        const Transform* transform = world.GetComponent<Transform>(current);
        if (transform == nullptr) {
            break;
        }
        result = LocalTransformMatrix(*transform) * result;
        current = GetParent(world, guids, current);
    }
    return result;
}

Vec3 WorldPosition(const World& world, const GuidIndex& guids, Entity entity) {
    Mat4 m = ComputeWorldTransform(world, guids, entity);
    return Vec3(m.cols[3].x, m.cols[3].y, m.cols[3].z);
}

bool WouldCreateCycle(const World& world, const GuidIndex& guids, Entity entity, Entity new_parent) {
    Entity current = new_parent;
    for (int depth = 0; depth < kMaxHierarchyDepth && world.IsAlive(current); ++depth) {
        if (current == entity) {
            return true;
        }
        current = GetParent(world, guids, current);
    }
    return false;
}

std::vector<Entity> ChildrenOf(const World& world, const GuidIndex& guids, Entity entity) {
    std::vector<Entity> children;
    const IdComponent* id = world.GetComponent<IdComponent>(entity);
    if (id == nullptr || id->guid.IsNull()) {
        return children;
    }
    world.ForEachArchetype([&](const Archetype& archetype) {
        const ComponentId parent_id = GetComponentId<Parent>();
        if (!archetype.Has(parent_id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            const auto* parents = static_cast<const Parent*>(archetype.ComponentArray(c, parent_id));
            for (u32 row = 0; row < archetype.ChunkEntityCount(c); ++row) {
                if (parents[row].parent == id->guid) {
                    children.push_back(entities[row]);
                }
            }
        }
    });
    (void)guids;
    std::sort(children.begin(), children.end(), [](Entity a, Entity b) { return a.index < b.index; });
    return children;
}

} // namespace aether
