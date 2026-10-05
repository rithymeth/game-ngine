#include "coin_run.h"

#include <algorithm>
#include <cmath>

namespace aether::coin_run {

namespace {
constexpr f32 kEps = 1e-4f;
}

// Legend: '#' wall  '.' air  'P' player start  'C' coin  'D' door (solid until
// every coin is collected)  '^' spike  'G' goal.
const std::vector<std::string>& Game::DefaultLevel() {
    static const std::vector<std::string> level = {
        "################################################",
        "#...........................................#..#",
        "#...........................................#..#",
        "#...........................................#..#",
        "#...........................................#..#",
        "#...........................................#..#",
        "#...........................................#..#",
        "#...........................................#..#",
        "#............CC....................CC.......#..#",
        "#...........####......CC..........####......D..#",
        "#......CC....................CC.............D..#",
        "#.....####..................####............D..#",
        "#.P....................................^^...D.G#",
        "#####################....#######################",
        "#####################^^^^#######################",
        "################################################",
    };
    return level;
}

Game::Game() : Game(DefaultLevel()) {}

Game::Game(const std::vector<std::string>& rows) : rows_(rows) {
    height_ = static_cast<i32>(rows_.size());
    width_ = rows_.empty() ? 0 : static_cast<i32>(rows_[0].size());
    Build();
}

void Game::Build() {
    world_ = World{};
    coins_.clear();
    for (i32 y = 0; y < height_; ++y) {
        for (i32 x = 0; x < width_; ++x) {
            char c = rows_[y][x];
            if (c == 'C') {
                coins_.push_back(world_.CreateEntity(Position{x + 0.5f, y + 0.5f}, Coin{}));
            } else if (c == 'P') {
                start_x_ = x + 0.5f;
                start_y_ = y + 1.0f;
            }
        }
    }
    player_ = world_.CreateEntity(Position{start_x_, start_y_}, Body{}, Player{});
    coins_total_ = static_cast<i32>(coins_.size());
    coins_collected_ = 0;
    deaths_ = 0;
    time_ = 0;
    prev_jump_ = false;
    state_ = State::Playing;
}

void Game::Restart() { Build(); }

Tile Game::TileAt(i32 x, i32 y) const {
    if (x < 0 || x >= width_ || y < 0) {
        return Tile::Solid;
    }
    if (y >= height_) {
        return Tile::Empty;
    }
    switch (rows_[y][x]) {
    case '#': return Tile::Solid;
    case 'D': return Tile::Door;
    case '^': return Tile::Spike;
    case 'G': return Tile::Goal;
    default: return Tile::Empty;
    }
}

bool Game::Blocked(i32 tx, i32 ty) const {
    Tile t = TileAt(tx, ty);
    return t == Tile::Solid || (t == Tile::Door && !DoorOpen());
}

// Is the box with bottom-centre (x, y) and size (w, h) touching a blocking tile?
bool Game::Overlaps(f32 x, f32 y, f32 w, f32 h) const {
    i32 x0 = static_cast<i32>(std::floor(x - w * 0.5f + kEps));
    i32 x1 = static_cast<i32>(std::floor(x + w * 0.5f - kEps));
    i32 y0 = static_cast<i32>(std::floor(y - h + kEps));
    i32 y1 = static_cast<i32>(std::floor(y - kEps));
    for (i32 ty = y0; ty <= y1; ++ty) {
        for (i32 tx = x0; tx <= x1; ++tx) {
            if (Blocked(tx, ty)) {
                return true;
            }
        }
    }
    return false;
}

void Game::TeleportPlayer(f32 x, f32 y) {
    Position& p = *world_.GetComponent<Position>(player_);
    Body& b = *world_.GetComponent<Body>(player_);
    p.x = x;
    p.y = y;
    b.vx = b.vy = 0;
}

void Game::MovePlayer(Position& p, Body& b) {
    // Horizontal, then vertical, each resolved by pushing back to the tile edge.
    f32 nx = p.x + b.vx * kStep;
    if (Overlaps(nx, p.y, b.w, b.h)) {
        if (b.vx > 0) {
            nx = std::floor(nx + b.w * 0.5f) - b.w * 0.5f - kEps;
        } else {
            nx = std::ceil(nx - b.w * 0.5f) + b.w * 0.5f + kEps;
        }
        b.vx = 0;
    }
    p.x = nx;

    f32 ny = p.y + b.vy * kStep;
    b.grounded = false;
    if (Overlaps(p.x, ny, b.w, b.h)) {
        if (b.vy > 0) {
            ny = std::floor(ny) - kEps;
            b.grounded = true;
        } else {
            ny = std::ceil(ny - b.h) + b.h + kEps;
        }
        b.vy = 0;
    }
    p.y = ny;
}

void Game::Die() {
    ++deaths_;
    TeleportPlayer(start_x_, start_y_);
}

void Game::Interact(Position& p, Body& b) {
    // Coins: picked up when their centre is within reach of the player's centre.
    f32 cx = p.x;
    f32 cy = p.y - b.h * 0.5f;
    for (usize i = 0; i < coins_.size();) {
        const Position* c = world_.GetComponent<Position>(coins_[i]);
        f32 dx = c->x - cx;
        f32 dy = c->y - cy;
        if (dx * dx + dy * dy < 0.6f * 0.6f) {
            world_.DestroyEntity(coins_[i]);
            coins_[i] = coins_.back();
            coins_.pop_back();
            ++coins_collected_;
        } else {
            ++i;
        }
    }

    // Hazards and goal sample the tiles the box touches.
    i32 x0 = static_cast<i32>(std::floor(p.x - b.w * 0.5f));
    i32 x1 = static_cast<i32>(std::floor(p.x + b.w * 0.5f - kEps));
    i32 y0 = static_cast<i32>(std::floor(p.y - b.h));
    i32 y1 = static_cast<i32>(std::floor(p.y - kEps));
    for (i32 ty = y0; ty <= y1; ++ty) {
        for (i32 tx = x0; tx <= x1; ++tx) {
            Tile t = TileAt(tx, ty);
            if (t == Tile::Goal) {
                state_ = State::Won;
            } else if (t == Tile::Spike && p.y > ty + 0.5f) {
                Die();
                return;
            }
        }
    }
    if (p.y - b.h > static_cast<f32>(height_)) {
        Die(); // fell out of the world
    }
}

void Game::Step(const Input& in) {
    if (state_ != State::Playing) {
        return;
    }
    time_ += kStep;
    Position& p = *world_.GetComponent<Position>(player_);
    Body& b = *world_.GetComponent<Body>(player_);
    Player& pl = *world_.GetComponent<Player>(player_);

    b.vx = ((in.right ? 1.0f : 0.0f) - (in.left ? 1.0f : 0.0f)) * config.move_speed;

    bool pressed = in.jump && !prev_jump_;
    prev_jump_ = in.jump;
    pl.jump_buffer = pressed ? config.jump_buffer_time : std::max(0.0f, pl.jump_buffer - kStep);
    pl.coyote = b.grounded ? config.coyote_time : std::max(0.0f, pl.coyote - kStep);

    if (pl.jump_buffer > 0 && pl.coyote > 0) {
        b.vy = -config.jump_speed;
        b.grounded = false;
        pl.jump_buffer = pl.coyote = 0;
    }
    // Variable jump height: letting go early cuts the rise short.
    if (!in.jump && b.vy < 0) {
        b.vy = std::min(0.0f, b.vy + config.gravity * 2.0f * kStep);
    }
    b.vy = std::min(b.vy + config.gravity * kStep, config.max_fall);

    MovePlayer(p, b);
    Interact(p, b);
}

} // namespace aether::coin_run
