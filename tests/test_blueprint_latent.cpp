#include "aether/blueprint/system.h"
#include "aether/ecs/world.h"
#include "aether/scene/entity_guid.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

struct Harness {
    World world;
    Blueprint bp;
    std::vector<std::string> printed;
    BlueprintVM vm{world};
    Entity entity;

    Harness() {
        Graph g;
        g.name = "EventGraph";
        bp.graphs.push_back(g);
        vm.SetPrintHandler([this](Entity, const std::string& text) { printed.push_back(text); });
    }
    GraphBuilder Events() { return GraphBuilder(*bp.FindGraph("EventGraph")); }
    NodeId Custom(GraphBuilder& b, const std::string& name, json params = json::array()) {
        return b.Add("Event.Custom", {{"name", name}, {"params", params}});
    }
    NodeId Print(GraphBuilder& b, NodeId after, const std::string& pin, const std::string& text) {
        const NodeId p = b.Add("Debug.Print");
        b.Default(p, "text", text);
        b.Connect(after, pin, p, "exec");
        return p;
    }
    bool Start() {
        CompileResult result = CompileBlueprint(bp);
        for (const Diagnostic& d : result.diagnostics.diagnostics) {
            if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
        }
        if (!result.Ok()) return false;
        entity = world.CreateEntity();
        return vm.Attach(entity, result.blueprint);
    }
    void Fire(const std::string& event, std::vector<VmValue> args = {}) {
        AETHER_CHECK(vm.Dispatch(entity, "Event.Custom:" + event, args));
    }
    std::vector<std::string> Take() {
        std::vector<std::string> out;
        out.swap(printed);
        return out;
    }
};

using Lines = std::vector<std::string>;

} // namespace

AETHER_TEST(BlueprintLatent_DelayResumesLaterWithItsFrame) {
    Harness h;
    GraphBuilder b = h.Events();
    // BeginPlay -> Sequence: [Delay(1) -> "later"], ["now"].
    const NodeId begin = b.Add("Event.BeginPlay"), seq = b.Add("Flow.Sequence");
    const NodeId delay = b.Add("Latent.Delay");
    b.Default(delay, "duration", 1.0);
    b.Connect(begin, "then", seq, "exec").Connect(seq, "then 0", delay, "exec");
    h.Print(b, delay, "completed", "later");
    h.Print(b, seq, "then 1", "now");
    // Hit(amount) -> Delay(0.5) -> Print(amount): the parameter survives the wait.
    const NodeId hit = h.Custom(b, "Hit", {{{"name", "amount"}, {"type", "int"}}});
    const NodeId wait = b.Add("Latent.Delay"), p = b.Add("Debug.Print");
    b.Default(wait, "duration", 0.5);
    b.Connect(hit, "then", wait, "exec").Connect(wait, "completed", p, "exec").Connect(hit, "amount", p, "text");
    AETHER_CHECK(h.Start());

    AETHER_CHECK(h.vm.Dispatch(h.entity, "Event.BeginPlay"));
    AETHER_CHECK(h.Take() == Lines{"now"}); // the Sequence went on without waiting
    AETHER_CHECK(h.vm.PendingLatentActions() == 1);
    AETHER_CHECK(h.vm.Dispatch(h.entity, "Event.BeginPlay")); // again while waiting: that Delay ignores it
    AETHER_CHECK(h.Take() == Lines{"now"} && h.vm.PendingLatentActions() == 1);
    h.vm.Tick(0.5f);
    AETHER_CHECK(h.Take().empty());
    h.vm.Tick(0.6f);
    AETHER_CHECK(h.Take() == Lines{"later"} && h.vm.PendingLatentActions() == 0);
    AETHER_CHECK(h.vm.GameTime() > 1.0);

    h.Fire("Hit", {i32{7}});
    h.Fire("Hit", {i32{9}}); // ignored: still waiting
    h.vm.Tick(0.5f);
    AETHER_CHECK(h.Take() == Lines{"7"});
    h.Fire("Hit", {i32{9}});
    h.vm.Tick(0.5f);
    AETHER_CHECK(h.Take() == Lines{"9"});
}

