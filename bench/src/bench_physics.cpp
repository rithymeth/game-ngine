// Physics benchmarks (Phase 38 step 5): stepping a pile of bodies that actually collide. That is the
// shape of work a game asks for -- many bodies, many contacts -- rather than one body falling.
//
// `CreateBox` makes a *static* body (level geometry), so it serves as the floor here and cannot be
// the falling bodies: a world of static boxes steps in microseconds because nothing moves.
#include "aether/bench/bench.h"
#include "aether/job/job_system.h"
#include "aether/physics/physics_world.h"

#include <cmath>
#include <memory>

using namespace aether;

namespace {

constexpr int kBodyCount = 500;
constexpr u32 kStepsPerIteration = 8; // run past the initial fall so the samples are contact work

struct Pile {
    JobSystem jobs;
    std::unique_ptr<PhysicsWorld> world;

    void Build() {
        world = std::make_unique<PhysicsWorld>(jobs);
        // A large static floor, then dynamic spheres in a grid above it.
        world->CreateBox(Vec3(0.0f, -0.5f, 0.0f), Vec3(50.0f, 0.5f, 50.0f));
        const int side = static_cast<int>(std::ceil(std::cbrt(static_cast<double>(kBodyCount))));
        int made = 0;
        for (int y = 0; y < side && made < kBodyCount; ++y) {
            for (int z = 0; z < side && made < kBodyCount; ++z) {
                for (int x = 0; x < side && made < kBodyCount; ++x) {
                    // Spaced so neighbours start apart; they fall, settle and keep resting contacts.
                    world->CreateSphere(
                        Vec3(static_cast<f32>(x) * 1.3f, static_cast<f32>(y) * 1.3f + 3.0f, static_cast<f32>(z) * 1.3f),
                        0.5f, 1.0f, false);
                    ++made;
                }
            }
        }
        // Let them land, so the measured steps are resting-contact steps rather than free fall.
        for (int i = 0; i < 300; ++i) world->Step(1.0f / 60.0f);
    }
};

Pile& ThePile() {
    static Pile pile;
    return pile;
}

} // namespace

AETHER_BENCH_SETUP("physics/step_500_bodies", 10, [] { ThePile().Build(); },
                   [] {
                       for (u32 i = 0; i < kStepsPerIteration; ++i) ThePile().world->Step(1.0f / 60.0f);
                   });
