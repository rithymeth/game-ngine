#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "aether/reflection/reflection.h"
#include "test_framework.h"

#include <algorithm>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

struct BpVmHealth {
    f32 current = 50.0f;
    i32 max = 100;
    f32 Heal(f32 amount) {
        current = std::min(static_cast<f32>(max), current + amount);
        return current;
    }
    bool IsFull() const { return current >= static_cast<f32>(max); }
};

AETHER_REFLECT(BpVmHealth, 1,
    AETHER_FIELD(current, Field_BlueprintReadWrite),
    AETHER_FIELD(max, Field_BlueprintReadWrite),
    AETHER_METHOD(Heal, Fn_BlueprintCallable, {"amount"}),
    AETHER_METHOD(IsFull, Fn_BlueprintCallable | Fn_Pure)
)

namespace {

const char* kDoor = R"({
  "$type": "Blueprint", "$version": 1, "parent": "native:Entity",
  "variables": [
    { "name": "IsOpen", "type": "bool", "default": false, "flags": ["InstanceEditable"] },
    { "name": "open_speed", "type": "float", "default": 1.0 }
  ],
  "graphs": [
    { "name": "EventGraph", "kind": "EventGraph",
      "nodes": [
        { "id": 1, "type": "Event.OnTriggerEnter" },
        { "id": 2, "type": "Flow.Branch" },
        { "id": 3, "type": "Var.Get:IsOpen" },
        { "id": 4, "type": "Call.Self:Open" }
      ],
      "links": [
        { "from": [1, "then"],  "to": [2, "exec"] },
        { "from": [3, "value"], "to": [2, "condition"] },
        { "from": [2, "false"], "to": [4, "exec"] }
      ] },
    { "name": "Open", "kind": "Function", "inputs": [], "outputs": [],
      "nodes": [
        { "id": 1, "type": "Function.Entry" },
        { "id": 2, "type": "Var.Set:IsOpen", "defaults": { "value": true } }
      ],
      "links": [ { "from": [1, "then"], "to": [2, "exec"] } ] }
  ]
})";

Blueprint Parse(const char* text) {
    Blueprint bp;
    std::string error;
    AETHER_CHECK(BlueprintFromJson(json::parse(text), bp, &error));
    return bp;
}

std::shared_ptr<const CompiledBlueprint> Compile(const Blueprint& bp) {
    CompileResult result = CompileBlueprint(bp);
    for (const Diagnostic& d : result.diagnostics.diagnostics) {
        if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
    }
    AETHER_CHECK(result.Ok());
    return result.blueprint;
}

Variable Var(const std::string& name, const char* type, Value value = {}, u32 flags = Var_None) {
    Variable v;
    v.name = name;
    v.type = *ParseType(type);
    v.default_value = std::holds_alternative<std::monostate>(value) ? DefaultValue(v.type) : value;
    v.flags = flags;
    return v;
}

// An empty Blueprint with one Event Graph, and a builder for it.
struct Fixture {
    World world;
    Blueprint bp;
    std::vector<std::string> printed;

    Fixture() {
        Graph g;
        g.name = "EventGraph";
        bp.graphs.push_back(g);
    }
    GraphBuilder Events() { return GraphBuilder(*bp.FindGraph("EventGraph")); }
    Graph& AddFunction(const std::string& name, bool pure = false) {
        Graph g;
        g.name = name;
        g.kind = GraphKind::Function;
        g.pure = pure;
        bp.graphs.push_back(g);
        return bp.graphs.back();
    }
    // Compiles, attaches to a new entity, and captures Print String.
    Entity Spawn(BlueprintVM& vm, const json& overrides = json::object()) {
        const Entity e = world.CreateEntity();
        AETHER_CHECK(vm.Attach(e, Compile(bp), overrides));
        vm.SetPrintHandler([this](Entity, const std::string& text) { printed.push_back(text); });
        return e;
    }
};

} // namespace