AETHER_TEST(BlueprintLatent_RetriggerNextTickAndLoops) {
    Harness h;
    h.bp.variables = {};
    Variable count;
    count.name = "count";
    count.type = PinType::Of(ValueType::Int);
    count.default_value = i32{0};
    h.bp.variables.push_back(count);
    GraphBuilder b = h.Events();
    // Poke(n) -> Retriggerable Delay(1) -> Print(n).
    const NodeId poke = h.Custom(b, "Poke", {{{"name", "n"}, {"type", "int"}}});
    const NodeId retrigger = b.Add("Latent.RetriggerableDelay"), p = b.Add("Debug.Print");
    b.Default(retrigger, "duration", 1.0);
    b.Connect(poke, "then", retrigger, "exec").Connect(retrigger, "completed", p, "exec").Connect(poke, "n", p, "text");
    // Soon -> Delay Until Next Tick -> "next"; Tick prints "tick" (after latent resumes).
    const NodeId soon = h.Custom(b, "Soon"), next = b.Add("Latent.DelayNextTick");
    b.Connect(soon, "then", next, "exec");
    h.Print(b, next, "completed", "next");
    const NodeId tick = b.Add("Event.Tick");
    h.Print(b, tick, "then", "tick");
    // Loop: Start -> Delay(0.25) -> count += 1 -> back into the Delay.
    const NodeId start = h.Custom(b, "Loop"), every = b.Add("Latent.Delay");
    b.Default(every, "duration", 0.25);
    const NodeId get = b.Add("Var.Get:count"), inc = b.Add("Math.Add:int"), set = b.Add("Var.Set:count");
    b.Default(inc, "b", 1);
    b.Connect(start, "then", every, "exec").Connect(every, "completed", set, "exec");
    b.Connect(get, "value", inc, "a").Connect(inc, "result", set, "value").Connect(set, "then", every, "exec");
    AETHER_CHECK(h.Start());

    h.Fire("Poke", {i32{1}});
    h.vm.Tick(0.6f);
    h.Fire("Poke", {i32{2}}); // restarts the timer, with the newer value
    h.vm.Tick(0.6f);
    AETHER_CHECK(h.Take() == (Lines{"tick", "tick"}));
    h.vm.Tick(0.5f);
    AETHER_CHECK(h.Take() == (Lines{"2", "tick"}));

    h.Fire("Soon");
    AETHER_CHECK(h.Take().empty());
    h.vm.Tick(0.0f);
    AETHER_CHECK(h.Take() == (Lines{"next", "tick"})); // latent actions resume before Tick

    h.Fire("Loop");
    for (int i = 0; i < 4; ++i) h.vm.Tick(0.25f);
    AETHER_CHECK(std::get<i32>(h.vm.GetVariable(h.entity, "count")) == 4);
    AETHER_CHECK(h.vm.PendingLatentActions() == 1); // still looping
    AETHER_CHECK(h.vm.Errors().empty());
}

AETHER_TEST(BlueprintLatent_DoOnceDoNGateFlipFlop) {
    Harness h;
    GraphBuilder b = h.Events();
    // Do Once: Go -> "once"; Reset -> reset.
    const NodeId go = h.Custom(b, "Go"), once = b.Add("Flow.DoOnce"), reset = h.Custom(b, "Reset");
    b.Connect(go, "then", once, "exec").Connect(reset, "then", once, "reset");
    h.Print(b, once, "completed", "once");
    // Do N (n = 2): Count -> Print(counter); Recount -> reset.
    const NodeId count = h.Custom(b, "Count"), don = b.Add("Flow.DoN"), recount = h.Custom(b, "Recount");
    const NodeId pc = b.Add("Debug.Print");
    b.Default(don, "n", 2);
    b.Connect(count, "then", don, "enter").Connect(recount, "then", don, "reset");
    b.Connect(don, "exit", pc, "exec").Connect(don, "counter", pc, "text");
    // Gate (starts closed): Enter / Open / Close / Toggle.
    const NodeId gate = b.Add("Flow.Gate");
    b.Default(gate, "start_closed", true);
    for (const char* pin : {"enter", "open", "close", "toggle"}) {
        const NodeId e = h.Custom(b, std::string("Gate_") + pin);
        b.Connect(e, "then", gate, pin);
    }
    h.Print(b, gate, "exit", "through");
    // Flip Flop: Flip -> A / B, printing is_a on A.
    const NodeId flip = h.Custom(b, "Flip"), ff = b.Add("Flow.FlipFlop");
    b.Connect(flip, "then", ff, "exec");
    const NodeId pa = b.Add("Debug.Print");
    b.Connect(ff, "A", pa, "exec").Connect(ff, "is_a", pa, "text");
    h.Print(b, ff, "B", "B");
    AETHER_CHECK(h.Start());

    h.Fire("Go");
    h.Fire("Go");
    AETHER_CHECK(h.Take() == Lines{"once"});
    h.Fire("Reset");
    h.Fire("Go");
    AETHER_CHECK(h.Take() == Lines{"once"});

    for (int i = 0; i < 3; ++i) h.Fire("Count");
    AETHER_CHECK(h.Take() == (Lines{"1", "2"}));
    h.Fire("Recount");
    h.Fire("Count");
    AETHER_CHECK(h.Take() == Lines{"1"});

    h.Fire("Gate_enter");
    AETHER_CHECK(h.Take().empty()); // starts closed
    h.Fire("Gate_open");
    h.Fire("Gate_enter");
    h.Fire("Gate_enter");
    h.Fire("Gate_close");
    h.Fire("Gate_enter");
    h.Fire("Gate_toggle");
    h.Fire("Gate_enter");
    AETHER_CHECK(h.Take() == (Lines{"through", "through", "through"}));

    for (int i = 0; i < 3; ++i) h.Fire("Flip");
    AETHER_CHECK(h.Take() == (Lines{"true", "B", "true"}));

    // State is per instance.
    const Entity other = h.world.CreateEntity();
    AETHER_CHECK(h.vm.Attach(other, CompileBlueprint(h.bp).blueprint));
    AETHER_CHECK(h.vm.Dispatch(other, "Event.Custom:Go"));
    AETHER_CHECK(h.Take() == Lines{"once"});

    // Do Once can start closed.
    Harness closed;
    GraphBuilder c = closed.Events();
    const NodeId go2 = closed.Custom(c, "Go"), once2 = c.Add("Flow.DoOnce"), open2 = closed.Custom(c, "Open");
    c.Default(once2, "start_closed", true);
    c.Connect(go2, "then", once2, "exec").Connect(open2, "then", once2, "reset");
    closed.Print(c, once2, "completed", "once");
    AETHER_CHECK(closed.Start());
    closed.Fire("Go");
    AETHER_CHECK(closed.Take().empty());
    closed.Fire("Open");
    closed.Fire("Go");
    AETHER_CHECK(closed.Take() == Lines{"once"});
}

