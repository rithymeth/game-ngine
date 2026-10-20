#pragma once

// "Beacon Run": a compact top-down arena game used as the engine's third
// playable sample. Collect every beacon while dodging the patrol drones.
// Gameplay is platform and renderer independent; main.cpp supplies a small
// Win32 software-rendered front end.

#include "aether/core/base.h"
#include "aether/ecs/world.h"

#include <vector>

namespace aether::beacon_run {

struct Position {
    f32 x = 0;
    f32 y = 0;
};

struct Player {};
struct Beacon {};

struct Drone {
    f32 vx = 0;
};

struct Input {
    f32 move_x = 0;
    f32 move_y = 0;
};

enum class State : u8 { Playing, Won, Lost };

class Game {
public:
    static constexpr f32 kStep = 1.0f / 60.0f;
    static constexpr f32 kWidth = 24.0f;
    static constexpr f32 kHeight = 16.0f;

    Game();

    void Step(const Input& input);
    void Restart();

    World& GetWorld() { return world_; }
    const World& GetWorld() const { return world_; }
    const Position& PlayerPos() const { return *world_.GetComponent<Position>(player_); }
    i32 BeaconsCollected() const { return collected_; }
    i32 BeaconsTotal() const { return total_; }
    i32 HitsRemaining() const { return hits_remaining_; }
    i32 HitsTaken() const { return 3 - hits_remaining_; }
    bool IsInvulnerable() const { return invulnerable_ > 0; }
    f32 Time() const { return time_; }
    State GetState() const { return state_; }

private:
    void Build();

    World world_;
    Entity player_ = kNullEntity;
    std::vector<Entity> beacons_;
    std::vector<Entity> drones_;
    i32 collected_ = 0;
    i32 total_ = 0;
    i32 hits_remaining_ = 3;
    f32 time_ = 0;
    f32 invulnerable_ = 0;
    State state_ = State::Playing;
};

} // namespace aether::beacon_run
