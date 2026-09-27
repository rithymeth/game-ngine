#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

using Lines = std::vector<std::string>;

Variable Pin(const std::string& name, const char* type, Value value = {}) {
    Variable v;
    v.name = name;
    v.type = *ParseType(type);
    v.default_value = std::holds_alternative<std::monostate>(value) ? DefaultValue(v.type) : value;
    return v;
}

struct MacroFixture {
    Blueprint bp;
    World world;
    Lines printed;
    std::unique_ptr<BlueprintVM> vm;
    Entity entity;

    MacroFixture() {
        Graph g;
        g.name = "EventGraph";
        bp.graphs.push_back(g);
    }
    GraphBuilder Events() { return GraphBuilder(*bp.FindGraph("EventGraph")); }
    Graph& Macro(const std::string& name, std::vector<Variable> inputs, std::vector<Variable> outputs) {
        Graph g;
        g.name = name;
        g.kind = GraphKind::Macro;
        g.inputs = std::move(inputs);
        g.outputs = std::move(outputs);
        bp.graphs.push_back(std::move(g));
        return bp.graphs.back();
    }
    bool Start() {
        CompileResult r = CompileBlueprint(bp);
        for (const Diagnostic& d : r.diagnostics.diagnostics) {
            if (d.severity == Severity::Error) std::printf("    %s %s node %u: %s\n", d.code.c_str(), d.graph.c_str(), d.node, d.message.c_str());
        }
        if (!r.Ok()) return false;
        vm = std::make_unique<BlueprintVM>(world);
        vm->SetPrintHandler([this](Entity, const std::string& t) { printed.push_back(t); });
        entity = world.CreateEntity();
        return vm->Attach(entity, r.blueprint);
    }
    Lines Fire(const std::string& event) {
        AETHER_CHECK(vm->Dispatch(entity, event));
        Lines out;
        out.swap(printed);
        return out;
    }
};

NodeId Print(GraphBuilder& b, NodeId after, const std::string& pin, const std::string& text) {
    const NodeId p = b.Add("Debug.Print");
    b.Default(p, "text", text);
    b.Connect(after, pin, p, "exec");
    return p;
}

NodeId PrintValue(GraphBuilder& b, NodeId after, const std::string& pin, NodeId value, const std::string& out) {
    const NodeId p = b.Add("Debug.Print");
    b.Connect(after, pin, p, "exec").Connect(value, out, p, "text");
    return p;
}

} // namespace

AETHER_TEST(BlueprintMacros_ExecAndDataMacrosInline) {
    MacroFixture f;
    // Twice: in -> Sequence -> out, out.
    {
        GraphBuilder m(f.Macro("Twice", {Pin("in", "exec")}, {Pin("out", "exec")}));
        const NodeId in = m.Add("Macro.Inputs"), seq = m.Add("Flow.Sequence"), out = m.Add("Macro.Outputs");
        m.Connect(in, "in", seq, "exec").Connect(seq, "then 0", out, "out").Connect(seq, "then 1", out, "out");
    }
    // Double (pure): y = x * 2, x defaults to 5.
    {
        GraphBuilder m(f.Macro("Double", {Pin("x", "float", 5.0f)}, {Pin("y", "float")}));
        const NodeId in = m.Add("Macro.Inputs"), mul = m.Add("Math.Multiply:float"), out = m.Add("Macro.Outputs");
        m.Default(mul, "b", 2.0);
        m.Connect(in, "x", mul, "a").Connect(mul, "result", out, "y");
    }
    // Quad: Double(Double(x)) — macros inside macros.
    {
        GraphBuilder m(f.Macro("Quad", {Pin("x", "float")}, {Pin("y", "float")}));
        const NodeId in = m.Add("Macro.Inputs"), d1 = m.Add("Macro:Double"), d2 = m.Add("Macro:Double"),
                     out = m.Add("Macro.Outputs");
        m.Connect(in, "x", d1, "x").Connect(d1, "y", d2, "x").Connect(d2, "y", out, "y");
    }
    // Pass: a value straight through a macro (Inputs -> Outputs).
    {
        GraphBuilder m(f.Macro("Pass", {Pin("v", "int")}, {Pin("v", "int")}));
        const NodeId in = m.Add("Macro.Inputs"), out = m.Add("Macro.Outputs");
        m.Connect(in, "v", out, "v");
    }
    GraphBuilder b = f.Events();
    const NodeId begin = b.Add("Event.BeginPlay"), twice = b.Add("Macro:Twice");
    b.Connect(begin, "then", twice, "in");
    const NodeId hi = Print(b, twice, "out", "hi");
    // Double(3), Double() with the instance's default 4, and with the macro's own default 5.
    const NodeId three = b.Add("Literal.Float", {{"value", 3.0}}), d = b.Add("Macro:Double");
    b.Connect(three, "value", d, "x");
    const NodeId d_default = b.Add("Macro:Double"), d_macro = b.Add("Macro:Double");
    b.Default(d_default, "x", 4.0);
    const NodeId seq = b.Add("Flow.Sequence", {{"count", 2}});
    b.Connect(hi, "then", seq, "exec"); // runs twice, like "hi"
    NodeId last = PrintValue(b, seq, "then 0", d, "y");
    last = PrintValue(b, last, "then", d_default, "y");
    last = PrintValue(b, last, "then", d_macro, "y");
    const NodeId q = b.Add("Macro:Quad"), pass = b.Add("Macro:Pass");
    b.Default(q, "x", 1.5).Default(pass, "v", 7);
    last = PrintValue(b, last, "then", q, "y");
    PrintValue(b, last, "then", pass, "v");
    AETHER_CHECK(f.Start());

    const Lines once = {"hi", "6", "8", "10", "6", "7"};
    Lines expected = once;
    expected.insert(expected.end(), once.begin(), once.end());
    AETHER_CHECK(f.Fire("Event.BeginPlay") == expected);

    // The .abp keeps exec pins in macro signatures.
    Blueprint again;
    std::string error;
    AETHER_CHECK(BlueprintFromJson(BlueprintToJson(f.bp), again, &error));
    AETHER_CHECK(again.FindGraph("Twice")->inputs[0].type.IsExec());
}

