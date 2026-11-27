#pragma once

#include "aether/ecs/world.h"

#include <array>
#include <vector>

namespace lanternwake {

inline constexpr int kWidth = 1280;
inline constexpr int kHeight = 800;
inline constexpr float kStep = 1.0f / 60.0f;

enum class Phase { Title, Playing, Paused, Won, Lost };
enum class EventType { Start, Pickup, Dash, Pulse, Hurt, ShrineLit, Victory, Defeat };

struct Position { float x = 0, y = 0; };
struct Keeper {
    float facing_x = 0, facing_y = 1;
    float invulnerable = 0, dash_time = 0, dash_cooldown = 0;
    float pulse_cooldown = 0, pulse_age = 10, energy = 100;
    int health = 5, carried = 0;
    float stride = 0;
};
struct Mote { int index = 0; bool collected = false; };
struct Shrine { int index = 0; bool lit = false; float charge = 0; };
struct Wraith {
    float home_x = 0, home_y = 0, phase = 0;
    float stunned = 0, alert = 0;
};
struct Input {
    float move_x = 0, move_y = 0;
    bool interact = false;
    // Commands are pressed edges; the host consumes each edge once, even if
    // a video frame advances more than one fixed simulation step.
    bool start = false, pause = false, restart = false, dash = false, pulse = false;
};
struct Event { EventType type; float x = 0, y = 0; int value = 0; };

class Game {
public:
    Game();
    void Step(const Input& input);
    void Restart();
    [[nodiscard]] Phase State() const { return state_; }
    [[nodiscard]] const aether::World& World() const { return world_; }
    [[nodiscard]] const Position& PlayerPosition() const;
    [[nodiscard]] const Keeper& Player() const;
    [[nodiscard]] float Time() const { return time_; }
    [[nodiscard]] int LitShrines() const { return lit_; }
    [[nodiscard]] int Collected() const { return collected_; }
    [[nodiscard]] int Hits() const { return hits_; }
    [[nodiscard]] int Score() const;
    [[nodiscard]] int NearbyShrine() const;
    [[nodiscard]] float ChannelProgress() const;
    [[nodiscard]] bool NearGate() const;
    [[nodiscard]] const std::vector<Event>& Events() const { return events_; }
    [[nodiscard]] static const std::array<Position, 3>& ShrinePositions();
    [[nodiscard]] static const std::array<Position, 12>& MotePositions();
    [[nodiscard]] static Position GatePosition() { return {640.0f, 164.0f}; }

private:
    void Build();
    aether::World world_;
    aether::Entity player_{};
    std::array<aether::Entity, 3> shrines_{};
    std::array<aether::Entity, 12> motes_{};
    std::array<aether::Entity, 6> wraiths_{};
    Phase state_ = Phase::Title;
    float time_ = 0;
    int lit_ = 0, collected_ = 0, hits_ = 0;
    std::vector<Event> events_;
};

} // namespace lanternwake