AETHER_TEST(BlueprintVM_DoorCompilesToGoldenBytecodeAndRuns) {
    const Blueprint bp = Parse(kDoor);
    std::shared_ptr<const CompiledBlueprint> compiled = Compile(bp);
    AETHER_CHECK(compiled != nullptr);
    if (!compiled) return;
    AETHER_CHECK(compiled->functions.size() == 2 && compiled->events.count("Event.OnTriggerEnter") == 1);

    // Golden bytecode (docs/ROADMAP_DETAILS.md §C.3): opcode changes show up here.
    const std::string trigger = Disassemble(*compiled, *compiled->FindEvent("Event.OnTriggerEnter"));
    const std::string open = Disassemble(*compiled, compiled->functions[compiled->function_index.at("Open")]);
    const char* kTriggerGolden =
        "0000  GETVAR    r1, IsOpen                   ; node 3\n" // the pure Get, emitted for the Branch
        "0001  JMPF      r1, @3                       ; node 2\n"
        "0002  JMP       @4                           ; node 2\n"
        "0003  CALLB     Open()                       ; node 4\n"
        "0004  RET                                    ; node 1\n";
    const char* kOpenGolden =
        "0000  LOADK     r0, k0                       ; node 2\n"
        "0001  SETVAR    r0, IsOpen                   ; node 2\n"
        "0002  MOVE      r1, r0                       ; node 2\n"
        "0003  RET                                    ; node 1\n";
    AETHER_CHECK(trigger == kTriggerGolden);
    AETHER_CHECK(open == kOpenGolden);
    if (trigger != kTriggerGolden) std::printf("%s", trigger.c_str());
    if (open != kOpenGolden) std::printf("%s", open.c_str());

    World world;
    BlueprintVM vm(world);
    const Entity door = world.CreateEntity(), player = world.CreateEntity();
    AETHER_CHECK(vm.Attach(door, compiled));
    AETHER_CHECK(std::get<bool>(vm.GetVariable(door, "IsOpen")) == false);
    const VmValue other = player;
    AETHER_CHECK(vm.Dispatch(door, "Event.OnTriggerEnter", std::span<const VmValue>(&other, 1)));
    AETHER_CHECK(std::get<bool>(vm.GetVariable(door, "IsOpen")) == true);
    AETHER_CHECK(vm.Dispatch(door, "Event.OnTriggerEnter")); // already open: the true pin is unconnected
    AETHER_CHECK(!vm.Dispatch(door, "Event.Tick"));          // no such event
    AETHER_CHECK(!vm.Dispatch(player, "Event.OnTriggerEnter")); // not an instance

    // Instance-editable overrides apply per instance; others are ignored.
    const Entity open_door = world.CreateEntity();
    AETHER_CHECK(vm.Attach(open_door, compiled, {{"IsOpen", true}, {"open_speed", 9.0}}));
    AETHER_CHECK(std::get<bool>(vm.GetVariable(open_door, "IsOpen")) == true);
    AETHER_CHECK(std::get<f32>(vm.GetVariable(open_door, "open_speed")) == 1.0f);
    AETHER_CHECK(vm.InstanceCount() == 2 && vm.Errors().empty());
    vm.Detach(open_door);
    AETHER_CHECK(!vm.IsAttached(open_door) && vm.InstanceCount() == 1);
}

