#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"
#include "aether/scene/entity_guid.h"

namespace aether::ropes {

// A rope, chain or cable: put it on an entity and the rope hangs from that entity's position. At the first physics step it becomes `segments` small dynamic
// bodies in a line (along `direction`, `length` long in all), each joined to the one before by a ball-and-socket PhysicsJoint, so it swings, drapes and is pushed
// by whatever it touches. The first segment is pinned to the world at the entity's position, or to `start_entity`'s body, or left free. A body named in
// `end_entity` is hung on the last segment: it gets a PhysicsJoint of its own for that, so it must not have one. A component, saved with the entity. Editing it
// while playing builds the rope again.
struct Rope {
    static constexpr u8 kMinSegments = 2, kMaxSegments = 64;

    bool enabled = true;
    f32 length = 5.0f;            // the whole rope, m
    u8 segments = 12;             // 2 to 64: more bends more smoothly and costs more
    Vec3 direction{0.0f, -1.0f, 0.0f}; // which way it points from the entity's position when it is made (any length)
    f32 radius = 0.05f;           // m; held to 45% of a segment's length so neighbours do not overlap
    f32 mass = 2.0f;              // the whole rope, kg
    f32 damping = 0.2f;           // air drag on every segment (per second): a rope with none swings for ever
    bool pin_start = true;        // the first segment is held where the rope starts (to the world, or to start_entity); off lets the whole rope fall
    EntityGuid start_entity;      // pin the first segment to this entity's body instead of the world
    EntityGuid end_entity;        // hang this body (it needs a RigidBody) from the last segment; none leaves the end free
    Vec3 end_anchor{0.0f, 0.0f, 0.0f}; // where on end_entity the rope ties on, in its own space; it should meet the rope's end when the rope is made
    char segment_model[128] = {}; // a model for each segment (the project's path, like ModelRenderer): a cylinder 1 m tall and 1 m across, along its local Y axis. Empty: invisible
};

// One piece of a built rope, added to its entity (read-only).
struct RopeSegment {
    EntityGuid rope;  // the rope entity's guid, or null when the rope entity has none
    u32 index = 0;    // 0 is the end that is pinned
};

void RegisterRopeComponents();

} // namespace aether::ropes

AETHER_REFLECT(aether::ropes::Rope, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(length, Field_EditAnywhere, {.tooltip = "The whole rope", .range_min = 0.05, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(segments, Field_EditAnywhere, {.tooltip = "2 to 64: more bends more smoothly and costs more", .range_min = 2.0, .range_max = 64.0}),
    AETHER_FIELD(direction, Field_EditAnywhere, {.tooltip = "Which way it points from here when it is made"}),
    AETHER_FIELD(radius, Field_EditAnywhere, {.tooltip = "Held to 45% of a segment's length so neighbours do not overlap", .range_min = 0.005, .range_max = 5.0, .units = "m"}),
    AETHER_FIELD(mass, Field_EditAnywhere, {.tooltip = "The whole rope", .range_min = 0.01, .range_max = 100000.0, .units = "kg"}),
    AETHER_FIELD(damping, Field_EditAnywhere, {.tooltip = "Air drag on every segment, per second", .range_min = 0.0, .range_max = 50.0}),
    AETHER_FIELD(pin_start, Field_EditAnywhere, {.tooltip = "Hold the first segment where the rope starts; off lets the whole rope fall"}),
    AETHER_FIELD(start_entity, Field_EditAnywhere, {.tooltip = "Pin the first segment to this entity's body instead of the world"}),
    AETHER_FIELD(end_entity, Field_EditAnywhere, {.tooltip = "Hang this body from the last segment (it gets a joint of its own)"}),
    AETHER_FIELD(end_anchor, Field_EditAnywhere, {.tooltip = "Where on the end entity the rope ties on, in its own space", .units = "m"}),
    AETHER_FIELD(segment_model, Field_EditAnywhere, {.tooltip = "A model for each segment: a cylinder 1 m tall and 1 m across along local Y. Empty: invisible"}))

AETHER_REFLECT(aether::ropes::RopeSegment, 1,
    AETHER_FIELD(rope, Field_ReadOnly),
    AETHER_FIELD(index, Field_ReadOnly))
