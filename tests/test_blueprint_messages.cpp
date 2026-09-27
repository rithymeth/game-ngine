#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

using Lines = std::vector<std::string>;

Blueprint Empty() {
    Blueprint bp;
    Graph g;
    g.name = "EventGraph";
    bp.graphs.push_back(g);
    return bp;
}

Variable Param(const std::string& name, const char* type) {
    Variable v;
    v.name = name;
    v.type = *ParseType(type);
    v.default_value = DefaultValue(v.type);
    return v;
}

std::shared_ptr<const CompiledBlueprint> Compile(const Blueprint& bp) {
    CompileResult r = CompileBlueprint(bp);
    for (const Diagnostic& d : r.diagnostics.diagnostics) {
        if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
    }
    AETHER_CHECK(r.Ok());
    return r.blueprint;
}

json EntityParam(const char* name) { return json::array({{{"name", name}, {"type", "Entity"}}}); }

} // namespace

AETHER_TEST(BlueprintMessages_DispatchersBindCallAndUnbind) {
    // BP_Door: dispatcher OnOpened(by: Entity); Custom Event Open(by) calls it.
    Blueprint door = Empty();
    door.dispatchers.push_back({"OnOpened", {Param("by", "Entity")}});
    {
        GraphBuilder b(*door.FindGraph("EventGraph"));
        const NodeId open = b.Add("Event.Custom", {{"name", "Open"}, {"params", EntityParam("by")}});
        const NodeId call = b.Add("Dispatch.Call:OnOpened");
        b.Connect(open, "then", call, "exec").Connect(open, "by", call, "by");
    }
    // BP_Light: Watch(door) binds its Custom Event Glow(by) to the door's
    // OnOpened; Ignore(door) unbinds; Forget(door) unbinds everything.
    Blueprint light = Empty();
    {
        GraphBuilder b(*light.FindGraph("EventGraph"));
        const NodeId glow = b.Add("Event.Custom", {{"name", "Glow"}, {"params", EntityParam("by")}});
        const NodeId fmt = b.Add("Text.Format", {{"format", "glow {me} by {who}"}});
        const NodeId self = b.Add("Entity.Self"), p = b.Add("Debug.Print");
        b.Connect(self, "self", fmt, "me").Connect(glow, "by", fmt, "who");
        b.Connect(glow, "then", p, "exec").Connect(fmt, "result", p, "text");
        for (const auto& [event, node] : {std::pair<const char*, const char*>{"Watch", "Dispatch.Bind:OnOpened"},
                                          {"Ignore", "Dispatch.Unbind:OnOpened"},
                                          {"Forget", "Dispatch.UnbindAll:OnOpened"}}) {
            const NodeId e = b.Add("Event.Custom", {{"name", event}, {"params", EntityParam("door")}});
            const NodeId n = b.Add(node, {{"event", "Glow"}});
            b.Connect(e, "then", n, "exec").Connect(e, "door", n, "target");
        }
    }
    World world;
    BlueprintVM vm(world);
    Lines printed;
    vm.SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });
    const Entity d = world.CreateEntity(), l1 = world.CreateEntity(), l2 = world.CreateEntity(), who = world.CreateEntity();
    AETHER_CHECK(vm.Attach(d, Compile(door)));
    const auto light_bp = Compile(light);
    AETHER_CHECK(vm.Attach(l1, light_bp) && vm.Attach(l2, light_bp));
    auto fire = [&](Entity target, const char* event, Entity arg) {
        const VmValue value = arg;
        AETHER_CHECK(vm.Dispatch(target, std::string("Event.Custom:") + event, std::span<const VmValue>(&value, 1)));
    };
    auto name = [](Entity e) { return "Entity(" + std::to_string(e.index) + "v" + std::to_string(e.generation) + ")"; };

    fire(d, "Open", who); // nobody bound: nothing
    AETHER_CHECK(printed.empty());
    fire(l1, "Watch", d);
    fire(l1, "Watch", d); // binding twice is still one binding
    fire(l2, "Watch", d);
    fire(d, "Open", who);
    AETHER_CHECK(printed == (Lines{"glow " + name(l1) + " by " + name(who), "glow " + name(l2) + " by " + name(who)}));
    printed.clear();
    fire(l1, "Ignore", d);
    fire(d, "Open", who);
    AETHER_CHECK(printed.size() == 1 && printed[0].find(name(l2)) != std::string::npos);
    printed.clear();
    vm.Detach(l2); // a listener that goes away is unbound
    fire(d, "Open", who);
    AETHER_CHECK(printed.empty());
    AETHER_CHECK(vm.Attach(l2, light_bp));
    fire(l1, "Watch", d);
    fire(l2, "Watch", d);
    fire(l1, "Forget", d);
    fire(d, "Open", who);
    AETHER_CHECK(printed.empty() && vm.Errors().empty());

    // The door's .abp round-trips its dispatcher.
    Blueprint again;
    AETHER_CHECK(BlueprintFromJson(BlueprintToJson(door), again) && again.dispatchers.size() == 1 &&
                 again.dispatchers[0].params[0].type == PinType::Of(ValueType::Entity));
}

