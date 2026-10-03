#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/core/console.h"
#include "aether/core/profiler.h"
#include "aether/debug/debug_draw.h"
#include "aether/ecs/world.h"
#include "aether/debug/stats.h"
#include "aether/net/net_stats.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <thread>

// Phase 23 step 3: debug drawing (shapes, text, durations, limits,
// threads, the CVar), its reflected functions and Blueprint nodes, and
// the stat overlay groups (built-in, custom, net) and the `stat` command.

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
bool Near(const Vec3& a, const Vec3& b) { return (a - b).Length() < 1e-4f; }

struct DrawReset {
    DrawReset() { Reset(); }
    ~DrawReset() { Reset(); }
    static void Reset() {
        DebugDrawList& d = DebugDrawList::Get();
        d.Clear();
        d.enabled = true;
        d.max_lines = 200'000;
    }
};
} // namespace

AETHER_TEST(DebugDraw_ShapesBecomeLines) {
    DrawReset reset;
    DebugDrawList& d = DebugDrawList::Get();
    CHECK(DebugColor(1, 0, 0) == 0xFF0000FFu && DebugColor(Vec3(0, 1, 0)) == 0xFF00FF00u && DebugColor(0, 0, 1, 0) == 0x00FF0000u);
    d.Line(Vec3(0, 0, 0), Vec3(1, 0, 0), 0xFFFFFFFFu);
    CHECK(d.LineCount() == 1);
    d.Arrow(Vec3(0, 0, 0), Vec3(0, 0, 2), 0xFFFFFFFFu);
    CHECK(d.LineCount() == 1 + 5);
    d.Clear();
    // A box rotated a quarter turn about y: its +x face goes to -z.
    d.Box(Vec3(10, 0, 0), Vec3(2, 1, 1), 0xFFFFFFFFu, 0.0f, Quaternion::FromAxisAngle(Vec3(0, 1, 0), 1.5707963f));
    std::vector<DebugLine> lines = d.Lines();
    CHECK(lines.size() == 12);
    f32 min_z = 1e9f, max_z = -1e9f;
    for (const DebugLine& l : lines) min_z = std::min({min_z, l.a.z, l.b.z}), max_z = std::max({max_z, l.a.z, l.b.z});
    CHECK(std::fabs(min_z + 2.0f) < 1e-4f && std::fabs(max_z - 2.0f) < 1e-4f);
    d.Clear();
    d.Sphere(Vec3(0, 5, 0), 2.0f, 0xFFFFFFFFu);
    lines = d.Lines();
    CHECK(lines.size() == 48);
    CHECK(std::all_of(lines.begin(), lines.end(), [](const DebugLine& l) {
        return std::fabs((l.a - Vec3(0, 5, 0)).Length() - 2.0f) < 1e-4f && std::fabs((l.b - Vec3(0, 5, 0)).Length() - 2.0f) < 1e-4f;
    }));
    d.Clear();
    d.Circle(Vec3(0, 0, 0), Vec3(0, 1, 0), 1.0f, 0xFFFFFFFFu, 0.0f, 8);
    lines = d.Lines();
    CHECK(lines.size() == 8 && std::all_of(lines.begin(), lines.end(), [](const DebugLine& l) { return std::fabs(l.a.y) < 1e-5f; }));
    d.Clear();
    d.Point(Vec3(1, 2, 3), 1.0f, 0xFFFFFFFFu);
    d.Axes(Vec3(0, 0, 0), Quaternion{}, 2.0f);
    lines = d.Lines();
    CHECK(lines.size() == 6 && Near(lines[0].a, Vec3(0.5f, 2, 3)) && Near(lines[5].b, Vec3(0, 0, 2)));
    CHECK(lines[3].color == DebugColor(1, 0.2f, 0.2f));
    CHECK(Near(RotateByQuaternion(Quaternion::FromAxisAngle(Vec3(0, 0, 1), 3.14159265f), Vec3(1, 0, 0)), Vec3(-1, 0, 0)));
}

AETHER_TEST(DebugDraw_DurationsTextLimitsAndThreads) {
    DrawReset reset;
    DebugDrawList& d = DebugDrawList::Get();
    d.Line(Vec3(), Vec3(1, 0, 0), 1);            // one frame
    d.Line(Vec3(), Vec3(0, 1, 0), 2, 0.5f);      // half a second
    d.Text(Vec3(0, 2, 0), "hello", 3, 0.25f);
    d.ScreenText("first", 4);
    d.ScreenText("second", 5, 1.0f);
    CHECK(d.LineCount() == 2 && d.TextCount() == 3);
    std::vector<DebugText> texts = d.Texts();
    CHECK(texts[1].screen && texts[2].position.y > texts[1].position.y);
    d.Tick(0.2f);
    CHECK(d.LineCount() == 1 && d.TextCount() == 2);
    texts = d.Texts();
    // The one-frame screen text went; "second" moves up into its place.
    const auto second = std::find_if(texts.begin(), texts.end(), [](const DebugText& t) { return t.text == "second"; });
    CHECK(second != texts.end() && second->position.y == 8.0f);
    d.Tick(0.2f);
    d.Tick(0.2f);
    CHECK(d.LineCount() == 1 && d.TextCount() == 1); // 0.5 s: shown on three ticks; the text's 0.25 s is up
    d.Tick(0.2f);
    CHECK(d.LineCount() == 0);
    d.Clear();
    // Off: nothing is kept.
    Console console;
    CHECK(console.Execute("debug.draw 0") && !d.enabled);
    d.Sphere(Vec3(), 1, 1);
    d.Text(Vec3(), "x", 1);
    CHECK(d.LineCount() == 0 && d.TextCount() == 0);
    CHECK(console.Execute("debug.draw 1") && d.enabled);
    // The limit.
    d.max_lines = 10;
    const u64 dropped = d.Dropped();
    d.Sphere(Vec3(), 1, 1); // 48
    CHECK(d.LineCount() == 10 && d.Dropped() == dropped + 38);
    d.Clear();
    d.max_lines = 200'000;
    // From several threads at once.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) d.Line(Vec3(), Vec3(static_cast<f32>(i), 0, 0), 1, 1.0f);
        });
    for (std::thread& t : threads) t.join();
    CHECK(d.LineCount() == 4000);
}

