#include "beacon_run.h"

#include <algorithm>
#include <cmath>

namespace aether::beacon_run {

namespace {
constexpr f32 kPlayerSpeed = 7.0f;
constexpr f32 kBeaconRadiusSq = 0.62f * 0.62f;
constexpr f32 kDroneRadiusSq = 0.9f * 0.9f;
}

Game::Game() { Build(); }

void Game::Build() {
    world_ = World{};
    beacons_.clear();
    drones_.clear();
    collected_ = 0;
    total_ = 0;
    hits_remaining_ = 3;
    time_ = 0;
    invulnerable_ = 0;
    state_ = State::Playing;

    player_ = world_.CreateEntity(Position{2.0f, 2.0f}, Player{});
    constexpr Position beacon_positions[] = {
        {5.0f, 3.0f}, {11.0f, 2.0f}, {19.0f, 3.0f}, {3.0f, 8.0f},
        {9.0f, 7.0f}, {16.0f, 8.0f}, {21.0f, 12.0f}, {11.0f, 14.0f},
    };
    for (const Position& p : beacon_positions) {
        beacons_.push_back(world_.CreateEntity(p, Beacon{}));
    }
    total_ = static_cast<i32>(beacons_.size());

    drones_.push_back(world_.CreateEntity(Position{7.0f, 5.0f}, Drone{4.0f}));
    drones_.push_back(world_.CreateEntity(Position{17.0f, 10.0f}, Drone{-3.5f}));
    drones_.push_back(world_.CreateEntity(Position{12.0f, 4.0f}, Drone{3.0f}));
}

void Game::Restart() { Build(); }

void Game::Step(const Input& input) {
    if (state_ != State::Playing) {
        return;
    }

    time_ += kStep;
    invulnerable_ = std::max(0.0f, invulnerable_ - kStep);
    Position& player = *world_.GetComponent<Position>(player_);

    const f32 length_sq = input.move_x * input.move_x + input.move_y * input.move_y;
    const f32 scale = length_sq > 1.0f ? 1.0f / std::sqrt(length_sq) : 1.0f;
    player.x = std::clamp(player.x + input.move_x * scale * kPlayerSpeed * kStep, 0.5f, kWidth - 0.5f);
    player.y = std::clamp(player.y + input.move_y * scale * kPlayerSpeed * kStep, 0.5f, kHeight - 0.5f);

    for (usize i = 0; i < beacons_.size();) {
        const Position& beacon = *world_.GetComponent<Position>(beacons_[i]);
        const f32 dx = beacon.x - player.x;
        const f32 dy = beacon.y - player.y;
        if (dx * dx + dy * dy < kBeaconRadiusSq) {
            world_.DestroyEntity(beacons_[i]);
            beacons_[i] = beacons_.back();
            beacons_.pop_back();
            ++collected_;
        } else {
            ++i;
        }
    }

    for (Entity drone_entity : drones_) {
        Position& drone_pos = *world_.GetComponent<Position>(drone_entity);
        Drone& drone = *world_.GetComponent<Drone>(drone_entity);
        drone_pos.x += drone.vx * kStep;
        if (drone_pos.x < 1.2f || drone_pos.x > kWidth - 1.2f) {
            drone_pos.x = std::clamp(drone_pos.x, 1.2f, kWidth - 1.2f);
            drone.vx = -drone.vx;
        }

        const f32 dx = drone_pos.x - player.x;
        const f32 dy = drone_pos.y - player.y;
        if (invulnerable_ == 0 && dx * dx + dy * dy < kDroneRadiusSq) {
            --hits_remaining_;
            invulnerable_ = 1.0f;
            player = {2.0f, 2.0f};
            if (hits_remaining_ == 0) {
                state_ = State::Lost;
                return;
            }
        }
    }

    if (collected_ == total_) {
        state_ = State::Won;
    }
}

} // namespace aether::beacon_run