AETHER_TEST(BlueprintLatent_OwnersPausingAndFunctionRule) {
    Harness h;
    GraphBuilder b = h.Events();
    const NodeId begin = b.Add("Event.BeginPlay"), delay = b.Add("Latent.Delay");
    b.Default(delay, "duration", 1.0);
    b.Connect(begin, "then", delay, "exec");
    h.Print(b, delay, "completed", "done");
    AETHER_CHECK(h.Start());

    // Disabled: waits (paused) until enabled again.
    h.vm.Dispatch(h.entity, "Event.BeginPlay");
    h.vm.SetEnabled(h.entity, false);
    h.vm.Tick(2.0f);
    AETHER_CHECK(h.Take().empty() && h.vm.PendingLatentActions() == 1);
    h.vm.SetEnabled(h.entity, true);
    h.vm.Tick(0.0f);
    AETHER_CHECK(h.Take() == Lines{"done"});

    // Detached or destroyed owners drop their actions.
    h.vm.Dispatch(h.entity, "Event.BeginPlay");
    h.vm.Detach(h.entity);
    AETHER_CHECK(h.vm.PendingLatentActions() == 0);
    AETHER_CHECK(h.vm.Attach(h.entity, CompileBlueprint(h.bp).blueprint));
    h.vm.Dispatch(h.entity, "Event.BeginPlay");
    h.world.DestroyEntity(h.entity);
    h.vm.Tick(2.0f);
    AETHER_CHECK(h.Take().empty() && h.vm.PendingLatentActions() == 0);

    // BP002: latent nodes can't be used in functions.
    Blueprint bp = h.bp;
    Graph fn;
    fn.name = "Wait";
    fn.kind = GraphKind::Function;
    GraphBuilder f(fn);
    const NodeId entry = f.Add("Function.Entry"), wait = f.Add("Latent.Delay");
    f.Connect(entry, "then", wait, "exec");
    bp.graphs.push_back(fn);
    const CompileResult result = CompileBlueprint(bp);
    AETHER_CHECK(!result.Ok() && result.diagnostics.Has("BP002"));
    const std::vector<const Diagnostic*> on_node = result.diagnostics.For("Wait", wait);
    AETHER_CHECK(on_node.size() == 1 && on_node[0]->message.find("can't be used in a function") != std::string::npos);
}