AETHER_TEST(BlueprintVM_MathFlowConversionsAndPureCaching) {
    Fixture f;
    f.bp.variables = {Var("count", "int"), Var("name", "string", std::string("bob"))};
    GraphBuilder b = f.Events();
    const NodeId begin = b.Add("Event.BeginPlay");
    const NodeId seq = b.Add("Flow.Sequence", {{"count", 5}});
    b.Connect(begin, "then", seq, "exec");

    // then 0: Print(Add:float(int 2, 1.5)) — int -> float and float -> string.
    const NodeId two = b.Add("Literal.Int", {{"value", 2}}), add = b.Add("Math.Add:float");
    b.Default(add, "b", 1.5);
    const NodeId p0 = b.Add("Debug.Print");
    b.Connect(two, "value", add, "a").Connect(add, "result", p0, "text").Connect(seq, "then 0", p0, "exec");

    // then 1: count = count + 1, twice (the Get is re-read for each Set: per exec step).
    const NodeId get = b.Add("Var.Get:count"), inc = b.Add("Math.Add:int");
    b.Default(inc, "b", 1);
    b.Connect(get, "value", inc, "a");
    const NodeId set1 = b.Add("Var.Set:count"), set2 = b.Add("Var.Set:count");
    b.Connect(inc, "result", set1, "value").Connect(inc, "result", set2, "value");
    b.Connect(seq, "then 1", set1, "exec").Connect(set1, "then", set2, "exec");

    // then 2: Branch(count < 3) -> Print "small" / "big" (count is 2 by now).
    const NodeId get2 = b.Add("Var.Get:count"), less = b.Add("Math.Less:int"), branch = b.Add("Flow.Branch");
    b.Default(less, "b", 3);
    b.Connect(get2, "value", less, "a").Connect(less, "result", branch, "condition").Connect(seq, "then 2", branch, "exec");
    const NodeId small = b.Add("Debug.Print"), big = b.Add("Debug.Print");
    b.Default(small, "text", "small").Default(big, "text", "big");
    b.Connect(branch, "true", small, "exec").Connect(branch, "false", big, "exec");

    // then 3: Print(vector) and Print(name == "bob").
    const NodeId make = b.Add("Vec3.Make"), p3 = b.Add("Debug.Print");
    b.Default(make, "x", 1).Default(make, "y", 2.5).Default(make, "z", -1);
    b.Connect(make, "result", p3, "text").Connect(seq, "then 3", p3, "exec");
    const NodeId name = b.Add("Var.Get:name"), eq = b.Add("Math.Equal:string"), p4 = b.Add("Debug.Print");
    b.Default(eq, "b", "bob");
    b.Connect(name, "value", eq, "a").Connect(eq, "result", p4, "text").Connect(p3, "then", p4, "exec");

    // then 4: int divide by zero gives 0; float -> int truncates; self prints.
    const NodeId div = b.Add("Math.Divide:int"), p5 = b.Add("Debug.Print");
    b.Default(div, "a", 7).Default(div, "b", 0);
    b.Connect(div, "result", p5, "text").Connect(seq, "then 4", p5, "exec");
    const NodeId half = b.Add("Literal.Float", {{"value", 9.9}}), trunc = b.Add("Math.Add:int"), p6 = b.Add("Debug.Print");
    b.Connect(half, "value", trunc, "a").Connect(trunc, "result", p6, "text").Connect(p5, "then", p6, "exec");
    const NodeId self = b.Add("Entity.Self"), valid = b.Add("Entity.IsValid"), p7 = b.Add("Debug.Print");
    b.Connect(self, "self", valid, "entity").Connect(valid, "result", p7, "text").Connect(p6, "then", p7, "exec");

    BlueprintVM vm(f.world);
    const Entity e = f.Spawn(vm);
    AETHER_CHECK(vm.Dispatch(e, "Event.BeginPlay"));
    const std::vector<std::string> expected = {"3.5", "small", "(1, 2.5, -1)", "true", "0", "9", "true"};
    AETHER_CHECK(f.printed == expected);
    AETHER_CHECK(std::get<i32>(vm.GetVariable(e, "count")) == 2);

    // Variables from C++; a second instance has its own.
    AETHER_CHECK(vm.SetVariable(e, "name", std::string("amy")) && !vm.SetVariable(e, "name", 3));
    AETHER_CHECK(std::get<std::string>(vm.GetVariable(e, "name")) == "amy");
    const Entity e2 = f.Spawn(vm);
    AETHER_CHECK(std::get<std::string>(vm.GetVariable(e2, "name")) == "bob");
    AETHER_CHECK(std::holds_alternative<std::monostate>(vm.GetVariable(e, "nope")));
}

