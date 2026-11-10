#pragma once

#include "aether/math/math.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace aether {

// Scene hierarchy (Phase 7 ReparentCommand).
// An entity with a Parent has a Transform relative to that parent. The link is
// the parent's EntityGuid, not its Entity handle, so it survives the parent
// being destroyed and recreated (undo, reload): while the parent doesn't
// exist the child simply behaves as a root, and it's attached again as soon
// as an entity with that GUID exists. (Moved here from the editor, where it
// stored a raw Entity and needed stale-handle cleanup.)
struct Parent {
    EntityGuid parent;
};

// Longest parent chain followed; deeper chains (or a cycle produced by bad
// data) are cut off there rather than looping.
inline constexpr int kMaxHierarchyDepth = 64;

// The entity's live parent, or kNullEntity (no Parent component, or its
// parent doesn't currently exist).
Entity GetParent(const World& world, const GuidIndex& guids, Entity entity);

Mat4 LocalTransformMatrix(const Transform& transform);

// Local -> world, by composing Transforms up the parent chain.
Mat4 ComputeWorldTransform(const World& world, const GuidIndex& guids, Entity entity);
Vec3 WorldPosition(const World& world, const GuidIndex& guids, Entity entity);

// True if making `new_parent` the parent of `entity` would make `entity` its
// own ancestor (including new_parent == entity).
bool WouldCreateCycle(const World& world, const GuidIndex& guids, Entity entity, Entity new_parent);

// Live children of `entity`, in entity-index order.
std::vector<Entity> ChildrenOf(const World& world, const GuidIndex& guids, Entity entity);

// The children of every entity, found in one pass over the world. ChildrenOf scans the whole world on each call, so walking a
// subtree with it costs (entities in the subtree) x (entities in the world): build one of these instead when asking about more
// than a few entities. It is a snapshot; build it again after entities or Parent components change.
class ChildIndex {
public:
    explicit ChildIndex(const World& world);
    // Live children of `entity`, in entity-index order: what ChildrenOf returns.
    std::span<const Entity> Children(const World& world, Entity entity) const;

private:
    std::unordered_map<EntityGuid, std::vector<Entity>> children_;
};

// `root` and every live entity under it, each once, a parent before its children and siblings in entity-index order. One pass
// over the world whatever the size of the subtree, and a cycle in bad data ends instead of looping.
std::vector<Entity> SubtreeOf(const World& world, Entity root);

} // namespace aether

AETHER_REFLECT(aether::Parent, 1, AETHER_FIELD(parent, Field_ReadOnly, {.tooltip = "The parent entity's ID"}))