AETHER_TEST(BlueprintMacros_StateIsPerInstanceAndLatentWorks) {
    MacroFixture f;
    // OnceOnly: in -> Do Once -> out; reset -> Do Once.reset.
    {
        GraphBuilder m(f.Macro("OnceOnly", {Pin("in", "exec"), Pin("reset", "exec")}, {Pin("out", "exec")}));
        const NodeId in = m.Add("Macro.Inputs"), once = m.Add("Flow.DoOnce"), out = m.Add("Macro.Outputs");
        m.Connect(in, "in", once, "exec").Connect(in, "reset", once, "reset").Connect(once, "completed", out, "out");
    }
    // Later: in -> Delay(seconds) -> done.
    {
        GraphBuilder m(f.Macro("Later", {Pin("in", "exec"), Pin("seconds", "float", 1.0f)}, {Pin("done", "exec")}));
        const NodeId in = m.Add("Macro.Inputs"), delay = m.Add("Latent.Delay"), out = m.Add("Macro.Outputs");
        m.Connect(in, "in", delay, "exec").Connect(in, "seconds", delay, "duration").Connect(delay, "completed", out, "done");
    }
    GraphBuilder b = f.Events();
    for (const char* name : {"A", "B"}) {
        const NodeId go = b.Add("Event.Custom", {{"name", std::string("Go") + name}});
        const NodeId reset = b.Add("Event.Custom", {{"name", std::string("Reset") + name}});
        const NodeId once = b.Add("Macro:OnceOnly");
        b.Connect(go, "then", once, "in").Connect(reset, "then", once, "reset");
        Print(b, once, "out", name);
    }
    const NodeId wait = b.Add("Event.Custom", {{"name", "Wait"}}), later = b.Add("Macro:Later");
    b.Default(later, "seconds", 0.5);
    b.Connect(wait, "then", later, "in");
    Print(b, later, "done", "waited");
    AETHER_CHECK(f.Start());

    AETHER_CHECK(f.Fire("Event.Custom:GoA") == Lines{"A"});
    AETHER_CHECK(f.Fire("Event.Custom:GoA").empty());
    AETHER_CHECK(f.Fire("Event.Custom:GoB") == Lines{"B"}); // its own Do Once
    f.Fire("Event.Custom:ResetA");
    AETHER_CHECK(f.Fire("Event.Custom:GoA") == Lines{"A"});
    AETHER_CHECK(f.Fire("Event.Custom:GoB").empty());

    AETHER_CHECK(f.Fire("Event.Custom:Wait").empty());
    f.vm->Tick(0.6f);
    AETHER_CHECK(f.printed == Lines{"waited"});
}

AETHER_TEST(BlueprintMacros_ErrorsPointAtTheInstance) {
    // BP016: two macros containing each other.
    MacroFixture loop;
    GraphBuilder(loop.Macro("A", {}, {})).Add("Macro:B");
    GraphBuilder(loop.Macro("B", {}, {})).Add("Macro:A");
    const CompileResult r = CompileBlueprint(loop.bp);
    AETHER_CHECK(!r.Ok() && r.diagnostics.Has("BP016"));

    // BP004: an unknown macro; BP010: tunnels outside a macro graph.
    MacroFixture unknown;
    unknown.Events().Add("Macro:Nope");
    unknown.Events().Add("Macro.Inputs");
    const CompileResult ru = CompileBlueprint(unknown.bp);
    AETHER_CHECK(ru.diagnostics.Has("BP004") && ru.diagnostics.Has("BP010"));

    // A latent node inlined into a function is BP002, on the macro instance.
    MacroFixture fn;
    {
        GraphBuilder m(fn.Macro("Later", {Pin("in", "exec")}, {Pin("done", "exec")}));
        const NodeId in = m.Add("Macro.Inputs"), delay = m.Add("Latent.Delay"), out = m.Add("Macro.Outputs");
        m.Connect(in, "in", delay, "exec").Connect(delay, "completed", out, "done");
    }
    Graph f;
    f.name = "Work";
    f.kind = GraphKind::Function;
    GraphBuilder g(f);
    const NodeId entry = g.Add("Function.Entry"), inst = g.Add("Macro:Later");
    g.Connect(entry, "then", inst, "in");
    fn.bp.graphs.push_back(f);
    const CompileResult rf = CompileBlueprint(fn.bp);
    AETHER_CHECK(!rf.Ok() && rf.diagnostics.Has("BP002"));
    const std::vector<const Diagnostic*> on_inst = rf.diagnostics.For("Work", inst);
    AETHER_CHECK(on_inst.size() == 1 && on_inst[0]->message.rfind("Inside the macro", 0) == 0);
}
