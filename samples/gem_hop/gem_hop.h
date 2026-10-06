#pragma once

// "Gem Hop": a small 3D platformer used as the engine's 3D sample game.
//
// Hop across floating platforms, collect every gem, then reach the goal pad.
// Falling into the void sends you back to the start. All gameplay lives here,
// free of platform and rendering code, so it runs headless (and is
// unit-tested); the Win32 software-3D front end is main.cpp.
//
// The player and gems are entities in an aether::World. Units are metres,
// +y is up, the camera is a third-person orbit controlled by the front end.

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/math/vec.h"

#include <vector>

namespace aether::gem_hop {

struct Transform {
    Vec3 pos; // player: bottom-centre of its box; gem: centre
};

struct Body {
    Vec3 vel;
    f32 half_w = 0.3f; // half extent in x and z
    f32 height = 1.0f;
    bool grounded = false;
};

struct Player {
    f32 coyote = 0;
    f32 jump_buffer = 0;
};

struct Gem {};

// An axis-aligned slab: top surface at `top`, one metre thick.
struct Platform {
    f32 cx, top, cz;
    f32 sx, sz; // full size in x and z
    f32 MinX() const { return cx - sx * 0.5f; }
    f32 MaxX() const { return cx + sx * 0.5f; }
    f32 MinZ() const { return cz - sz * 0.5f; }
    f32 MaxZ() const { return cz + sz * 0.5f; }
    f32 Bottom() const { return top - kThickness; }
    static constexpr f32 kThickness = 1.0f;
};

struct Input {
    f32 move_x = 0; // world-space desired direction (length <= 1)
    f32 move_z = 0;
    bool jump = false; // held; a fresh press is detected internally
};

enum class State : u8 { Playing, Won };

struct Config {
    f32 move_speed = 6.0f;
    f32 gravity = 25.0f;
    f32 jump_speed = 9.0f;
    f32 max_fall = 30.0f;
    f32 coyote_time = 0.1f;
    f32 jump_buffer_time = 0.1f;
    f32 void_y = -8.0f;
};

class Game {
public:
    static constexpr f32 kStep = 1.0f / 60.0f;

    Game();
    // The last platform is the goal pad.
    Game(std::vector<Platform> platforms, std::vector<Vec3> gems, Vec3 start);

    void Step(const Input& input);
    void Restart();

    // Platforms in route order; the last one is the goal pad.
    static std::vector<Platform> DefaultPlatforms();
    static std::vector<Vec3> DefaultGems();

    const std::vector<Platform>& Platforms() const { return platforms_; }
    const Platform& GoalPad() const { return platforms_.back(); }
    // The goal pad is "live" once every gem is collected.
    bool GoalActive() const { return gems_collected_ == gems_total_; }

    World& GetWorld() { return world_; }
    const World& GetWorld() const { return world_; }
    Entity PlayerEntity() const { return player_; }
    const Vec3& PlayerPos() const { return world_.GetComponent<Transform>(player_)->pos; }
    const Body& PlayerBody() const { return *world_.GetComponent<Body>(player_); }
    void TeleportPlayer(const Vec3& pos);

    i32 GemsCollected() const { return gems_collected_; }
    i32 GemsTotal() const { return gems_total_; }
    i32 Deaths() const { return deaths_; }
    f32 Time() const { return time_; }
    State GetState() const { return state_; }
    Config config;

private:
    void Build();
    bool Overlaps(const Vec3& pos, const Body& b) const;
    void Move(Transform& t, Body& b);

    std::vector<Platform> platforms_;
    std::vector<Vec3> gem_positions_;
    Vec3 start_;
    World world_;
    Entity player_ = kNullEntity;
    std::vector<Entity> gems_;
    i32 gems_total_ = 0;
    i32 gems_collected_ = 0;
    i32 deaths_ = 0;
    f32 time_ = 0;
    bool prev_jump_ = false;
    State state_ = State::Playing;
};

} // namespace aether::gem_hop
