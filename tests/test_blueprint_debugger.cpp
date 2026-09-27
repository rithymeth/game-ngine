#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

Variable Var(const std::string& name, const char* type) {
    Variable v;
    v.name = name;
    v.type = *ParseType(type);
    v.default_value = DefaultValue(v.type);
    return v;
}

// BeginPlay -> [A] Set x = 40 + 2 -> [B] Call Twice(x) -> [C] Print(result).
// Twice: Entry -> [F] Set y = n * 2 -> Return(y).
struct DebugFixture {
    Blueprint bp;
    NodeId a = 0, b = 0, c = 0, add = 0, f = 0, loop = 0, body = 0;
    World world;
    std::shared_ptr<const CompiledBlueprint> compiled;
    std::vector<std::string> printed;
    BlueprintVM vm{world};

    DebugFixture() {
        bp.variables = {Var("x", "int"), Var("y", "int")};
        Graph twice;
        twice.name = "Twice";
        twice.kind = GraphKind::Function;
        twice.inputs = {Var("n", "int")};
        twice.outputs = {Var("result", "int")};
        {
            GraphBuilder g(twice);
            const NodeId entry = g.Add("Function.Entry"), mul = g.Add("Math.Multiply:int"), ret = g.Add("Function.Return");
            f = g.Add("Var.Set:y");
            g.Default(mul, "b", 2);
            g.Connect(entry, "then", f, "exec").Connect(entry, "n", mul, "a").Connect(mul, "result", f, "value");
            g.Connect(f, "then", ret, "exec").Connect(f, "value", ret, "result");
        }
        Graph events;
        events.name = "EventGraph";
        {
            GraphBuilder g(events);
            const NodeId begin = g.Add("Event.BeginPlay");
            add = g.Add("Math.Add:int");
            g.Default(add, "a", 40).Default(add, "b", 2);
            a = g.Add("Var.Set:x");
            b = g.Add("Call.Self:Twice");
            c = g.Add("Debug.Print");
            g.Connect(begin, "then", a, "exec").Connect(add, "result", a, "value");
            g.Connect(a, "then", b, "exec").Connect(a, "value", b, "n");
            g.Connect(b, "then", c, "exec").Connect(b, "result", c, "text");
            // Custom event Count: For Loop 1..3 printing the index.
            const NodeId count = g.Add("Event.Custom", {{"name", "Count"}});
            loop = g.Add("Flow.ForLoop");
            g.Default(loop, "first", 1).Default(loop, "last", 3);
            body = g.Add("Debug.Print");
            g.Connect(count, "then", loop, "exec").Connect(loop, "loop_body", body, "exec").Connect(loop, "index", body, "text");
        }
        bp.graphs = {events, twice};
        CompileResult r = CompileBlueprint(bp);
        AETHER_CHECK(r.Ok());
        compiled = r.blueprint;
        vm.SetPrintHandler([this](Entity, const std::string& t) { printed.push_back(t); });
    }
    Entity Spawn() {
        const Entity e = world.CreateEntity();
        vm.Attach(e, compiled);
        return e;
    }
};

const BpPinValue* ValueOf(const BpFrame& frame, NodeId node, const std::string& pin) {
    for (const BpPinValue& v : frame.values) {
        if (v.node == node && v.pin == pin) return &v;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(BlueprintDebugger_BreakpointsShowValuesAndFrames) {
    DebugFixture d;
    const Entity e = d.Spawn();
    std::vector<BpStop> stops;
    d.vm.SetDebugHandler([&](const BpStop& stop) {
        stops.push_back(stop);
        return BpAction::Continue;
    });
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "EventGraph", d.c));
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "Twice", d.f));
    AETHER_CHECK(!d.vm.SetBreakpoint(*d.compiled, "EventGraph", 999)); // no such node
    AETHER_CHECK(!d.vm.SetBreakpoint(*d.compiled, "Nope", d.c));

    AETHER_CHECK(d.vm.Dispatch(e, "Event.BeginPlay"));
    AETHER_CHECK(d.printed == std::vector<std::string>{"84"});
    AETHER_CHECK(stops.size() == 2);
    // Inside Twice first: two frames, the function innermost.
    const BpStop& inner = stops[0];
    AETHER_CHECK(inner.reason == BpStopReason::Breakpoint && inner.graph == "Twice" && inner.node == d.f);
    AETHER_CHECK(inner.frames.size() == 2 && inner.frames[0].function == "Twice");
    AETHER_CHECK(inner.frames[1].function == "Event.BeginPlay" && inner.frames[1].node == d.b && inner.entity == e);
    const BpPinValue* sum = ValueOf(inner.frames[1], d.add, "result");
    AETHER_CHECK(sum != nullptr && sum->value == "42" && sum->type == "int");
    // Then at Print, back in the event, with the call's result.
    const BpStop& outer = stops[1];
    AETHER_CHECK(outer.node == d.c && outer.frames.size() == 1);
    const BpPinValue* result = ValueOf(outer.frames[0], d.b, "result");
    AETHER_CHECK(result != nullptr && result->value == "84");

    // Cleared: no stops; detached: none either.
    stops.clear();
    d.vm.ClearBreakpoints();
    AETHER_CHECK(d.vm.Dispatch(e, "Event.BeginPlay") && stops.empty());
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "EventGraph", d.c));
    d.vm.SetDebugHandler(nullptr);
    AETHER_CHECK(d.vm.Dispatch(e, "Event.BeginPlay") && stops.empty());

    // A breakpoint in a loop body stops every iteration.
    d.vm.SetDebugHandler([&](const BpStop& stop) {
        stops.push_back(stop);
        return BpAction::Continue;
    });
    d.vm.ClearBreakpoints();
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "EventGraph", d.body));
    AETHER_CHECK(d.vm.Dispatch(e, "Event.Custom:Count"));
    AETHER_CHECK(stops.size() == 3);
    const BpPinValue* index = ValueOf(stops[1].frames[0], d.loop, "index");
    AETHER_CHECK(index != nullptr && index->value == "2");
}

