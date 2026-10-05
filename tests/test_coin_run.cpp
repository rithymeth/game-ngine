#include "coin_run.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::coin_run;

namespace {

void Run(Game& g, Input in, int steps) {
    for (int i = 0; i < steps; ++i) {
        g.Step(in);
    }
}

Input Right() {
    Input in;
    in.right = true;
    return in;
}

// A tiny open room: floor on row 3, player starts at column 1.
Game Room(const char* row1 = "#......#", const char* row2 = "#.P....#") {
    return Game({"########", row1, row2, "########"});
}

} // namespace

AETHER_TEST(CoinRun_DefaultLevelIsWellFormed) {
    const auto& level = Game::DefaultLevel();
    AETHER_CHECK(!level.empty());
    for (const auto& row : level) {
        AETHER_CHECK(row.size() == level[0].size());
    }
    Game g;
    AETHER_CHECK(g.CoinsTotal() == 10);
    AETHER_CHECK(!g.DoorOpen());
    AETHER_CHECK(g.GetState() == State::Playing);
}

AETHER_TEST(CoinRun_PlayerFallsAndLands) {
    Game g = Room();
    Run(g, Input{}, 60);
    AETHER_CHECK(g.PlayerBody().grounded);
    AETHER_CHECK_NEAR(g.PlayerPos().y, 3.0f, 0.01);
}

AETHER_TEST(CoinRun_WallsStopMovement) {
    Game g = Room();
    Run(g, Right(), 120);
    AETHER_CHECK(g.PlayerPos().x < 7.0f);
    AETHER_CHECK(g.PlayerPos().x > 6.0f);
}

AETHER_TEST(CoinRun_JumpRisesAboutThreeTiles) {
    Game g = Game({"##########", "#........#", "#........#", "#........#", "#........#", "#.P......#", "##########"});
    Run(g, Input{}, 30);
    f32 floor_y = g.PlayerPos().y;
    Input jump;
    jump.jump = true;
    f32 top = floor_y;
    for (int i = 0; i < 60; ++i) {
        g.Step(jump);
        top = std::min(top, g.PlayerPos().y);
    }
    f32 height = floor_y - top;
    AETHER_CHECK(height > 2.5f);
    AETHER_CHECK(height < 3.2f);
}

AETHER_TEST(CoinRun_ReleasingJumpEarlyJumpsLower) {
    auto jump_height = [](int held_frames) {
        Game g = Game({"##########", "#........#", "#........#", "#........#", "#........#", "#.P......#", "##########"});
        Run(g, Input{}, 30);
        f32 floor_y = g.PlayerPos().y;
        f32 top = floor_y;
        Input jump;
        jump.jump = true;
        for (int i = 0; i < 60; ++i) {
            g.Step(i < held_frames ? jump : Input{});
            top = std::min(top, g.PlayerPos().y);
        }
        return floor_y - top;
    };
    AETHER_CHECK(jump_height(3) < jump_height(30) - 0.5f);
}

AETHER_TEST(CoinRun_CannotJumpMidAir) {
    Game g = Game({"##########", "#........#", "#........#", "#........#", "#........#", "#.P......#", "##########"});
    Run(g, Input{}, 30);
    Input jump;
    jump.jump = true;
    Run(g, jump, 5); // takes off
    Run(g, Input{}, 1);
    f32 vy_before = g.PlayerBody().vy;
    Run(g, jump, 1); // fresh press in the air: ignored
    AETHER_CHECK(g.PlayerBody().vy >= vy_before);
}

AETHER_TEST(CoinRun_CollectsCoinsAndRemovesEntities) {
    Game g = Room("#......#", "#.PC...#");
    AETHER_CHECK(g.CoinsTotal() == 1);
    usize before = g.GetWorld().EntityCount();
    Run(g, Right(), 30);
    AETHER_CHECK(g.CoinsCollected() == 1);
    AETHER_CHECK(g.GetWorld().EntityCount() == before - 1);
    AETHER_CHECK(g.DoorOpen());
}