AETHER_TEST(BlueprintSystem_RunsInstancesThroughTheLifecycle) {
    World world;
    GuidIndex guids;
    Lifecycle lifecycle(world, guids);
    BlueprintSystem system(world);
    std::vector<std::string> printed;
    system.VM().SetPrintHandler([&](Entity, const std::string& text) { printed.push_back(text); });

    // BP_Greeter: BeginPlay prints the (instance-editable) greeting; Tick
    // counts; EndPlay prints "bye".
    Blueprint greeter;
    Variable greeting;
    greeting.name = "greeting";
    greeting.type = PinType::Of(ValueType::String);
    greeting.default_value = std::string("hello");
    greeting.flags = Var_InstanceEditable;
    Variable ticks;
    ticks.name = "ticks";
    ticks.type = PinType::Of(ValueType::Int);
    ticks.default_value = i32{0};
    greeter.variables = {greeting, ticks};
    Graph g;
    g.name = "EventGraph";
    GraphBuilder b(g);
    const NodeId begin = b.Add("Event.BeginPlay"), get = b.Add("Var.Get:greeting"), p = b.Add("Debug.Print");
    b.Connect(begin, "then", p, "exec").Connect(get, "value", p, "text");
    const NodeId tick = b.Add("Event.Tick"), getn = b.Add("Var.Get:ticks"), inc = b.Add("Math.Add:int");
    const NodeId set = b.Add("Var.Set:ticks");
    b.Default(inc, "b", 1);
    b.Connect(tick, "then", set, "exec").Connect(getn, "value", inc, "a").Connect(inc, "result", set, "value");
    const NodeId end = b.Add("Event.EndPlay"), bye = b.Add("Debug.Print");
    b.Default(bye, "text", "bye");
    b.Connect(end, "then", bye, "exec");
    greeter.graphs.push_back(g);

    Blueprint broken;
    Graph bg;
    bg.name = "EventGraph";
    GraphBuilder(bg).Add("Var.Get:nothing");
    broken.graphs.push_back(bg);

    const assets::AssetGuid greeter_guid = assets::NewAssetGuid(), broken_guid = assets::NewAssetGuid();
    int loads = 0;
    system.SetLoader([&](const assets::AssetGuid& guid, Blueprint& out, std::string& name) {
        ++loads;
        if (guid == greeter_guid) {
            out = greeter;
            name = "BP_Greeter";
            return true;
        }
        if (guid == broken_guid) {
            out = broken;
            name = "BP_Broken";
            return true;
        }
        return false;
    });
    system.Register(lifecycle);

    auto spawn = [&](const assets::AssetGuid& guid, const char* greeting_override) {
        BlueprintInstance instance;
        instance.blueprint.guid = guid;
        if (greeting_override != nullptr) instance.SetOverride("greeting", greeting_override);
        const Entity e = world.CreateEntity(IdComponent{NewEntityGuid()}, instance);
        guids.Add(world.GetComponent<IdComponent>(e)->guid, e);
        return e;
    };
    const Entity a = spawn(greeter_guid, nullptr), c = spawn(greeter_guid, "howdy");
    const Entity bad = spawn(broken_guid, nullptr), bad2 = spawn(broken_guid, nullptr);

    lifecycle.BeginPlay();
    AETHER_CHECK(printed.size() == 2);
    AETHER_CHECK(std::find(printed.begin(), printed.end(), "hello") != printed.end());
    AETHER_CHECK(std::find(printed.begin(), printed.end(), "howdy") != printed.end());
    AETHER_CHECK(system.VM().IsAttached(a) && system.VM().IsAttached(c));
    AETHER_CHECK(!system.VM().IsAttached(bad) && !system.VM().IsAttached(bad2));
    AETHER_CHECK(system.CompileErrors().size() == 1 && system.CompileErrors()[0].name == "BP_Broken");
    AETHER_CHECK(system.CompileErrors()[0].diagnostics[0].code == "BP005");
    AETHER_CHECK(loads == 2); // compiled once per asset

    system.Update(1.0f / 60.0f);
    system.Update(1.0f / 60.0f);
    AETHER_CHECK(std::get<i32>(system.VM().GetVariable(a, "ticks")) == 2);
    lifecycle.SetActive(c, false); // disabled: no Tick
    system.Update(1.0f / 60.0f);
    AETHER_CHECK(std::get<i32>(system.VM().GetVariable(a, "ticks")) == 3);
    AETHER_CHECK(std::get<i32>(system.VM().GetVariable(c, "ticks")) == 2);

    printed.clear();
    lifecycle.Destroy(a);
    AETHER_CHECK(printed == Lines{"bye"} && !system.VM().IsAttached(a));
    printed.clear();
    lifecycle.EndPlay();
    AETHER_CHECK(printed == Lines{"bye"} && system.VM().InstanceCount() == 0);
}
