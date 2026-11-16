#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

namespace aether::vehicles {

enum class DriveType : u8 { FrontWheel, RearWheel, AllWheel };

// A car: four wheels on springs, an engine with an automatic gearbox, brakes and a handbrake, on top of a dynamic body. Put it on an entity that has a
// RigidBody (dynamic) and a BoxCollider for the chassis; the vehicle drives along the entity's +Z axis (its nose) with +Y up. The wheels are placed from the
// collider: at the corners, `track_fraction` of the half width out and `wheelbase_fraction` of the half length along, in the order front-left, front-right,
// rear-left, rear-right (front is +Z; the two front wheels steer). A component, saved with the entity. Editing it while playing rebuilds the vehicle.
struct Vehicle {
    bool enabled = true;
    DriveType drive = DriveType::RearWheel;
    f32 wheel_radius = 0.35f;
    f32 wheel_width = 0.25f;
    f32 track_fraction = 0.9f;      // how far out the wheels sit, as a fraction of the chassis' half width
    f32 wheelbase_fraction = 0.7f;  // how far from the middle, as a fraction of the chassis' half length
    f32 ground_clearance = 0.3f;    // how far the chassis bottom sits above flat ground at rest, m
    f32 suspension_min = 0.2f;      // the suspension's shortest length, m
    f32 suspension_max = 0.5f;      // its longest, m
    f32 suspension_frequency = 1.5f; // the spring's natural frequency, Hz (a stiffer ride is higher)
    f32 suspension_damping = 0.5f;  // 0 bounces for ever, 1 is critically damped
    f32 engine_torque = 500.0f;     // the engine's peak torque, N m
    f32 max_rpm = 6000.0f;
    f32 max_steer_degrees = 30.0f;
    f32 brake_torque = 1500.0f;     // per wheel, N m
    f32 handbrake_torque = 4000.0f; // per rear wheel, N m
    f32 grip = 1.0f;                // scales the tyres' friction: 1 is a road tyre, below it slides, above it sticks
};

// What drives the vehicle: written by gameplay, a script (Vehicles.Drive) or the AI; read by the system every physics step.
struct VehicleInput {
    f32 throttle = 0.0f;   // -1 (reverse) to 1 (forward)
    f32 steer = 0.0f;      // -1 (left) to 1 (right)
    f32 brake = 0.0f;      // 0 to 1
    f32 handbrake = 0.0f;  // 0 to 1
};

// What the vehicle is doing, written by the system after every physics step (read-only).
struct VehicleState {
    f32 speed = 0.0f;          // m/s along the vehicle's nose (negative when reversing)
    f32 rpm = 0.0f;
    i32 gear = 0;              // -1 reverse, 0 neutral, 1 and up forward
    f32 steer_angle = 0.0f;    // the front wheels' angle now, radians (positive to the right)
    u32 wheels_on_ground = 0;  // 0 to 4
    bool airborne = false;     // no wheel touches the ground
};

void RegisterVehicleComponents();

} // namespace aether::vehicles

AETHER_ENUM(aether::vehicles::DriveType, 1, AETHER_ENUM_VALUE(FrontWheel), AETHER_ENUM_VALUE(RearWheel), AETHER_ENUM_VALUE(AllWheel))
AETHER_REFLECT(aether::vehicles::Vehicle, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(drive, Field_EditAnywhere, {.tooltip = "Which wheels the engine drives"}),
    AETHER_FIELD(wheel_radius, Field_EditAnywhere, {.range_min = 0.05, .range_max = 5.0, .units = "m"}),
    AETHER_FIELD(wheel_width, Field_EditAnywhere, {.range_min = 0.02, .range_max = 2.0, .units = "m"}),
    AETHER_FIELD(track_fraction, Field_EditAnywhere, {.tooltip = "How far out the wheels sit, as a fraction of the chassis' half width", .range_min = 0.1, .range_max = 1.5}),
    AETHER_FIELD(wheelbase_fraction, Field_EditAnywhere, {.tooltip = "How far from the middle, as a fraction of the chassis' half length", .range_min = 0.1, .range_max = 1.5}),
    AETHER_FIELD(ground_clearance, Field_EditAnywhere, {.tooltip = "How far the chassis bottom sits above flat ground at rest", .range_min = 0.0, .range_max = 5.0, .units = "m"}),
    AETHER_FIELD(suspension_min, Field_EditAnywhere, {.tooltip = "The suspension's shortest length", .range_min = 0.01, .range_max = 3.0, .units = "m"}),
    AETHER_FIELD(suspension_max, Field_EditAnywhere, {.tooltip = "The suspension's longest length", .range_min = 0.02, .range_max = 3.0, .units = "m"}),
    AETHER_FIELD(suspension_frequency, Field_EditAnywhere, {.tooltip = "The spring's natural frequency: a stiffer ride is higher", .range_min = 0.1, .range_max = 20.0, .units = "Hz"}),
    AETHER_FIELD(suspension_damping, Field_EditAnywhere, {.tooltip = "0 bounces for ever, 1 is critically damped", .range_min = 0.0, .range_max = 5.0}),
    AETHER_FIELD(engine_torque, Field_EditAnywhere, {.tooltip = "The engine's peak torque", .range_min = 1.0, .range_max = 100000.0, .units = "N m"}),
    AETHER_FIELD(max_rpm, Field_EditAnywhere, {.range_min = 1500.0, .range_max = 20000.0}),
    AETHER_FIELD(max_steer_degrees, Field_EditAnywhere, {.range_min = 0.0, .range_max = 80.0, .units = "deg"}),
    AETHER_FIELD(brake_torque, Field_EditAnywhere, {.tooltip = "Per wheel", .range_min = 0.0, .range_max = 100000.0, .units = "N m"}),
    AETHER_FIELD(handbrake_torque, Field_EditAnywhere, {.tooltip = "Per rear wheel", .range_min = 0.0, .range_max = 100000.0, .units = "N m"}),
    AETHER_FIELD(grip, Field_EditAnywhere, {.tooltip = "Scales the tyres' friction: 1 is a road tyre", .range_min = 0.05, .range_max = 5.0}))

AETHER_REFLECT(aether::vehicles::VehicleInput, 1,
    AETHER_FIELD(throttle, Field_EditAnywhere, {.range_min = -1.0, .range_max = 1.0}),
    AETHER_FIELD(steer, Field_EditAnywhere, {.range_min = -1.0, .range_max = 1.0}),
    AETHER_FIELD(brake, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(handbrake, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}))

AETHER_REFLECT(aether::vehicles::VehicleState, 1,
    AETHER_FIELD(speed, Field_ReadOnly, {.units = "m/s"}),
    AETHER_FIELD(rpm, Field_ReadOnly),
    AETHER_FIELD(gear, Field_ReadOnly),
    AETHER_FIELD(steer_angle, Field_ReadOnly, {.units = "rad"}),
    AETHER_FIELD(wheels_on_ground, Field_ReadOnly),
    AETHER_FIELD(airborne, Field_ReadOnly))
