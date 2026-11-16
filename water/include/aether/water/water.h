#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

namespace aether::water {

// A body of water: put it on an entity. The entity's position is the still-water level (its Y) and, for a bounded surface, the middle of it. The surface is a sum
// of two travelling waves (the second shorter, lower and turned 37 degrees) over the level, and a current moves whatever floats in it. A component, saved with the
// entity. Where surfaces overlap, a bounded one wins over an infinite one, and then the first by entity order.
struct WaterSurface {
    bool enabled = true;
    bool infinite = true;                  // covers the whole plane; off: only the box of `half_extents` (X and Z) round the entity
    Vec3 half_extents{50.0f, 0.0f, 50.0f}; // a bounded surface's reach, m (Y is not used)
    f32 density = 1000.0f;                 // kg/m^3 (fresh water 1000, sea 1025)
    f32 wave_height = 0.3f;                // the first wave's crest above the level, m (0 is flat water)
    f32 wave_length = 12.0f;               // crest to crest, m
    f32 wave_speed = 2.5f;                 // how fast the crests travel, m/s
    f32 wave_direction = 0.0f;             // degrees about the up axis that the crests travel towards: 0 is +X, 90 is +Z
    f32 current_speed = 0.0f;              // m/s
    f32 current_direction = 0.0f;          // degrees, as wave_direction
};

// A body that floats: put it on an entity with a RigidBody (dynamic) and a collider. The hull is a box of `half_extents` round `offset` (in the body's space);
// it is cut into a grid of columns, each of which gets the upward force of the water it displaces under it, at its own point, plus drag from the water. So a
// body sits where its weight meets the water it displaces, rocks with the waves, and rolls back upright when tipped. A component, saved with the entity.
struct Floater {
    static constexpr u8 kMinColumns = 1, kMaxColumns = 12;

    bool enabled = true;
    Vec3 half_extents{0.5f, 0.25f, 1.0f};  // the hull, m
    Vec3 offset{0.0f, 0.0f, 0.0f};         // the hull's middle in the body's space, m
    u8 columns_x = 3;                      // 1 to 12 across and along: more is smoother and costs more
    u8 columns_z = 3;
    f32 buoyancy_scale = 1.0f;             // 1 is the water's true lift; more floats it higher
    f32 linear_drag = 3.0f;                // per second where the hull is wet: how the water damps its bobbing (gliding is a quarter of it)
    f32 angular_drag = 1.5f;               // per second where the hull is wet
};

// A boat: put it on an entity that has a Floater. A propeller at `propeller_offset` pushes it along its +Z axis (thrust), only while that point is under the
// water, and a rudder turns it (steering), more the faster it goes; a keel resists sliding sideways. Driven by Water.Drive from a script. A component.
struct Boat {
    bool enabled = true;
    f32 thrust = 4000.0f;                  // N at full throttle
    f32 reverse_ratio = 0.4f;              // reversing is this fraction of the forward thrust
    f32 turn_torque = 1500.0f;             // N*m about the up axis at full steer and full speed
    f32 turn_speed = 4.0f;                 // m/s at which the rudder reaches full effect
    f32 keel_drag = 4.0f;                  // per second: how hard sideways drift is resisted
    f32 max_speed = 12.0f;                 // m/s: the propeller stops pushing beyond this
    Vec3 propeller_offset{0.0f, -0.2f, -1.0f}; // where the thrust acts, in the body's space (aft, low)
};

void RegisterWaterComponents();

} // namespace aether::water

AETHER_REFLECT(aether::water::WaterSurface, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(infinite, Field_EditAnywhere, {.tooltip = "Covers the whole plane; off: only the box of half_extents round the entity"}),
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.tooltip = "A bounded surface's reach (X and Z)", .units = "m"}),
    AETHER_FIELD(density, Field_EditAnywhere, {.tooltip = "Fresh water 1000, sea 1025", .range_min = 1.0, .range_max = 20000.0, .units = "kg/m^3"}),
    AETHER_FIELD(wave_height, Field_EditAnywhere, {.tooltip = "The first wave's crest above the level; 0 is flat", .range_min = 0.0, .range_max = 20.0, .units = "m"}),
    AETHER_FIELD(wave_length, Field_EditAnywhere, {.tooltip = "Crest to crest", .range_min = 0.5, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(wave_speed, Field_EditAnywhere, {.tooltip = "How fast the crests travel", .range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(wave_direction, Field_EditAnywhere, {.tooltip = "Degrees the crests travel towards: 0 is +X, 90 is +Z", .range_min = -360.0, .range_max = 360.0}),
    AETHER_FIELD(current_speed, Field_EditAnywhere, {.tooltip = "Moves whatever floats in it", .range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(current_direction, Field_EditAnywhere, {.tooltip = "Degrees, as wave_direction", .range_min = -360.0, .range_max = 360.0}))

AETHER_REFLECT(aether::water::Floater, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(half_extents, Field_EditAnywhere, {.tooltip = "The hull: the box of water the body displaces", .units = "m"}),
    AETHER_FIELD(offset, Field_EditAnywhere, {.tooltip = "The hull's middle in the body's space", .units = "m"}),
    AETHER_FIELD(columns_x, Field_EditAnywhere, {.tooltip = "1 to 12: more is smoother and costs more", .range_min = 1.0, .range_max = 12.0}),
    AETHER_FIELD(columns_z, Field_EditAnywhere, {.tooltip = "1 to 12: more is smoother and costs more", .range_min = 1.0, .range_max = 12.0}),
    AETHER_FIELD(buoyancy_scale, Field_EditAnywhere, {.tooltip = "1 is the water's true lift; more floats it higher", .range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(linear_drag, Field_EditAnywhere, {.tooltip = "Per second where the hull is wet", .range_min = 0.0, .range_max = 100.0}),
    AETHER_FIELD(angular_drag, Field_EditAnywhere, {.tooltip = "Per second at full submersion", .range_min = 0.0, .range_max = 100.0}))

AETHER_REFLECT(aether::water::Boat, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(thrust, Field_EditAnywhere, {.tooltip = "Newtons at full throttle", .range_min = 0.0, .range_max = 1.0e7, .units = "N"}),
    AETHER_FIELD(reverse_ratio, Field_EditAnywhere, {.tooltip = "Reversing is this fraction of the forward thrust", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(turn_torque, Field_EditAnywhere, {.tooltip = "About the up axis at full steer and full speed", .range_min = 0.0, .range_max = 1.0e7, .units = "N*m"}),
    AETHER_FIELD(turn_speed, Field_EditAnywhere, {.tooltip = "The speed at which the rudder reaches full effect", .range_min = 0.1, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(keel_drag, Field_EditAnywhere, {.tooltip = "How hard sideways drift is resisted, per second", .range_min = 0.0, .range_max = 100.0}),
    AETHER_FIELD(max_speed, Field_EditAnywhere, {.tooltip = "The propeller stops pushing beyond this", .range_min = 0.1, .range_max = 200.0, .units = "m/s"}),
    AETHER_FIELD(propeller_offset, Field_EditAnywhere, {.tooltip = "Where the thrust acts, in the body's space", .units = "m"}))
