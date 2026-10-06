#include "aether/dev/debug_draw.h"
#include "test_framework.h"

#include <algorithm>
#include <atomic>
#include <thread>

// Phase 23 step 5: the general debug draw accumulator - primitives, their
// fields, duration expiry via advance, thread-safe accumulation, clearing,
// and the free-function forwarding.

using namespace aether;
using namespace aether::dev;

namespace {

usize CountType(const std::vector<DebugPrimitive>& v, DebugPrimitive::Type type) {
    return static_cast<usize>(std::count_if(v.begin(), v.end(),
                                            [&](const DebugPrimitive& p) { return p.type == type; }));
}

} // namespace

AETHER_TEST(DevDebugDraw_AddsPrimitivesWithFields) {
    DebugDraw dd;
    dd.AddLine(Vec3(0, 0, 0), Vec3(1, 2, 3), 0xFF00FF00u, 0.0f);
    dd.AddSphere(Vec3(4, 5, 6), 2.0f, 0xFFFF0000u, 0.5f);
    dd.AddBox(Vec3(0, 0, 0), Vec3(1, 1, 1), 0xFF0000FFu, 1.0f);

    const std::vector<DebugPrimitive> snap = dd.Snapshot();
    AETHER_CHECK(snap.size() == 3);
    AETHER_CHECK(CountType(snap, DebugPrimitive::Type::Line) == 1);
    AETHER_CHECK(CountType(snap, DebugPrimitive::Type::Sphere) == 1);
    AETHER_CHECK(CountType(snap, DebugPrimitive::Type::Box) == 1);
    const DebugPrimitive& line = snap[0];
    AETHER_CHECK(line.type == DebugPrimitive::Type::Line);
    AETHER_CHECK(line.b.x == 1.0f && line.b.y == 2.0f && line.b.z == 3.0f);
    AETHER_CHECK(line.color == 0xFF00FF00u);
    const DebugPrimitive& sphere = snap[1];
    AETHER_CHECK(sphere.radius == 2.0f);
    AETHER_CHECK(sphere.a.x == 4.0f);
    dd.Clear();
}

AETHER_TEST(DevDebugDraw_DurationZeroLivesOneAdvance) {
    DebugDraw dd;
    dd.AddLine(Vec3(0, 0, 0), Vec3(1, 0, 0), 0xFFFFFFFFu, 0.0f); // duration 0 = this frame
    dd.Advance(1.0f / 60.0f);
    AETHER_CHECK(!dd.Snapshot().empty()); // still alive for the render this frame
    dd.Advance(1.0f / 60.0f);
    AETHER_CHECK(dd.Snapshot().empty()); // gone next advance
}

AETHER_TEST(DevDebugDraw_DurationExpiry) {
    DebugDraw dd;
    dd.AddLine(Vec3(0, 0, 0), Vec3(1, 0, 0), 0xFFFFFFFFu, 1.5f);

    dd.Advance(0.5f);
    AETHER_CHECK(!dd.Snapshot().empty()); // 1.0s elapsed < 1.5s
    dd.Advance(0.5f);
    AETHER_CHECK(!dd.Snapshot().empty()); // 1.0s < 1.5s
    dd.Advance(0.5f);
    AETHER_CHECK(!dd.Snapshot().empty()); // 1.5s == duration, not > yet
    dd.Advance(0.01f);
    AETHER_CHECK(dd.Snapshot().empty()); // 1.51s > 1.5s
}

AETHER_TEST(DevDebugDraw_ThreadSafeAccumulation) {
    DebugDraw dd;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                dd.AddLine(Vec3(static_cast<f32>(t), static_cast<f32>(i), 0),
                           Vec3(static_cast<f32>(t) + 1, static_cast<f32>(i), 0));
            }
        });
    }
    for (auto& th : threads) th.join();
    AETHER_CHECK(dd.Snapshot().size() == static_cast<usize>(kThreads * kPerThread));
    dd.Clear();
}

AETHER_TEST(DevDebugDraw_ClearEmpties) {
    DebugDraw dd;
    dd.AddLine(Vec3(0, 0, 0), Vec3(1, 0, 0));
    dd.Clear();
    AETHER_CHECK(dd.Snapshot().empty());
}

AETHER_TEST(DevDebugDraw_InstanceForwards) {
    DebugDraw& inst = DebugDrawInstance();
    const usize before = inst.Snapshot().size();
    inst.AddLine(Vec3(0, 0, 0), Vec3(1, 0, 0));
    AETHER_CHECK(inst.Snapshot().size() == before + 1);
    inst.Clear();
}