AETHER_TEST(BlueprintMessages_DispatcherErrors) {
    // BP014: a handler whose parameters differ from a declared dispatcher's.
    Blueprint bp = Empty();
    bp.dispatchers.push_back({"OnHit", {Param("damage", "float")}});
    GraphBuilder b(*bp.FindGraph("EventGraph"));
    b.Add("Event.Custom", {{"name", "Wrong"}, {"params", EntityParam("who")}});
    const NodeId bind = b.Add("Dispatch.Bind:OnHit", {{"event", "Wrong"}});
    const CompileResult r = CompileBlueprint(bp);
    AETHER_CHECK(!r.Ok() && r.diagnostics.Has("BP014") && r.diagnostics.For("EventGraph", bind).size() == 1);

    // BP004: a missing handler, and calling a dispatcher this Blueprint doesn't declare.
    Blueprint missing = Empty();
    GraphBuilder m(*missing.FindGraph("EventGraph"));
    m.Add("Dispatch.Bind:OnHit", {{"event", "Nope"}});
    m.Add("Dispatch.Call:OnHit");
    const CompileResult rm = CompileBlueprint(missing);
    AETHER_CHECK(rm.diagnostics.errors == 2 && rm.diagnostics.Has("BP004"));
}

AETHER_TEST(BlueprintMessages_InterfacesCallOnlyImplementers) {
    RegisterBlueprintInterface({"Interactable", {{"Interact", {Param("instigator", "Entity")}}, {"Highlight", {}}}});

    // BP_Lever implements Interactable: Interact prints "pulled".
    Blueprint lever = Empty();
    lever.interfaces = {"Interactable"};
    {
        GraphBuilder b(*lever.FindGraph("EventGraph"));
        const NodeId interact = b.Add("Event.Interface:Interactable.Interact");
        const NodeId self = b.Add("Entity.Self"), same = b.Add("Math.Equal:Entity"), p = b.Add("Debug.Print");
        b.Connect(interact, "instigator", same, "a").Connect(self, "self", same, "b");
        const NodeId fmt = b.Add("Text.Format", {{"format", "pulled (by itself: {me})"}});
        b.Connect(same, "result", fmt, "me").Connect(interact, "then", p, "exec").Connect(fmt, "result", p, "text");
    }
    // BP_Player: Use(target) prints Does Implement, then sends Interact.
    Blueprint player = Empty();
    {
        GraphBuilder b(*player.FindGraph("EventGraph"));
        const NodeId use = b.Add("Event.Custom", {{"name", "Use"}, {"params", EntityParam("target")}});
        const NodeId does = b.Add("Interface.Implements:Interactable"), p = b.Add("Debug.Print");
        b.Connect(use, "target", does, "target").Connect(use, "then", p, "exec").Connect(does, "result", p, "text");
        const NodeId call = b.Add("Interface.Call:Interactable.Interact"), self = b.Add("Entity.Self");
        b.Connect(p, "then", call, "exec").Connect(use, "target", call, "target").Connect(self, "self", call, "instigator");
    }
    World world;
    BlueprintVM vm(world);
    Lines printed;
    vm.SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });
    const Entity lv = world.CreateEntity(), pl = world.CreateEntity(), rock = world.CreateEntity();
    AETHER_CHECK(vm.Attach(lv, Compile(lever)) && vm.Attach(pl, Compile(player)));
    auto use = [&](Entity target) {
        const VmValue value = target;
        AETHER_CHECK(vm.Dispatch(pl, "Event.Custom:Use", std::span<const VmValue>(&value, 1)));
    };
    use(lv);
    AETHER_CHECK(printed == (Lines{"true", "pulled (by itself: false)"}));
    printed.clear();
    use(pl);   // an instance that doesn't implement it: nothing happens, no error
    use(rock); // not an instance at all: the same
    AETHER_CHECK(printed == (Lines{"false", "false"}) && vm.Errors().empty());

    // BP015: an interface event in a Blueprint that doesn't list the interface.
    Blueprint stray = Empty();
    GraphBuilder s(*stray.FindGraph("EventGraph"));
    const NodeId ev = s.Add("Event.Interface:Interactable.Interact");
    const CompileResult r = CompileBlueprint(stray);
    AETHER_CHECK(r.diagnostics.Has("BP015") && r.diagnostics.For("EventGraph", ev).size() == 1);
    // BP004: an unknown function or interface.
    Blueprint unknown = Empty();
    GraphBuilder u(*unknown.FindGraph("EventGraph"));
    u.Add("Interface.Call:Interactable.Explode");
    u.Add("Interface.Implements:Openable");
    AETHER_CHECK(CompileBlueprint(unknown).diagnostics.errors == 2);
    // The palette offers the interface's events to implementers.
    const std::vector<PaletteEntry> palette = ListNodeTypes(lever);
    AETHER_CHECK(std::any_of(palette.begin(), palette.end(), [](const PaletteEntry& e) {
        return e.id == "Event.Interface:Interactable.Highlight";
    }));
    Blueprint round;
    AETHER_CHECK(BlueprintFromJson(BlueprintToJson(lever), round) && round.interfaces == lever.interfaces);
}
