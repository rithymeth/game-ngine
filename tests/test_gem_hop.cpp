#include "gem_hop.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace aether;
using namespace aether::gem_hop;

namespace {

void Run(Game& g, const Input& in, int steps) {
    for (int i = 0; i < steps; ++i) {
        g.Step(in);
    }
}

Game Slab() {
    return Game({{0, 0, 0, 10, 10}, {0, 0, 0, 2, 2}}, {{0, 1, 3}}, {0, 0.5f, 0});
}

} // namespace

AETHER_TEST(GemHop_PlayerFallsAndLandsOnPlatform) {
    Game g = Slab();
    Run(g, Input{}, 60);
    AETHER_CHECK(g.PlayerBody().grounded);
    AETHER_CHECK_NEAR(g.PlayerPos().y, 0.0f, 0.01);
}

AETHER_TEST(GemHop_WalkingOffTheEdgeFalls) {
    Game g = Slab();
    Input right;
    right.move_x = 1;
    Run(g, right, 120);
    AETHER_CHECK(g.Deaths() >= 1); // fell into the void and respawned
}

AETHER_TEST(GemHop_JumpHeightAndNoAirJump) {
    Game g = Slab();
    Run(g, Input{}, 30);
    Input jump;
    jump.jump = true;
    f32 top = 0;
    for (int i = 0; i < 40; ++i) {
        g.Step(jump);
        top = std::max(top, g.PlayerPos().y);
    }
    AETHER_CHECK(top > 1.4f);
    AETHER_CHECK(top < 1.9f);

    // Press again while airborne: ignored.
    Game h = Slab();
    Run(h, Input{}, 30);
    Run(h, jump, 5);
    Run(h, Input{}, 1);
    f32 vy = h.PlayerBody().vel.y;
    Run(h, jump, 1);
    AETHER_CHECK(h.PlayerBody().vel.y <= vy);
}

AETHER_TEST(GemHop_CollectsGemsAndRemovesEntities) {
    Game g({{0, 0, 0, 10, 10}, {20, 0, 0, 4, 4}}, {{0, 1, 0}, {0, 1, 2}}, {0, 0, 0});
    usize before = g.GetWorld().EntityCount();
    Input fwd;
    fwd.move_z = 1;
    Run(g, fwd, 40);
    AETHER_CHECK(g.GemsCollected() == 2);
    AETHER_CHECK(g.GetWorld().EntityCount() == before - 2);
    AETHER_CHECK(g.GoalActive());
}

AETHER_TEST(GemHop_GoalNeedsAllGems) {
    // Start on the goal pad itself; a gem far away keeps it dormant.
    Game g({{0, 0, 0, 10, 10}}, {{0, 1, 40}}, {0, 0, 0});
    Run(g, Input{}, 60);
    AETHER_CHECK(!g.GoalActive());
    AETHER_CHECK(g.GetState() == State::Playing);

    Game w({{0, 0, 0, 10, 10}}, {}, {0, 0, 0});
    Run(w, Input{}, 60);
    AETHER_CHECK(w.GetState() == State::Won);
}

AETHER_TEST(GemHop_VoidRespawnsAtStart) {
    Game g = Slab();
    g.TeleportPlayer({50, 5, 50});
    for (int i = 0; i < 300 && g.Deaths() == 0; ++i) {
        g.Step(Input{});
    }
    AETHER_CHECK(g.Deaths() == 1);
    AETHER_CHECK_NEAR(g.PlayerPos().x, 0.0f, 1e-4);
    AETHER_CHECK_NEAR(g.PlayerPos().y, 0.5f, 1e-4);
}

AETHER_TEST(GemHop_PlatformSidesBlockMovement) {
    // A block whose face you can't walk through: it spans x = 3..7.
    Game g({{0, 0, 0, 20, 20}, {5, 1, 0, 4, 4}}, {{0, 1, 40}}, {0, 0, 0});
    Input right;
    right.move_x = 1;
    Run(g, right, 90);
    AETHER_CHECK(g.PlayerPos().x < 3.0f); // stopped at the block's face (x = 3)
}

AETHER_TEST(GemHop_RestartResets) {
    Game g({{0, 0, 0, 10, 10}}, {}, {0, 0, 0});
    Run(g, Input{}, 60);
    AETHER_CHECK(g.GetState() == State::Won);
    g.Restart();
    AETHER_CHECK(g.GetState() == State::Playing);
    AETHER_CHECK(g.Time() == 0.0f);
}

// Every hop of the shipped route is individually makeable: standing at the
// near edge of platform i, running toward platform i+1 and jumping lands on it.
// Proves the level is completable without scripting the whole run.
AETHER_TEST(GemHop_EveryHopInTheShippedLevelIsMakeable) {
    auto plats = Game::DefaultPlatforms();
    AETHER_CHECK(plats.size() >= 2);
    for (usize i = 0; i + 1 < plats.size(); ++i) {
        const Platform& a = plats[i];
        const Platform& b = plats[i + 1];
        // Take-off point: on `a`, as close to `b`'s centre as the platform allows (inset by the body).
        f32 sx = std::clamp(b.cx, a.MinX() + 0.31f, a.MaxX() - 0.31f);
        f32 sz = std::clamp(b.cz, a.MinZ() + 0.31f, a.MaxZ() - 0.31f);
        // Run toward the target for a couple of steps first so we leave the edge moving, then jump.
        Game g(plats, {{0, 100, 0}}, {sx, a.top, sz});
        Input in;
        Vec3 dir{b.cx - sx, 0, b.cz - sz};
        f32 len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
        in.move_x = dir.x / len;
        in.move_z = dir.z / len;
        Run(g, Input{}, 5);
        // Jump when within 0.2 m of the edge (or immediately if already there).
        in.jump = true;
        bool landed = false;
        for (int s = 0; s < 120 && !landed; ++s) {
            g.Step(in);
            if (s > 20) {
                in.jump = false;
            }
            landed = g.PlayerBody().grounded && std::fabs(g.PlayerPos().y - b.top) < 0.01f;
        }
        if (!landed) {
            std::printf("  hop %zu -> %zu failed (ended at %.2f %.2f %.2f)\n", i, i + 1, g.PlayerPos().x,
                        g.PlayerPos().y, g.PlayerPos().z);
        }
        AETHER_CHECK(landed);
    }
}
