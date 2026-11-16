#pragma once

#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/physics/physics_scene.h"
#include "aether/ropes/rope.h"

#include <memory>
#include <vector>

namespace aether::ropes {

// Builds every enabled Rope into a chain of jointed bodies and keeps them tidy, once per physics step: call Apply just before PhysicsScene::Step. A rope is built
// once, where its entity is when the step finds it; a rope whose entity is gone, whose component is disabled or whose settings were edited is taken apart first
// (its segments are destroyed, and the joint it gave its end entity is removed) and built again. Deterministic: ropes go in entity order, segments in order.
class RopeSystem {
public:
    RopeSystem(World& world, PhysicsScene& scene);
    ~RopeSystem();
    RopeSystem(const RopeSystem&) = delete;
    RopeSystem& operator=(const RopeSystem&) = delete;

    static RopeSystem* Active();
    void MakeActive();

    // Takes apart and builds as above. Returns how many ropes are built.
    usize Apply();

    // The rope's segments in order (0 is the pinned end); empty for an entity that is not a built rope.
    std::vector<Entity> Segments(Entity rope) const;
    // The world position of a segment's centre (index 0 is the pinned end). False for an entity that is not a built rope or an index past its last segment.
    bool SegmentPosition(Entity rope, u32 index, Vec3& out) const;
    // How long the chain is now: the distances between its segments' centres, plus one segment (so a rope hanging unstretched is its `length`). 0 if not built.
    f32 CurrentLength(Entity rope) const;
    // Cuts the rope between two segments: `fraction` 0 cuts next to the pinned end, 1 next to the free end. The part beyond the cut falls away. False for a rope that
    // is not built, a fraction that is not a number, or a cut that has already been made there.
    bool Cut(Entity rope, f32 fraction);

    usize RopeCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    World& world_;
    PhysicsScene& scene_;
};

} // namespace aether::ropes