AETHER_TEST(BlueprintVM_TickNativeCallsAndFields) {
    (void)GetComponentId<BpVmHealth>();
    Fixture f;
    f.bp.variables = {Var("time", "float")};
    GraphBuilder b = f.Events();
    // Tick: time = time + delta_seconds.
    const NodeId tick = b.Add("Event.Tick"), get = b.Add("Var.Get:time"), add = b.Add("Math.Add:float");
    const NodeId set = b.Add("Var.Set:time");
    b.Connect(tick, "then", set, "exec").Connect(get, "value", add, "a").Connect(tick, "delta_seconds", add, "b");
    b.Connect(add, "result", set, "value");
    // BeginPlay: Print(Heal(25)), Print(IsFull), current = 100, Print(current).
    const NodeId begin = b.Add("Event.BeginPlay"), heal = b.Add("Call.Native:BpVmHealth.Heal"), p1 = b.Add("Debug.Print");
    b.Default(heal, "amount", 25.0);
    b.Connect(begin, "then", heal, "exec").Connect(heal, "then", p1, "exec").Connect(heal, "return", p1, "text");
    const NodeId full = b.Add("Call.Native:BpVmHealth.IsFull"), p2 = b.Add("Debug.Print");
    b.Connect(p1, "then", p2, "exec").Connect(full, "return", p2, "text");
    const NodeId fill = b.Add("Comp.Set:BpVmHealth.current"), current = b.Add("Comp.Get:BpVmHealth.current");
    const NodeId p3 = b.Add("Debug.Print");
    b.Default(fill, "value", 100.0);
    b.Connect(p2, "then", fill, "exec").Connect(fill, "then", p3, "exec").Connect(current, "value", p3, "text");
    const NodeId full2 = b.Add("Call.Native:BpVmHealth.IsFull"), p4 = b.Add("Debug.Print");
    b.Connect(p3, "then", p4, "exec").Connect(full2, "return", p4, "text");
    // Custom event "Poke": heal an entity without Health (BP201, once).
    const NodeId poke = b.Add("Event.Custom", {{"name", "Poke"}, {"params", {{{"name", "who"}, {"type", "Entity"}}}}});
    const NodeId heal_other = b.Add("Call.Native:BpVmHealth.Heal"), p5 = b.Add("Debug.Print");
    b.Connect(poke, "then", heal_other, "exec").Connect(poke, "who", heal_other, "target");
    b.Connect(heal_other, "then", p5, "exec").Connect(heal_other, "return", p5, "text");

    BlueprintVM vm(f.world);
    const Entity e = f.Spawn(vm);
    f.world.AddComponent(e, BpVmHealth{});
    const Entity no_tick = f.world.CreateEntity();
    Fixture other;
    GraphBuilder(*other.bp.FindGraph("EventGraph")).Add("Event.BeginPlay");
    AETHER_CHECK(vm.Attach(no_tick, Compile(other.bp)));

    vm.BeginPlay();
    AETHER_CHECK(f.printed == (std::vector<std::string>{"75", "false", "100", "true"}));
    AETHER_CHECK(f.world.GetComponent<BpVmHealth>(e)->current == 100.0f);
    vm.Tick(0.25f);
    vm.Tick(0.5f);
    AETHER_CHECK(std::get<f32>(vm.GetVariable(e, "time")) == 0.75f);

    const Entity stranger = f.world.CreateEntity();
    const VmValue who = stranger;
    f.printed.clear();
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:Poke", std::span<const VmValue>(&who, 1)));
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:Poke", std::span<const VmValue>(&who, 1)));
    AETHER_CHECK(f.printed == (std::vector<std::string>{"0", "0"})); // the default result, and execution went on
    AETHER_CHECK(vm.Errors().size() == 1 && vm.Errors()[0].code == "BP201" && vm.Errors()[0].node == heal_other);
    AETHER_CHECK(vm.Errors()[0].message.find("Is Valid") != std::string::npos);
}

AETHER_TEST(BlueprintVM_FunctionsAndCustomEvents) {
    Fixture f;
    f.bp.variables = {Var("total", "float")};
    // Pure function Double(x) -> y = x * 2.
    Graph& dbl = f.AddFunction("Double", true);
    dbl.inputs = {Var("x", "float")};
    dbl.outputs = {Var("y", "float")};
    {
        GraphBuilder g(dbl);
        const NodeId entry = g.Add("Function.Entry"), mul = g.Add("Math.Multiply:float"), ret = g.Add("Function.Return");
        g.Default(mul, "b", 2.0);
        g.Connect(entry, "x", mul, "a").Connect(mul, "result", ret, "y");
    }
    // Impure function AddTo(amount) -> new_total: total += amount; returns early past a Branch.
    Graph& add_to = f.AddFunction("AddTo");
    add_to.inputs = {Var("amount", "float")};
    add_to.outputs = {Var("new_total", "float")};
    {
        GraphBuilder g(add_to);
        const NodeId entry = g.Add("Function.Entry"), get = g.Add("Var.Get:total"), add = g.Add("Math.Add:float");
        const NodeId set = g.Add("Var.Set:total"), ret = g.Add("Function.Return");
        g.Connect(entry, "then", set, "exec").Connect(get, "value", add, "a").Connect(entry, "amount", add, "b");
        g.Connect(add, "result", set, "value").Connect(set, "then", ret, "exec").Connect(set, "value", ret, "new_total");
    }
    GraphBuilder b = f.Events();
    // Custom event Hit(amount): Print(AddTo(Double(amount))).
    const NodeId hit = b.Add("Event.Custom", {{"name", "Hit"}, {"params", {{{"name", "amount"}, {"type", "float"}}}}});
    const NodeId call_double = b.Add("Call.Self:Double"), call_add = b.Add("Call.Self:AddTo"), print = b.Add("Debug.Print");
    b.Connect(hit, "amount", call_double, "x").Connect(call_double, "y", call_add, "amount");
    b.Connect(hit, "then", call_add, "exec").Connect(call_add, "then", print, "exec").Connect(call_add, "new_total", print, "text");
    // BeginPlay calls Hit(5) through a Call node.
    const NodeId begin = b.Add("Event.BeginPlay"), call_hit = b.Add("Call.Custom:Hit");
    b.Default(call_hit, "amount", 5.0);
    b.Connect(begin, "then", call_hit, "exec");

    BlueprintVM vm(f.world);
    const Entity e = f.Spawn(vm);
    AETHER_CHECK(vm.Dispatch(e, "Event.BeginPlay"));
    const VmValue three = 3.0f;
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:Hit", std::span<const VmValue>(&three, 1)));
    AETHER_CHECK(f.printed == (std::vector<std::string>{"10", "16"}));
    AETHER_CHECK(std::get<f32>(vm.GetVariable(e, "total")) == 16.0f);
}