AETHER_TEST(DebugDraw_ReflectedAndBlueprintNodes) {
    DrawReset reset;
    const reflect::TypeInfo& type = reflect::Reflect<DebugDraw>();
    const reflect::FunctionInfo* line = type.FindFunction("Line");
    CHECK(line && line->HasFlag(reflect::Fn_BlueprintCallable) && line->HasFlag(reflect::Fn_Static) && line->params.size() == 4);
    std::vector<reflect::Any> args = {Vec3(0, 0, 0), Vec3(1, 1, 1), Vec3(1, 0, 0), 2.0f};
    CHECK(line->Invoke(nullptr, args));
    std::vector<DebugLine> lines = DebugDrawList::Get().Lines();
    CHECK(lines.size() == 1 && lines[0].color == DebugColor(1, 0, 0) && lines[0].remaining == 2.0f);
    DebugDrawList::Get().Clear();

    // A Blueprint draws a sphere and some text at BeginPlay.
    bp::Blueprint blueprint;
    bp::Graph g;
    g.name = "EventGraph";
    blueprint.graphs.push_back(g);
    bp::GraphBuilder b(*blueprint.FindGraph("EventGraph"));
    const bp::NodeId begin = b.Add("Event.BeginPlay"), sphere = b.Add("Call.Native:DebugDraw.Sphere");
    const bp::NodeId text = b.Add("Call.Native:DebugDraw.Text");
    b.Default(sphere, "radius", 3.0).Default(sphere, "duration", 1.0).Default(text, "text", std::string("spawn"));
    b.Connect(begin, "then", sphere, "exec").Connect(sphere, "then", text, "exec");
    bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
    CHECK(compiled.Ok());
    World world;
    bp::BlueprintVM vm(world);
    CHECK(vm.Attach(world.CreateEntity(), compiled.blueprint));
    vm.BeginPlay();
    CHECK(DebugDrawList::Get().LineCount() == 48 && DebugDrawList::Get().TextCount() == 1);
    CHECK(DebugDrawList::Get().Texts()[0].text == "spawn" && DebugDrawList::Get().Lines()[0].remaining == 1.0f);
    CHECK(vm.Errors().empty());
}

AETHER_TEST(Stats_GroupsCommandAndNet) {
    StatGroups& g = StatGroups::Get();
    g.HideAll();
    for (const char* name : {"fps", "zones", "counters", "gpu", "memory"}) CHECK(g.Has(name));
    int calls = 0;
    g.Register("test.custom", "A test group", [&] {
        ++calls;
        return std::vector<StatLine>{{"custom line", kStatGood}};
    });
    CHECK(g.Toggle("test.custom") && g.Shown("test.custom") && !g.Toggle("nope"));
    CHECK(g.Collect().size() == 1 && g.Collect()[0].second[0].text == "custom line" && calls == 2);
    // With some profiled frames, fps has numbers.
    Profiler& p = Profiler::Get();
    p.SetEnabled(true), p.SetPaused(false);
    for (int i = 0; i < 3; ++i) {
        p.BeginFrame();
        p.Count("Test.Stat.Draws", 5);
        p.EndFrame();
    }
    Console console;
    CHECK(console.Execute("stat fps counters"));
    const auto groups = g.Collect();
    CHECK(g.ShownNames() == (std::vector<std::string>{"test.custom", "fps", "counters"}) && groups.size() == 3);
    CHECK(groups[1].second[0].text.find("fps") != std::string::npos);
    CHECK(std::any_of(groups[2].second.begin(), groups[2].second.end(),
                      [](const StatLine& l) { return l.text.find("Test.Stat.Draws") != std::string::npos; }));
    CHECK(console.Execute("stat") && std::any_of(console.Output().begin(), console.Output().end(),
                                                  [](const ConsoleLine& l) { return l.text.rfind("* fps", 0) == 0; }));
    console.Execute("stat bogus");
    CHECK(console.Output().back().text.find("no stat group named 'bogus'") != std::string::npos);
    CHECK(console.Execute("stat none") && g.ShownNames().empty());
    g.Unregister("test.custom");
    CHECK(!g.Has("test.custom"));

    // `stat net` for a host, while the group exists.
    net::LoopbackNetwork network;
    auto server_socket = network.Open(7777), client_socket = network.Open();
    net::NetHost server(*server_socket), client(*client_socket);
    server.Listen();
    client.Connect(net::Address::Loopback(7777), network.Now());
    for (int i = 0; i < 30; ++i) {
        network.Advance(1.0 / 60.0);
        server.Update(network.Now()), client.Update(network.Now());
        server.TakeEvents(), client.TakeEvents();
    }
    {
        net::NetStatGroup stat(server);
        CHECK(g.Has("net") && g.Toggle("net"));
        const auto lines = g.Collect()[0].second;
        CHECK(lines.size() == 2 && lines[0].text == "1 peer" && lines[1].text.find("127.0.0.1:") != std::string::npos);
        CHECK(lines[1].text.find(" ms") != std::string::npos && lines[1].text.find("KB") != std::string::npos);
    }
    CHECK(!g.Has("net") && g.ShownNames().empty());
    p.Clear();
}
