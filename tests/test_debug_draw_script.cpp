#include "aether/debug/debug_draw.h"
#include "aether/script/luau_host.h"
#include "test_framework.h"

// Phase 23 step 3: the Luau `Draw` table.

using namespace aether;
using namespace aether::script;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

AETHER_TEST(DebugDraw_FromLuau) {
    DebugDrawList& d = DebugDrawList::Get();
    d.Clear();
    d.enabled = true;
    LuauHost host;
    ScriptResult r = host.Run(R"(
        local o = vector.create(0, 0, 0)
        Draw.line(o, vector.create(1, 0, 0))
        Draw.line(o, vector.create(0, 1, 0), vector.create(0, 1, 0), 2)
        Draw.box(vector.create(0, 1, 0), vector.create(1, 1, 1), vector.create(1, 1, 0))
        Draw.sphere(o, 2)
        Draw.arrow(o, vector.create(0, 0, 3))
        Draw.point(o)
        Draw.text(vector.create(0, 2, 0), "enemy", vector.create(1, 0, 0), 1.5)
        Draw.screen("score 10")
    )");
    CHECK(r.ok);
    CHECK(d.LineCount() == 1 + 1 + 12 + 48 + 5 + 3 && d.TextCount() == 2);
    const std::vector<DebugLine> lines = d.Lines();
    CHECK(lines[0].color == 0xFFFFFFFFu && lines[1].color == DebugColor(0, 1, 0) && lines[1].remaining == 2.0f);
    const std::vector<DebugText> texts = d.Texts();
    CHECK(texts[0].text == "enemy" && texts[0].remaining == 1.5f && texts[1].screen && texts[1].text == "score 10");
    // Bad arguments are script errors; the table can't be replaced.
    CHECK(!host.Run("Draw.line(1, 2)").ok);
    CHECK(!host.Run("Draw.sphere(vector.create(0,0,0))").ok);
    CHECK(!host.Run("Draw.line = nil").ok);
    d.Clear();
}
