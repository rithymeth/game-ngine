// Blueprint VM benchmarks (Phase 38 step 5): a counting loop and many ticking instances, built from synthetic
// graphs so nothing is loaded from disk.
#include "aether/bench/bench.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/graph.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace aether;
using namespace aether::bp;

namespace {

Variable IntVar(const std::string& name) {
    Variable v;
    v.name = name;
    v.type = *ParseType("int");
    v.default_value = DefaultValue(v.type);
    return v;
}

// Event.BeginPlay -> For Loop 0..last: acc = acc + index. Event.Tick: acc = acc + 1.
std::shared_ptr<const CompiledBlueprint> Compile(int loop_last) {
    Blueprint bp;
    Graph g;
    g.name = "EventGraph";
    bp.graphs.push_back(g);
    bp.variables.push_back(IntVar("acc"));
    GraphBuilder b(*bp.FindGraph("EventGraph"));

    const NodeId begin = b.Add("Event.BeginPlay"), loop = b.Add("Flow.ForLoop");
    b.Default(loop, "first", 0).Default(loop, "last", loop_last);
    b.Connect(begin, "then", loop, "exec");
    const NodeId get = b.Add("Var.Get:acc"), add = b.Add("Math.Add:int"), set = b.Add("Var.Set:acc");
    b.Connect(loop, "loop_body", set, "exec").Connect(get, "value", add, "a").Connect(loop, "index", add, "b").Connect(add, "result", set, "value");

    const NodeId tick = b.Add("Event.Tick");
    const NodeId get2 = b.Add("Var.Get:acc"), inc = b.Add("Math.Add:int"), set2 = b.Add("Var.Set:acc");
    b.Default(inc, "b", 1);
    b.Connect(tick, "then", set2, "exec").Connect(get2, "value", inc, "a").Connect(inc, "result", set2, "value");

    CompileResult result = CompileBlueprint(bp);
    return result.Ok() ? result.blueprint : nullptr;
}

struct LoopState {
    World world;
    BlueprintVM vm{world};
    Entity entity;
};
LoopState& Loop() {
    static LoopState s;
    return s;
}

struct TickState {
    World world;
    BlueprintVM vm{world};
};
TickState& Ticks() {
    static TickState s;
    return s;
}

} // namespace

// 10,000 iterations of a loop that adds the index to a variable (about 50,000 VM instructions).
AETHER_BENCH_SETUP(
    "bp/loop_arith_10k", 10,
    [] {
        LoopState& s = Loop();
        s.entity = s.world.CreateEntity();
        s.vm.Attach(s.entity, Compile(9999));
        // Self-check, so the benchmark can't quietly measure an empty loop: sum of 0..9999 is 49,995,000.
        s.vm.Dispatch(s.entity, "Event.BeginPlay");
        const VmValue acc = s.vm.GetVariable(s.entity, "acc");
        if (!s.vm.Errors().empty() || !std::holds_alternative<i32>(acc) || std::get<i32>(acc) != 49'995'000) {
            std::fprintf(stderr, "bp/loop_arith_10k: the graph did not run as expected\n");
            std::abort();
        }
    },
    [] { Loop().vm.Dispatch(Loop().entity, "Event.BeginPlay"); });

// One tick of 1,000 Blueprint instances, each running a three-node graph.
AETHER_BENCH_SETUP(
    "bp/tick_1k_instances", 20,
    [] {
        TickState& s = Ticks();
        const auto compiled = Compile(0);
        for (int i = 0; i < 1000; ++i) s.vm.Attach(s.world.CreateEntity(), compiled);
    },
    [] { Ticks().vm.Tick(1.0f / 60.0f); });
