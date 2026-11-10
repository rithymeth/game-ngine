#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <unordered_set>

namespace aether {

Entity GetParent(const World& world, const GuidIndex& guids, Entity entity) {
    const Parent* parent = world.GetComponent<Parent>(entity);
    if (parent == nullptr || parent->parent.IsNull()) {
        return kNullEntity;
    }
    return guids.Find(world, parent->parent);
}

Mat4 LocalTransformMatrix(const Transform& transform) {
    return Mat4::Translation(transform.position) * transform.rotation.ToMat4() * Mat4::Scale(transform.scale);
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
    const ComponentId parent_id = GetComponentId<Parent>();
    world.ForEachArchetype([&](const Archetype& archetype) {
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

ChildIndex::ChildIndex(const World& world) {
    const ComponentId parent_id = GetComponentId<Parent>();
    world.ForEachArchetype([&](const Archetype& archetype) {
        if (!archetype.Has(parent_id)) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const Entity* entities = archetype.EntityArray(c);
            const auto* parents = static_cast<const Parent*>(archetype.ComponentArray(c, parent_id));
            for (u32 row = 0; row < archetype.ChunkEntityCount(c); ++row) {
                if (!parents[row].parent.IsNull()) {
                    children_[parents[row].parent].push_back(entities[row]);
                }
            }
        }
    });
    for (auto& [guid, children] : children_) {
        (void)guid;
        std::sort(children.begin(), children.end(), [](Entity a, Entity b) { return a.index < b.index; });
    }
}

std::span<const Entity> ChildIndex::Children(const World& world, Entity entity) const {
    const IdComponent* id = world.IsAlive(entity) ? world.GetComponent<IdComponent>(entity) : nullptr;
    if (id == nullptr || id->guid.IsNull()) {
        return {};
    }
    const auto it = children_.find(id->guid);
    return it == children_.end() ? std::span<const Entity>() : std::span<const Entity>(it->second);
}

std::vector<Entity> SubtreeOf(const World& world, Entity root) {
    std::vector<Entity> subtree;
    if (!world.IsAlive(root)) {
        return subtree;
    }
    const ChildIndex index(world);
    std::unordered_set<u32> seen;
    std::vector<Entity> stack{root};
    while (!stack.empty()) {
        const Entity entity = stack.back();
        stack.pop_back();
        if (!seen.insert(entity.index).second) {
            continue; // a cycle in bad data
        }
        subtree.push_back(entity);
        const std::span<const Entity> children = index.Children(world, entity);
        for (usize i = children.size(); i-- > 0;) { // reversed, so they pop in entity order
            stack.push_back(children[i]);
        }
    }
    return subtree;
}

} // namespace aether
