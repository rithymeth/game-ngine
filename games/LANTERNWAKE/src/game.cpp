#include "game.h"

#include <algorithm>
#include <cmath>

namespace lanternwake {
namespace {

constexpr float kWalkSpeed = 170.0f;
constexpr float kDashSpeed = 500.0f;
constexpr float kOfferingTime = 0.8f;
constexpr float kTimerEpsilon = 0.00001f;
constexpr std::array<Position, 6> kShadeHomes{{
    {275, 585}, {444, 425}, {600, 254}, {1030, 390}, {996, 620}, {805, 460},
}};

float DistanceSquared(Position a, Position b) {
    const float x = a.x - b.x, y = a.y - b.y;
    return x * x + y * y;
}

float Elapse(float timer, float dt = kStep) {
    return timer <= dt + kTimerEpsilon ? 0.0f : timer - dt;
}

Position Movement(const Input& input) {
    Position move{
        std::isfinite(input.move_x) ? std::clamp(input.move_x, -1.0f, 1.0f) : 0.0f,
        std::isfinite(input.move_y) ? std::clamp(input.move_y, -1.0f, 1.0f) : 0.0f,
    };
    const float length = std::hypot(move.x, move.y);
    if (length > 1) { move.x /= length; move.y /= length; }
    return move;
}

void MoveKeeper(Position& position, float dx, float dy) {
    // Resolve axes separately so the north corridor has solid side walls.
    const bool in_corridor = position.y < 195;
    position.x = std::clamp(position.x + dx, in_corridor ? 590.0f : 115.0f,
                                             in_corridor ? 690.0f : 1165.0f);
    const float north = position.x >= 590 && position.x <= 690 ? 150.0f : 195.0f;
    position.y = std::clamp(position.y + dy, north, 715.0f);
}

void ConstrainShade(Position& position) {
    position.x = std::clamp(position.x, 115.0f, 1165.0f);
    position.y = std::clamp(position.y, 195.0f, 715.0f);
}

void Approach(Position& position, Position target, float speed) {
    const float dx = target.x - position.x, dy = target.y - position.y;
    const float distance = std::hypot(dx, dy);
    if (distance > 0.0001f) {
        const float scale = std::min(speed * kStep, distance) / distance;
        position.x += dx * scale;
        position.y += dy * scale;
    }
}

} // namespace

const std::array<Position, 3>& Game::ShrinePositions() {
    static constexpr std::array<Position, 3> positions{{{326, 286}, {964, 286}, {645, 549}}};
    return positions;
}

const std::array<Position, 12>& Game::MotePositions() {
    static constexpr std::array<Position, 12> positions{{
        {208, 277}, {472, 289}, {235, 427}, {390, 616},
        {539, 467}, {564, 678}, {763, 648}, {795, 541},
        {1100, 651}, {1076, 458}, {1105, 255}, {843, 256},
    }};
    return positions;
}

Game::Game() { Build(); }

void Game::Build() {
    world_ = aether::World{};
    events_.clear();
    state_ = Phase::Title;
    time_ = 0;
    lit_ = collected_ = hits_ = 0;
    player_ = world_.CreateEntity(Position{640, 650}, Keeper{});
    for (int i = 0; i < 3; ++i)
        shrines_[i] = world_.CreateEntity(ShrinePositions()[i], Shrine{i});
    for (int i = 0; i < 12; ++i)
        motes_[i] = world_.CreateEntity(MotePositions()[i], Mote{i});
    for (int i = 0; i < 6; ++i) {
        const float phase = static_cast<float>(i) * 1.04719755f;
        const Position home = kShadeHomes[i];
        const Position position{home.x + std::cos(phase) * 44, home.y + std::sin(phase) * 32};
        wraiths_[i] = world_.CreateEntity(position, Wraith{home.x, home.y, phase});
    }
}

void Game::Restart() {
    Build();
    state_ = Phase::Playing;
    events_.push_back({EventType::Start, 640, 650});
}

const Position& Game::PlayerPosition() const { return *world_.GetComponent<Position>(player_); }
const Keeper& Game::Player() const { return *world_.GetComponent<Keeper>(player_); }

int Game::Score() const {
    return std::max(0, lit_ * 1000 + collected_ * 100 + Player().health * 200
                       - static_cast<int>(time_ * 10));
}

int Game::NearbyShrine() const {
    int nearest = -1;
    float distance = 75 * 75;
    for (int i = 0; i < 3; ++i) {
        if (world_.GetComponent<Shrine>(shrines_[i])->lit) continue;
        const float candidate = DistanceSquared(PlayerPosition(), ShrinePositions()[i]);
        if (candidate <= distance) { nearest = i; distance = candidate; }
    }
    return nearest;
}

float Game::ChannelProgress() const {
    const int shrine = NearbyShrine();
    return shrine < 0 ? 0 : std::clamp(world_.GetComponent<Shrine>(shrines_[shrine])->charge / kOfferingTime, 0.0f, 1.0f);
}

bool Game::NearGate() const { return DistanceSquared(PlayerPosition(), GatePosition()) <= 60 * 60; }

void Game::Step(const Input& input) {
    events_.clear();
    if (state_ == Phase::Title) {
        if (input.start) {
            state_ = Phase::Playing;
            events_.push_back({EventType::Start, 640, 650});
        }
        return;
    }
    if (state_ == Phase::Won || state_ == Phase::Lost) {
        if (input.restart) Restart();
        return;
    }
    if (state_ == Phase::Paused) {
        if (input.restart) Restart();
        else if (input.pause) state_ = Phase::Playing;
        return;
    }
    if (input.pause) { state_ = Phase::Paused; return; }

    time_ += kStep;
    Position& position = *world_.GetComponent<Position>(player_);
    Keeper& keeper = *world_.GetComponent<Keeper>(player_);
    keeper.invulnerable = Elapse(keeper.invulnerable);
    keeper.dash_cooldown = Elapse(keeper.dash_cooldown);
    keeper.pulse_cooldown = Elapse(keeper.pulse_cooldown);
    keeper.pulse_age = std::min(10.0f, keeper.pulse_age + kStep);
    keeper.energy = std::min(100.0f, keeper.energy + 10 * kStep);

    const Position move = Movement(input);
    const float movement_length = std::hypot(move.x, move.y);
    const bool walking = movement_length > 0.0001f;
    if (keeper.dash_time == 0 && walking) {
        keeper.facing_x = move.x / movement_length;
        keeper.facing_y = move.y / movement_length;
    }
    if (input.dash && keeper.dash_cooldown == 0) {
        keeper.dash_time = 0.18f;
        keeper.dash_cooldown = 1.05f;
        events_.push_back({EventType::Dash, position.x, position.y});
    }
    const Position previous = position;
    const float dash_dt = std::min(kStep, keeper.dash_time);
    const bool dashing = dash_dt > 0;
    const float walk_dt = kStep - dash_dt;
    MoveKeeper(position, keeper.facing_x * kDashSpeed * dash_dt + move.x * kWalkSpeed * walk_dt,
                         keeper.facing_y * kDashSpeed * dash_dt + move.y * kWalkSpeed * walk_dt);
    keeper.dash_time = Elapse(keeper.dash_time, dash_dt);
    keeper.stride += std::sqrt(DistanceSquared(previous, position)) * 0.09f;

    if (input.pulse && keeper.pulse_cooldown == 0 && keeper.energy >= 28) {
        keeper.energy -= 28;
        keeper.pulse_cooldown = 1;
        keeper.pulse_age = 0;
        events_.push_back({EventType::Pulse, position.x, position.y});
        for (const aether::Entity entity : wraiths_) {
            Position& shade = *world_.GetComponent<Position>(entity);
            Wraith& wraith = *world_.GetComponent<Wraith>(entity);
            if (DistanceSquared(shade, position) > 160 * 160) continue;
            const float dx = shade.x - position.x, dy = shade.y - position.y;
            const float distance = std::hypot(dx, dy);
            shade.x += (distance > 0.001f ? dx / distance : std::cos(wraith.phase)) * 48;
            shade.y += (distance > 0.001f ? dy / distance : std::sin(wraith.phase)) * 48;
            ConstrainShade(shade);
            wraith.stunned = 2.3f;
            wraith.alert = 0;
        }
    }

    for (const aether::Entity entity : wraiths_) {
        Position& shade = *world_.GetComponent<Position>(entity);
        Wraith& wraith = *world_.GetComponent<Wraith>(entity);
        wraith.phase += kStep * 0.55f;
        if (wraith.stunned > 0) { wraith.stunned = Elapse(wraith.stunned); continue; }
        if (DistanceSquared(shade, position) <= 180 * 180) {
            wraith.alert = std::min(1.0f, wraith.alert + kStep * 3);
            Approach(shade, position, 80);
        } else {
            wraith.alert = Elapse(wraith.alert, kStep * 2);
            Approach(shade, {wraith.home_x + std::cos(wraith.phase) * 44,
                             wraith.home_y + std::sin(wraith.phase) * 32}, 42);
        }
        ConstrainShade(shade);
    }

    bool hurt = false;
    if (lit_ < 3 && !dashing && keeper.invulnerable == 0) {
        for (const aether::Entity entity : wraiths_) {
            if (world_.GetComponent<Wraith>(entity)->stunned > 0) continue;
            if (DistanceSquared(position, *world_.GetComponent<Position>(entity)) > 28 * 28) continue;
            keeper.health = std::max(0, keeper.health - 1);
            keeper.invulnerable = 1.4f;
            ++hits_;
            hurt = true;
            events_.push_back({EventType::Hurt, position.x, position.y, keeper.health});
            break; // Overlapping shades still cause just one hit per immunity window.
        }
    }
    if (keeper.health == 0) {
        state_ = Phase::Lost;
        for (const aether::Entity entity : shrines_) world_.GetComponent<Shrine>(entity)->charge = 0;
        events_.push_back({EventType::Defeat, position.x, position.y, Score()});
        return;
    }

    for (const aether::Entity entity : motes_) {
        Mote& mote = *world_.GetComponent<Mote>(entity);
        const Position& ember = *world_.GetComponent<Position>(entity);
        if (mote.collected || DistanceSquared(position, ember) > 25 * 25) continue;
        mote.collected = true;
        ++keeper.carried;
        ++collected_;
        events_.push_back({EventType::Pickup, ember.x, ember.y, collected_});
    }

    const int nearby = NearbyShrine();
    for (int i = 0; i < 3; ++i) {
        Shrine& shrine = *world_.GetComponent<Shrine>(shrines_[i]);
        if (shrine.lit) continue;
        if (i != nearby || !input.interact || walking || dashing || hurt || keeper.carried < 4) {
            shrine.charge = 0;
            continue;
        }
        shrine.charge = std::min(kOfferingTime, shrine.charge + kStep);
        if (shrine.charge + kTimerEpsilon >= kOfferingTime) {
            shrine.charge = kOfferingTime;
            shrine.lit = true;
            keeper.carried -= 4; // Consume only after the full uninterrupted offering.
            ++lit_;
            const Position center = ShrinePositions()[i];
            events_.push_back({EventType::ShrineLit, center.x, center.y, lit_});
        }
    }
    if (lit_ == 3 && input.interact && NearGate()) {
        state_ = Phase::Won;
        events_.push_back({EventType::Victory, position.x, position.y, Score()});
    }
}

} // namespace lanternwake
