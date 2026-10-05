#include "gem_hop.h"

#include <algorithm>
#include <cmath>

namespace aether::gem_hop {

namespace {
constexpr f32 kEps = 1e-4f;
}

// A snaking route: each platform is a short hop from the one before it, and
// rises a little. The last entry is the goal pad.
std::vector<Platform> Game::DefaultPlatforms() {
    return {
        {0.0f, 0.0f, 0.0f, 6, 6},    // start
        {0.0f, 0.5f, 7.0f, 4, 4},    //
        {4.0f, 1.0f, 11.5f, 4, 4},   //
        {9.0f, 1.5f, 13.0f, 3, 3},   //
        {13.0f, 2.0f, 9.5f, 3, 3},   //
        {17.0f, 2.5f, 12.0f, 3, 3},  //
        {21.0f, 3.0f, 9.0f, 3, 3},   //
        {25.0f, 3.5f, 12.0f, 3, 3},  //
        {30.0f, 4.0f, 12.0f, 4, 4},  // goal pad
    };
}

std::vector<Vec3> Game::DefaultGems() {
    return {
        {-2.0f, 0.9f, -1.5f}, // bonus on the start platform
        {0.0f, 1.4f, 7.0f},   {4.0f, 1.9f, 11.5f}, {9.0f, 2.4f, 13.0f}, {13.0f, 2.9f, 9.5f},
        {17.0f, 3.4f, 12.0f}, {21.0f, 3.9f, 9.0f}, {25.0f, 4.4f, 12.0f},
    };
}

Game::Game() : Game(DefaultPlatforms(), DefaultGems(), {0.0f, 0.0f, 0.0f}) {}

Game::Game(std::vector<Platform> platforms, std::vector<Vec3> gems, Vec3 start)
    : platforms_(std::move(platforms)), gem_positions_(std::move(gems)), start_(start) {
    Build();
}

void Game::Build() {
    world_ = World{};
    gems_.clear();
    for (const Vec3& g : gem_positions_) {
        gems_.push_back(world_.CreateEntity(Transform{g}, Gem{}));
    }
    player_ = world_.CreateEntity(Transform{start_}, Body{}, Player{});
    gems_total_ = static_cast<i32>(gems_.size());
    gems_collected_ = 0;
    deaths_ = 0;
    time_ = 0;
    prev_jump_ = false;
    state_ = State::Playing;
}

void Game::Restart() { Build(); }

void Game::TeleportPlayer(const Vec3& pos) {
    world_.GetComponent<Transform>(player_)->pos = pos;
    Body& b = *world_.GetComponent<Body>(player_);
    b.vel = {0, 0, 0};
}

bool Game::Overlaps(const Vec3& p, const Body& b) const {
    for (const Platform& pl : platforms_) {
        if (p.x + b.half_w > pl.MinX() + kEps && p.x - b.half_w < pl.MaxX() - kEps &&
            p.z + b.half_w > pl.MinZ() + kEps && p.z - b.half_w < pl.MaxZ() - kEps &&
            p.y + b.height > pl.Bottom() + kEps && p.y < pl.top - kEps) {
            return true;
        }
    }
    return false;
}

void Game::Move(Transform& t, Body& b) {
    // One axis at a time; on a hit the move on that axis is undone and its
    // velocity zeroed. Steps are tiny (<= 0.5 m) next to platform sizes, so
    // this is stable. Vertical hits snap to the platform surface.
    Vec3 p = t.pos;

    p.x += b.vel.x * kStep;
    if (Overlaps(p, b)) {
        p.x = t.pos.x;
        b.vel.x = 0;
    }
    f32 z_before = p.z;
    p.z += b.vel.z * kStep;
    if (Overlaps(p, b)) {
        p.z = z_before;
        b.vel.z = 0;
    }

    f32 old_y = p.y;
    p.y += b.vel.y * kStep;
    b.grounded = false;
    if (Overlaps(p, b)) {
        if (b.vel.y <= 0) {
            // Land on the highest platform under the feet that we sank into.
            f32 surface = old_y;
            for (const Platform& pl : platforms_) {
                bool under = p.x + b.half_w > pl.MinX() + kEps && p.x - b.half_w < pl.MaxX() - kEps &&
                             p.z + b.half_w > pl.MinZ() + kEps && p.z - b.half_w < pl.MaxZ() - kEps;
                if (under && pl.top <= old_y + kEps && pl.top > p.y) {
                    surface = std::max(surface == old_y ? pl.top : surface, pl.top);
                }
            }
            p.y = surface;
            b.grounded = true;
        } else {
            p.y = old_y; // bumped a ceiling
        }
        b.vel.y = 0;
    }
    t.pos = p;
}

void Game::Step(const Input& in) {
    if (state_ != State::Playing) {
        return;
    }
    time_ += kStep;
    Transform& t = *world_.GetComponent<Transform>(player_);
    Body& b = *world_.GetComponent<Body>(player_);
    Player& pl = *world_.GetComponent<Player>(player_);

    f32 len = std::sqrt(in.move_x * in.move_x + in.move_z * in.move_z);
    f32 scale = len > 1.0f ? 1.0f / len : 1.0f;
    b.vel.x = in.move_x * scale * config.move_speed;
    b.vel.z = in.move_z * scale * config.move_speed;

    bool pressed = in.jump && !prev_jump_;
    prev_jump_ = in.jump;
    pl.jump_buffer = pressed ? config.jump_buffer_time : std::max(0.0f, pl.jump_buffer - kStep);
    pl.coyote = b.grounded ? config.coyote_time : std::max(0.0f, pl.coyote - kStep);
    if (pl.jump_buffer > 0 && pl.coyote > 0) {
        b.vel.y = config.jump_speed;
        b.grounded = false;
        pl.jump_buffer = pl.coyote = 0;
    }
    if (!in.jump && b.vel.y > 0) { // variable jump height
        b.vel.y = std::max(0.0f, b.vel.y - config.gravity * 2.0f * kStep);
    }
    b.vel.y = std::max(b.vel.y - config.gravity * kStep, -config.max_fall);

    Move(t, b);

    // Gems.
    Vec3 centre{t.pos.x, t.pos.y + b.height * 0.5f, t.pos.z};
    for (usize i = 0; i < gems_.size();) {
        if ((world_.GetComponent<Transform>(gems_[i])->pos - centre).LengthSq() < 0.9f * 0.9f) {
            world_.DestroyEntity(gems_[i]);
            gems_[i] = gems_.back();
            gems_.pop_back();
            ++gems_collected_;
        } else {
            ++i;
        }
    }

    // Goal: standing on the pad once it's live.
    const Platform& goal = GoalPad();
    if (GoalActive() && b.grounded && std::fabs(t.pos.y - goal.top) < 0.05f && t.pos.x > goal.MinX() &&
        t.pos.x < goal.MaxX() && t.pos.z > goal.MinZ() && t.pos.z < goal.MaxZ()) {
        state_ = State::Won;
    }

    if (t.pos.y < config.void_y) {
        ++deaths_;
        TeleportPlayer(start_);
    }
}

} // namespace aether::gem_hop