AETHER_TEST(CoinRun_DoorBlocksUntilAllCoinsCollected) {
    // Coin sits behind the player; the door is between the player and the goal.
    Game g({"#########", "#.......#", "#CP..D.G#", "#########"});
    AETHER_CHECK(!g.DoorOpen());
    Run(g, Right(), 90);
    AETHER_CHECK(g.GetState() == State::Playing);
    AETHER_CHECK(g.PlayerPos().x < 5.0f); // stopped by the closed door

    Input left;
    left.left = true;
    Run(g, left, 60); // go get the coin
    AETHER_CHECK(g.CoinsCollected() == 1);
    AETHER_CHECK(g.DoorOpen());
    Run(g, Right(), 120);
    AETHER_CHECK(g.GetState() == State::Won);
}

AETHER_TEST(CoinRun_SpikesRespawnAtStart) {
    Game g({"#########", "#.......#", "#.P..^..#", "#########"});
    for (int i = 0; i < 120 && g.Deaths() == 0; ++i) {
        g.Step(Right());
    }
    AETHER_CHECK(g.Deaths() == 1);
    AETHER_CHECK_NEAR(g.PlayerPos().x, 2.5f, 1e-4); // back at the start the moment it died
}

AETHER_TEST(CoinRun_FallingOutOfTheWorldRespawns) {
    Game g({"#########", "#.......#", "#.P....##"});
    Run(g, Right(), 1);
    g.TeleportPlayer(4.5f, 2.9f);
    Run(g, Input{}, 120);
    AETHER_CHECK(g.Deaths() >= 1);
}

AETHER_TEST(CoinRun_WinFreezesClockAndRestartResets) {
    Game g({"#######", "#.....#", "#.P.G.#", "#######"});
    Run(g, Right(), 60);
    AETHER_CHECK(g.GetState() == State::Won);
    f32 t = g.Time();
    Run(g, Right(), 30);
    AETHER_CHECK_NEAR(g.Time(), t, 1e-6);

    g.Restart();
    AETHER_CHECK(g.GetState() == State::Playing);
    AETHER_CHECK(g.Time() == 0.0f);
    AETHER_CHECK_NEAR(g.PlayerPos().x, 2.5f, 1e-4);
}

// Scripted playthrough of the shipped level: proves it can actually be beaten.
// The "player" runs right the whole way and holds jump for `hold` seconds
// whenever it crosses a trigger x -- just enough to follow the intended route.
AETHER_TEST(CoinRun_ShippedLevelCanBeBeaten) {
    struct Jump {
        f32 x;
        f32 hold;
    };
    const std::vector<Jump> jumps = {
        {5.0f, 0.5f},  // onto platform A (coins 1-2)
        {9.6f, 0.5f},  // hop to platform B (coins 3-4)
        {20.0f, 0.5f}, // leap the pit (coins 5-6 at the apex)
        {26.0f, 0.5f}, // onto platform C (coins 7-8)
        {31.6f, 0.5f}, // hop to platform D (coins 9-10)
        {37.0f, 0.5f}, // off D, over the spikes, past the (now open) door
    };

    Game g;
    size_t next = 0;
    f32 hold = 0;
    for (int i = 0; i < 60 * 60 && g.GetState() == State::Playing && g.Deaths() == 0; ++i) {
        if (next < jumps.size() && g.PlayerPos().x >= jumps[next].x) {
            hold = jumps[next++].hold;
        }
        Input in;
        in.right = true;
        if (hold > 0) {
            in.jump = true;
            hold -= Game::kStep;
        }
        g.Step(in);
    }
    if (g.GetState() != State::Won) {
        std::printf("  playthrough stopped at x=%.2f y=%.2f coins=%d deaths=%d\n", g.PlayerPos().x,
                    g.PlayerPos().y, g.CoinsCollected(), g.Deaths());
    }
    AETHER_CHECK(g.GetState() == State::Won);
    AETHER_CHECK(g.CoinsCollected() == 10);
    AETHER_CHECK(g.Deaths() == 0);
}
