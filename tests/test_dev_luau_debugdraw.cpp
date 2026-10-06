// Phase 23 step 6: engine.debug_draw.* Luau functions forward primitives to
// the host's debug-draw sink (SetDebugDrawHandler).
#include "aether/script/luau_host.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::script;

namespace {
f64 Number(const ScriptResult& r, usize i = 0) {
    return r.values.size() > i && std::holds_alternative<f64>(r.values[i]) ? std::get<f64>(r.values[i]) : -12345.0;
}
} // namespace

AETHER_TEST(LuauDebugDraw_ForwardsPrimitivesToSink) {
    LuauHost host;
    std::vector<LuauHost::DebugDrawCall> calls;
    host.SetDebugDrawHandler([&](const LuauHost::DebugDrawCall& call) { calls.push_back(call); });

    ScriptResult r = host.Run(R"(
        engine.debug_draw.Line(vector.create(0,0,0), vector.create(1,2,3), 0xFFFFFF00, 2.0)
        engine.debug_draw.Box(vector.create(5,5,5), vector.create(1,1,1), 0xFF0000FF, 1.0)
        engine.debug_draw.Sphere(vector.create(0,0,0), 7.5, 0xFFFFFFFF, 0.5)
        engine.debug_draw.Point(vector.create(10,0,0), 3.0, 0xFFFFFF00, 0.25)
        engine.debug_draw.Text(vector.create(0,10,0), "hello", 0xFF00FFFF, 4.0)
    )", "draw");
    AETHER_CHECK(r.ok);

    AETHER_CHECK(calls.size() == 5);
    if (calls.size() < 5) return;
    AETHER_CHECK(calls[0].kind == LuauHost::DebugDrawCall::Kind::Line);
    AETHER_CHECK_NEAR(calls[0].from.x, 0.0f, 1e-4f);
    AETHER_CHECK_NEAR(calls[0].to.z, 3.0f, 1e-4f);
    AETHER_CHECK(calls[0].color == 0xFFFFFF00u);
    AETHER_CHECK_NEAR(calls[0].duration, 2.0f, 1e-4f);
    AETHER_CHECK(calls[1].kind == LuauHost::DebugDrawCall::Kind::Box);
    AETHER_CHECK_NEAR(calls[1].size.y, 1.0f, 1e-4f);
    AETHER_CHECK(calls[1].color == 0xFF0000FFu);
    AETHER_CHECK(calls[2].kind == LuauHost::DebugDrawCall::Kind::Sphere);
    AETHER_CHECK_NEAR(calls[2].to.x, 7.5f, 1e-4f);
    AETHER_CHECK(calls[3].kind == LuauHost::DebugDrawCall::Kind::Point);
    AETHER_CHECK_NEAR(calls[3].from.x, 10.0f, 1e-4f);
    AETHER_CHECK(calls[4].kind == LuauHost::DebugDrawCall::Kind::Text);
    AETHER_CHECK(calls[4].text == "hello");
    AETHER_CHECK(calls[4].color == 0xFF00FFFFu);
}

AETHER_TEST(LuauDebugDraw_NoOpWithoutSink) {
    LuauHost host;
    // No handler set: calls do nothing, no crash.
    ScriptResult r = host.Run(R"(
        engine.debug_draw.Line(vector.create(0,0,0), vector.create(1,0,0))
        engine.debug_draw.Text(vector.create(0,0,0), "hi")
        return 42
    )", "draw_nosink");
    AETHER_CHECK(r.ok && Number(r) == 42.0);
}

AETHER_TEST(LuauDebugDraw_BadArgsError) {
    LuauHost host;
    ScriptResult r = host.Run(R"(
        engine.debug_draw.Line(vector.create(0,0,0), 123)  -- to is a number, not a vector
    )", "draw_bad");
    AETHER_CHECK(!r.ok); // raises
}