AETHER_TEST(BlueprintDebugger_StepOverIntoOutAndPause) {
    DebugFixture d;
    const Entity e = d.Spawn();
    std::vector<std::pair<std::string, NodeId>> path;
    std::vector<BpAction> script;
    usize step = 0;
    d.vm.SetDebugHandler([&](const BpStop& stop) {
        path.emplace_back(stop.graph, stop.node);
        return step < script.size() ? script[step++] : BpAction::Continue;
    });
    auto run = [&](std::vector<BpAction> actions) {
        path.clear();
        script = std::move(actions);
        step = 0;
        AETHER_CHECK(d.vm.Dispatch(e, "Event.BeginPlay"));
    };

    // Step Over from A: B, then C; the function isn't entered.
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "EventGraph", d.a));
    run({BpAction::StepOver, BpAction::StepOver, BpAction::Continue});
    AETHER_CHECK(path.size() == 3 && path[0].second == d.a && path[1].second == d.b && path[2].second == d.c);
    AETHER_CHECK(path[1].first == "EventGraph" && path[2].first == "EventGraph");

    // Step Into from B goes into Twice; Step Out comes back to C.
    d.vm.ClearBreakpoints();
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "EventGraph", d.b));
    run({BpAction::StepInto, BpAction::StepOut, BpAction::Continue});
    AETHER_CHECK(path.size() == 3 && path[0].second == d.b);
    AETHER_CHECK(path[1].first == "Twice");
    AETHER_CHECK(path[2].first == "EventGraph" && path[2].second == d.c);

    // Pause stops at the first node the next dispatch runs.
    d.vm.ClearBreakpoints();
    d.vm.RequestPause();
    run({BpAction::Continue});
    AETHER_CHECK(path.size() == 1 && path[0].first == "EventGraph");
    AETHER_CHECK(d.vm.DebugStops() == 7);
}

AETHER_TEST(BlueprintDebugger_InstanceFilterAndTrace) {
    DebugFixture d;
    const Entity first = d.Spawn(), second = d.Spawn();
    std::vector<Entity> stopped;
    d.vm.SetDebugHandler([&](const BpStop& stop) {
        stopped.push_back(stop.entity);
        return BpAction::Continue;
    });
    AETHER_CHECK(d.vm.SetBreakpoint(*d.compiled, "EventGraph", d.c));
    d.vm.SetDebugFilter(second); // "Debug: BP_2"
    AETHER_CHECK(d.vm.Dispatch(first, "Event.BeginPlay"));
    AETHER_CHECK(d.vm.Dispatch(second, "Event.BeginPlay"));
    AETHER_CHECK(stopped == std::vector<Entity>{second});

    // The trace lists nodes as they start, for wire animation, including
    // the called function's; and it works without a handler.
    d.vm.SetDebugHandler(nullptr);
    d.vm.SetTraceEnabled(true);
    d.vm.Tick(0.0f);
    AETHER_CHECK(d.vm.Dispatch(first, "Event.BeginPlay"));
    const std::vector<BpTraceEvent> trace = d.vm.TakeTrace();
    auto position = [&](const std::string& graph, NodeId node) {
        return std::find_if(trace.begin(), trace.end(),
                            [&](const BpTraceEvent& t) { return t.graph == graph && t.node == node; }) - trace.begin();
    };
    const auto pa = position("EventGraph", d.a), pb = position("EventGraph", d.b), pf = position("Twice", d.f),
               pc = position("EventGraph", d.c);
    AETHER_CHECK(pa < pb && pb < pf && pf < pc && pc < static_cast<std::ptrdiff_t>(trace.size()));
    AETHER_CHECK(trace.front().entity == first && trace.front().frame == 1);
    AETHER_CHECK(d.vm.TakeTrace().empty()); // taken

    d.vm.SetTraceEnabled(true, 2); // capped: the latest ones are kept
    AETHER_CHECK(d.vm.Dispatch(first, "Event.BeginPlay"));
    const std::vector<BpTraceEvent> capped = d.vm.TakeTrace();
    AETHER_CHECK(capped.size() == 2 && capped.back().node == d.c);
}