AETHER_TEST(BlueprintVM_BudgetRecursionAndCompileErrors) {
    // An exec loop: Set count -> back into the same Set. BP202 names the node.
    Fixture loop;
    loop.bp.variables = {Var("count", "int")};
    {
        GraphBuilder b = loop.Events();
        const NodeId begin = b.Add("Event.BeginPlay"), get = b.Add("Var.Get:count"), inc = b.Add("Math.Add:int");
        const NodeId set = b.Add("Var.Set:count");
        b.Default(inc, "b", 1);
        b.Connect(begin, "then", set, "exec").Connect(get, "value", inc, "a").Connect(inc, "result", set, "value");
        b.Connect(set, "then", set, "exec");
    }
    BlueprintVM::Options options;
    options.instruction_budget = 1000;
    BlueprintVM vm(loop.world, options);
    const Entity e = loop.Spawn(vm);
    AETHER_CHECK(!vm.Dispatch(e, "Event.BeginPlay"));
    AETHER_CHECK(vm.Errors().size() == 1 && vm.Errors()[0].code == "BP202" && vm.Errors()[0].node == 4);
    const i32 count = std::get<i32>(vm.GetVariable(e, "count"));
    AETHER_CHECK(count > 100 && count < 1000); // it ran, then was stopped
    // The budget is per dispatch: the next one starts fresh.
    AETHER_CHECK(!vm.Dispatch(e, "Event.BeginPlay") && std::get<i32>(vm.GetVariable(e, "count")) > count);

    // Endless recursion: a function calling itself. BP203.
    Fixture rec;
    Graph& self_call = rec.AddFunction("Forever");
    {
        GraphBuilder g(self_call);
        const NodeId entry = g.Add("Function.Entry"), call = g.Add("Call.Self:Forever");
        g.Connect(entry, "then", call, "exec");
    }
    {
        GraphBuilder b = rec.Events();
        const NodeId begin = b.Add("Event.BeginPlay"), call = b.Add("Call.Self:Forever");
        b.Connect(begin, "then", call, "exec");
    }
    BlueprintVM vm2(rec.world);
    const Entity r = rec.Spawn(vm2);
    AETHER_CHECK(!vm2.Dispatch(r, "Event.BeginPlay"));
    AETHER_CHECK(!vm2.Errors().empty() && vm2.Errors()[0].code == "BP203");

    // Values Blueprints can't run yet fail to compile (BP012); validation errors too.
    Fixture unsupported;
    unsupported.bp.variables = {Var("where", "Transform")};
    {
        GraphBuilder b = unsupported.Events();
        const NodeId begin = b.Add("Event.BeginPlay"), set = b.Add("Var.Set:where");
        b.Connect(begin, "then", set, "exec");
    }
    CompileResult bad = CompileBlueprint(unsupported.bp);
    AETHER_CHECK(!bad.Ok() && bad.diagnostics.Has("BP012"));
    Fixture invalid;
    invalid.Events().Add("Var.Get:missing");
    CompileResult bad2 = CompileBlueprint(invalid.bp);
    AETHER_CHECK(!bad2.Ok() && bad2.diagnostics.Has("BP005"));
}
