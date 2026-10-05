#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "aether/sprite2d/physics2d.h"

// The platformer character controller (Phase 26 step 5, §26.6): on an entity
// with a Rigidbody2D and a Collider2D it turns the player's input into
// running, jumping and falling, the way platformers feel good: acceleration
// rather than instant speed, a short grace period to jump after leaving a
// ledge (coyote time), a jump pressed just before landing still counting
// (jump buffer), variable jump height, and a faster fall than rise.

namespace aether::sprite2d {

struct PlatformerController2D {
    f32 move_speed = 6.0f;          // m/s along x at full input
    f32 ground_acceleration = 60.0f; // m/s²
    f32 air_acceleration = 35.0f;
    f32 jump_speed = 12.0f;         // m/s up
    f32 coyote_time = 0.1f;         // s a jump is still allowed after leaving the ground
    f32 jump_buffer = 0.1f;         // s a jump press is remembered before landing
    f32 jump_cut = 0.45f;           // vertical speed kept when the jump is released early
    f32 fall_gravity_scale = 1.6f;  // gravity while falling
    f32 max_fall_speed = 25.0f;     // m/s

    // Set by the host each frame from the player's input.
    f32 input_move = 0.0f;  // -1 left .. 1 right
    bool input_jump = false; // the jump button is held

    // What it remembers between steps.
    f32 time_since_grounded = 99.0f;
    f32 jump_buffer_timer = 0.0f;
    bool jump_was_held = false;
    bool jumping = false; // rising from a jump (variable height applies)
    bool grounded = false;
    i32 facing = 1; // -1 or 1, the last way it was moved
};

// A camera that follows the first PlatformerController2D entity: it eases
// toward the target (plus `offset`), optionally kept inside bounds.
struct CameraFollow2D {
    f32 smoothing = 8.0f; // 1/s; 0 snaps to the target
    Vec2 offset{0.0f, 1.0f};
    bool use_bounds = false;
    Vec2 min_bounds{-1000.0f, -1000.0f}; // the camera centre stays inside
    Vec2 max_bounds{1000.0f, 1000.0f};
};

// Moves every CameraFollow2D camera by `dt` (x and y; z stays). Call after
// the step that moved the target.
void UpdateCameraFollow2D(World& world, f32 dt);

// Runs every controller for one fixed step of `dt`: call before
// Physics2D::Step, which supplies the grounding.
void UpdatePlatformers(World& world, const Physics2D& physics, f32 dt);

void RegisterPlatformerComponents();

} // namespace aether::sprite2d

AETHER_REFLECT(aether::sprite2d::CameraFollow2D, 1,
    AETHER_FIELD(smoothing, Field_EditAnywhere, {.tooltip = "How quickly it catches up; 0 snaps", .range_min = 0.0, .range_max = 100.0}),
    AETHER_FIELD(offset, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(use_bounds, Field_EditAnywhere),
    AETHER_FIELD(min_bounds, Field_EditAnywhere, {.units = "m"}),
    AETHER_FIELD(max_bounds, Field_EditAnywhere, {.units = "m"})
)

AETHER_REFLECT(aether::sprite2d::PlatformerController2D, 1,
    AETHER_FIELD(move_speed, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(ground_acceleration, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0}),
    AETHER_FIELD(air_acceleration, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0}),
    AETHER_FIELD(jump_speed, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100.0, .units = "m/s"}),
    AETHER_FIELD(coyote_time, Field_EditAnywhere, {.tooltip = "A jump still works this long after leaving the ground", .range_min = 0.0, .range_max = 1.0, .units = "s"}),
    AETHER_FIELD(jump_buffer, Field_EditAnywhere, {.tooltip = "A jump pressed this long before landing still counts", .range_min = 0.0, .range_max = 1.0, .units = "s"}),
    AETHER_FIELD(jump_cut, Field_EditAnywhere, {.tooltip = "Share of the rising speed kept when the jump is released", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(fall_gravity_scale, Field_EditAnywhere, {.range_min = 0.1, .range_max = 10.0}),
    AETHER_FIELD(max_fall_speed, Field_EditAnywhere, {.range_min = 1.0, .range_max = 200.0, .units = "m/s"}),
    AETHER_FIELD(input_move, Field_ReadOnly),
    AETHER_FIELD(input_jump, Field_ReadOnly),
    AETHER_FIELD(grounded, Field_ReadOnly),
    AETHER_FIELD(facing, Field_ReadOnly)
)
