#pragma once

// "Coin Run": a small 2D platformer used as the engine's sample game.
//
// Collect every coin to open the door, then walk through it to the goal.
// Spikes send you back to the start. All gameplay lives here, free of any
// platform or rendering code, so it runs headless (and is unit-tested); the
// Win32 front end is main.cpp.
//
// Entities live in an aether::World: coins are entities with Position +
// Coin, the player is an entity with Position + Body + Player. The level's
// static geometry is a tile grid parsed from ASCII.

#include "aether/core/base.h"
#include "aether/ecs/world.h"

#include <string>
#include <vector>

namespace aether::coin_run {

// Units are tiles; +y points down (screen space). For the player, Position is
// the bottom-centre of its box; for coins it is the centre.
struct Position {
    f32 x = 0;
    f32 y = 0;
};

struct Body {
    f32 vx = 0;
    f32 vy = 0;
    f32 w = 0.7f;
    f32 h = 0.9f;
    bool grounded = false;
};

struct Player {
    f32 coyote = 0;      // seconds left in which a jump is still allowed after leaving a ledge
    f32 jump_buffer = 0; // seconds left in which a recent jump press is still honoured
};

struct Coin {};

enum class Tile : u8 { Empty, Solid, Door, Spike, Goal };

struct Input {
    bool left = false;
    bool right = false;
    bool jump = false; // held; a fresh press is detected internally
};

enum class State : u8 { Playing, Won };

struct Config {
    f32 move_speed = 7.0f;
    f32 gravity = 40.0f;
    f32 jump_speed = 15.0f;
    f32 max_fall = 25.0f;
    f32 coyote_time = 0.1f;
    f32 jump_buffer_time = 0.1f;
};

class Game {
public:
    static constexpr f32 kStep = 1.0f / 60.0f;

    Game();
    // `rows` must all be the same length; see DefaultLevel() for the legend.
    explicit Game(const std::vector<std::string>& rows);

    // Advances the simulation by one fixed step.
    void Step(const Input& input);
    // Back to the initial state (coins, door, deaths and clock included).
    void Restart();

    static const std::vector<std::string>& DefaultLevel();

    i32 Width() const { return width_; }
    i32 Height() const { return height_; }
    // Out-of-range x counts as wall, y above the map as ceiling, y below as empty.
    Tile TileAt(i32 x, i32 y) const;
    // The door is solid until every coin is collected.
    bool DoorOpen() const { return coins_collected_ == coins_total_; }

    World& GetWorld() { return world_; }
    const World& GetWorld() const { return world_; }
    Entity PlayerEntity() const { return player_; }
    const Position& PlayerPos() const { return *world_.GetComponent<Position>(player_); }
    const Body& PlayerBody() const { return *world_.GetComponent<Body>(player_); }
    // Test/editor hook: place the player (clears velocity).
    void TeleportPlayer(f32 x, f32 y);

    i32 CoinsCollected() const { return coins_collected_; }
    i32 CoinsTotal() const { return coins_total_; }
    i32 Deaths() const { return deaths_; }
    f32 Time() const { return time_; }
    State GetState() const { return state_; }
    Config config;

private:
    void Build();
    bool Blocked(i32 tx, i32 ty) const;
    bool Overlaps(f32 x, f32 y, f32 w, f32 h) const;
    void MovePlayer(Position& p, Body& b);
    void Interact(Position& p, Body& b);
    void Die();

    std::vector<std::string> rows_;
    i32 width_ = 0;
    i32 height_ = 0;
    f32 start_x_ = 1;
    f32 start_y_ = 1;
    World world_;
    Entity player_ = kNullEntity;
    std::vector<Entity> coins_;
    i32 coins_total_ = 0;
    i32 coins_collected_ = 0;
    i32 deaths_ = 0;
    f32 time_ = 0;
    bool prev_jump_ = false;
    State state_ = State::Playing;
};

} // namespace aether::coin_run
