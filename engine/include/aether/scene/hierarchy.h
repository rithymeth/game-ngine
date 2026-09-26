#pragma once

#include "aether/math/math.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"

#include <vector>

namespace aether {

// Scene hierarchy (Phase 7, docs/design/PHASE_SPECS.md §7.2 ReparentCommand).
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

} // namespace aether

AETHER_REFLECT(aether::Parent, 1, AETHER_FIELD(parent, Field_ReadOnly, {.tooltip = "The parent entity's ID"}))
