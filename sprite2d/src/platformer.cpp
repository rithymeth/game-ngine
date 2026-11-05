#include "aether/sprite2d/platformer.h"

#include "aether/sprite2d/components.h"

#include <algorithm>
#include <cmath>

namespace aether::sprite2d {

namespace {

f32 MoveToward(f32 value, f32 target, f32 max_delta) {
    if (std::fabs(target - value) <= max_delta) return target;
    return value + (target > value ? max_delta : -max_delta);
}

void EachEntityWith(const World& world, const std::function<void(Entity)>& f) {
    world.ForEachArchetype([&](const Archetype& archetype) {
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            const u32 count = archetype.ChunkEntityCount(c);
            const Entity* entities = archetype.EntityArray(c);
            for (u32 i = 0; i < count; ++i) f(entities[i]);
        }
    });
}

} // namespace

void UpdatePlatformers(World& world, const Physics2D& physics, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    std::vector<Entity> controlled;
    EachEntityWith(world, [&](Entity e) {
        if (world.GetComponent<PlatformerController2D>(e) && world.GetComponent<Rigidbody2D>(e) &&
            world.GetComponent<Collider2D>(e) && world.GetComponent<Transform>(e)) controlled.push_back(e);
    });
    for (Entity e : controlled) {
        PlatformerController2D& pc = *world.GetComponent<PlatformerController2D>(e);
        Rigidbody2D& rb = *world.GetComponent<Rigidbody2D>(e);
        if (!std::isfinite(pc.move_speed) || pc.move_speed < 0.0f ||
            !std::isfinite(pc.ground_acceleration) || pc.ground_acceleration < 0.0f ||
            !std::isfinite(pc.air_acceleration) || pc.air_acceleration < 0.0f ||
            !std::isfinite(pc.jump_speed) || pc.jump_speed < 0.0f ||
            !std::isfinite(pc.coyote_time) || pc.coyote_time < 0.0f ||
            !std::isfinite(pc.jump_buffer) || pc.jump_buffer < 0.0f ||
            !std::isfinite(pc.jump_cut) || pc.jump_cut < 0.0f || pc.jump_cut > 1.0f ||
            !std::isfinite(pc.fall_gravity_scale) || pc.fall_gravity_scale < 0.0f ||
            !std::isfinite(pc.max_fall_speed) || pc.max_fall_speed < 0.0f ||
            !std::isfinite(pc.time_since_grounded) || pc.time_since_grounded < 0.0f ||
            !std::isfinite(pc.jump_buffer_timer) || pc.jump_buffer_timer < 0.0f ||
            !std::isfinite(rb.velocity.x) || !std::isfinite(rb.velocity.y)) continue;
        pc.grounded = physics.IsGrounded(e);
        pc.time_since_grounded = pc.grounded ? 0.0f : std::min(pc.time_since_grounded + dt, 99.0f);

        const f32 move = std::isfinite(pc.input_move) ? std::clamp(pc.input_move, -1.0f, 1.0f) : 0.0f;
        const f32 accel = pc.grounded ? pc.ground_acceleration : pc.air_acceleration;
        rb.velocity.x = MoveToward(rb.velocity.x, move * pc.move_speed, accel * dt);
        if (std::fabs(move) > 0.05f) pc.facing = move < 0.0f ? -1 : 1;
        if (Sprite* sprite = world.GetComponent<Sprite>(e)) sprite->flip_x = pc.facing < 0;

        // A press starts the buffer; a jump uses it while the ground (or its grace period) is there.
        const bool pressed = pc.input_jump && !pc.jump_was_held;
        pc.jump_was_held = pc.input_jump;
        pc.jump_buffer_timer = pressed ? pc.jump_buffer : std::max(pc.jump_buffer_timer - dt, 0.0f);
        if ((pressed || pc.jump_buffer_timer > 0.0f) && pc.time_since_grounded <= pc.coyote_time) {
            rb.velocity.y = pc.jump_speed;
            pc.jump_buffer_timer = 0.0f;
            pc.time_since_grounded = 99.0f; // used up: no second jump from the same ground
            pc.jumping = true;
        }
        // Releasing early cuts the rise short.
        if (pc.jumping && !pc.input_jump && rb.velocity.y > 0.0f) {
            rb.velocity.y *= pc.jump_cut;
            pc.jumping = false;
        }
        if (rb.velocity.y <= 0.0f) pc.jumping = false;
        rb.gravity_scale = rb.velocity.y < 0.0f ? pc.fall_gravity_scale : 1.0f;
        rb.velocity.y = std::max(rb.velocity.y, -pc.max_fall_speed);
    }
}

void UpdateCameraFollow2D(World& world, f32 dt) {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    Entity target;
    EachEntityWith(world, [&](Entity e) {
        if (target.IsNull() && world.GetComponent<PlatformerController2D>(e) && world.GetComponent<Transform>(e)) target = e;
    });
    if (target.IsNull()) return;
    const Vec3 at = world.GetComponent<Transform>(target)->position;
    if (!std::isfinite(at.x) || !std::isfinite(at.y)) return;
    std::vector<Entity> cameras;
    EachEntityWith(world, [&](Entity e) {
        if (world.GetComponent<CameraFollow2D>(e) && world.GetComponent<Transform>(e)) cameras.push_back(e);
    });
    for (Entity e : cameras) {
        const CameraFollow2D& follow = *world.GetComponent<CameraFollow2D>(e);
        Transform& t = *world.GetComponent<Transform>(e);
        if (!std::isfinite(follow.smoothing) || follow.smoothing < 0.0f ||
            !std::isfinite(follow.offset.x) || !std::isfinite(follow.offset.y) ||
            !std::isfinite(t.position.x) || !std::isfinite(t.position.y)) continue;
        if (follow.use_bounds &&
            (!std::isfinite(follow.min_bounds.x) || !std::isfinite(follow.min_bounds.y) ||
             !std::isfinite(follow.max_bounds.x) || !std::isfinite(follow.max_bounds.y))) continue;
        f32 goal_x = at.x + follow.offset.x, goal_y = at.y + follow.offset.y;
        if (follow.use_bounds) {
            goal_x = std::clamp(goal_x, follow.min_bounds.x, std::max(follow.min_bounds.x, follow.max_bounds.x));
            goal_y = std::clamp(goal_y, follow.min_bounds.y, std::max(follow.min_bounds.y, follow.max_bounds.y));
        }
        const f32 k = follow.smoothing > 0.0f ? 1.0f - std::exp(-follow.smoothing * dt) : 1.0f;
        t.position.x += (goal_x - t.position.x) * k;
        t.position.y += (goal_y - t.position.y) * k;
        if (follow.use_bounds) {
            t.position.x = std::clamp(t.position.x, follow.min_bounds.x, std::max(follow.min_bounds.x, follow.max_bounds.x));
            t.position.y = std::clamp(t.position.y, follow.min_bounds.y, std::max(follow.min_bounds.y, follow.max_bounds.y));
        }
    }
}

void RegisterPlatformerComponents() {
    (void)GetComponentId<PlatformerController2D>();
    (void)GetComponentId<CameraFollow2D>();
}

} // namespace aether::sprite2d
