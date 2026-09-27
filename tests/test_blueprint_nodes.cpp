#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "test_framework.h"

#include <algorithm>
#include <cmath>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

using Lines = std::vector<std::string>;

// One Blueprint whose BeginPlay runs `build`'s nodes; returns what they printed.
struct Graphs {
    Blueprint bp;
    World world;
    std::vector<std::string> printed;
    BlueprintVM::Options options;

    Graphs() {
        Graph g;
        g.name = "EventGraph";
        bp.graphs.push_back(g);
        options.random_seed = 42;
    }
    GraphBuilder Events() { return GraphBuilder(*bp.FindGraph("EventGraph")); }
    void AddVariable(const std::string& name, const char* type) {
        Variable v;
        v.name = name;
        v.type = *ParseType(type);
        v.default_value = DefaultValue(v.type);
        bp.variables.push_back(v);
    }
    Lines Run(const std::string& event = "Event.BeginPlay", std::vector<VmValue> args = {}) {
        CompileResult result = CompileBlueprint(bp);
        for (const Diagnostic& d : result.diagnostics.diagnostics) {
            if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
        }
        AETHER_CHECK(result.Ok());
        if (!result.Ok()) return {};
        BlueprintVM vm(world, options);
        vm.SetPrintHandler([this](Entity, const std::string& text) { printed.push_back(text); });
        const Entity e = world.CreateEntity();
        vm.Attach(e, result.blueprint);
        AETHER_CHECK(vm.Dispatch(e, event, args));
        AETHER_CHECK(vm.Errors().empty());
        Lines out;
        out.swap(printed);
        return out;
    }
};

// Prints `value_node.pin` after `after.after_pin`; returns the Print node.
NodeId PrintValue(GraphBuilder& b, NodeId after, const std::string& after_pin, NodeId value_node, const std::string& pin) {
    const NodeId p = b.Add("Debug.Print");
    b.Connect(after, after_pin, p, "exec").Connect(value_node, pin, p, "text");
    return p;
}

// A chain of pure values printed in order from BeginPlay.
struct PrintChain {
    GraphBuilder b;
    NodeId last;
    std::string pin = "then";
    explicit PrintChain(Graphs& g) : b(g.Events()), last(b.Add("Event.BeginPlay")) {}
    void Print(NodeId node, const std::string& out = "result") {
        last = PrintValue(b, last, pin, node, out);
        pin = "then";
    }
};

} // namespace

AETHER_TEST(BlueprintNodes_ForLoopsWhileAndBreak) {
    Graphs g;
    g.AddVariable("n", "int");
    GraphBuilder b = g.Events();
    const NodeId begin = b.Add("Event.BeginPlay"), seq = b.Add("Flow.Sequence", {{"count", 3}});
    b.Connect(begin, "then", seq, "exec");
    // For Loop 1..3: prints the index, then "done".
    const NodeId loop = b.Add("Flow.ForLoop");
    b.Default(loop, "first", 1).Default(loop, "last", 3);
    b.Connect(seq, "then 0", loop, "exec");
    PrintValue(b, loop, "loop_body", loop, "index");
    const NodeId done = b.Add("Debug.Print");
    b.Default(done, "text", "done");
    b.Connect(loop, "completed", done, "exec");
    // For Loop with Break 0..9, breaking once index == 2.
    const NodeId brk = b.Add("Flow.ForLoopWithBreak"), eq = b.Add("Math.Equal:int"), branch = b.Add("Flow.Branch");
    b.Default(brk, "first", 0).Default(brk, "last", 9).Default(eq, "b", 2);
    b.Connect(seq, "then 1", brk, "exec").Connect(brk, "index", eq, "a");
    const NodeId pb = PrintValue(b, brk, "loop_body", brk, "index");
    b.Connect(pb, "then", branch, "exec").Connect(eq, "result", branch, "condition");
    b.Connect(branch, "true", brk, "break");
    const NodeId after_break = b.Add("Debug.Print");
    b.Default(after_break, "text", "broke");
    b.Connect(brk, "completed", after_break, "exec");
    // While n < 3: n = n + 1; then print n. The condition is re-read each time.
    const NodeId wl = b.Add("Flow.WhileLoop"), get = b.Add("Var.Get:n"), less = b.Add("Math.Less:int");
    b.Default(less, "b", 3);
    b.Connect(seq, "then 2", wl, "exec").Connect(get, "value", less, "a").Connect(less, "result", wl, "condition");
    const NodeId get2 = b.Add("Var.Get:n"), inc = b.Add("Math.Add:int"), set = b.Add("Var.Set:n");
    b.Default(inc, "b", 1);
    b.Connect(wl, "loop_body", set, "exec").Connect(get2, "value", inc, "a").Connect(inc, "result", set, "value");
    const NodeId get3 = b.Add("Var.Get:n");
    PrintValue(b, wl, "completed", get3, "value");

    AETHER_CHECK(g.Run() == (Lines{"1", "2", "3", "done", "0", "1", "2", "broke", "3"}));

    // An empty range runs no body; completed still fires.
    Graphs empty;
    GraphBuilder e = empty.Events();
    const NodeId start = e.Add("Event.BeginPlay"), none = e.Add("Flow.ForLoop");
    e.Default(none, "first", 5).Default(none, "last", 4);
    e.Connect(start, "then", none, "exec");
    PrintValue(e, none, "loop_body", none, "index");
    const NodeId fin = e.Add("Debug.Print");
    e.Default(fin, "text", "end");
    e.Connect(none, "completed", fin, "exec");
    AETHER_CHECK(empty.Run() == Lines{"end"});
}

AETHER_TEST(BlueprintNodes_SwitchesAndSelect) {
    Graphs g;
    GraphBuilder b = g.Events();
    // Pick(k: int, name: string): Switch on Int [1, 5] and Switch on String [red, blue].
    const NodeId pick = b.Add("Event.Custom", {{"name", "Pick"},
                                               {"params", {{{"name", "k"}, {"type", "int"}}, {{"name", "name"}, {"type", "string"}}}}});
    const NodeId si = b.Add("Flow.SwitchInt", {{"cases", {1, 5}}});
    b.Connect(pick, "then", si, "exec").Connect(pick, "k", si, "selection");
    const NodeId ss = b.Add("Flow.SwitchString", {{"cases", {"red", "blue"}}});
    b.Connect(pick, "name", ss, "selection");
    for (const char* pin : {"1", "5", "default"}) {
        const NodeId p = b.Add("Debug.Print");
        b.Default(p, "text", std::string("int:") + pin);
        b.Connect(si, pin, p, "exec").Connect(p, "then", ss, "exec");
    }
    for (const char* pin : {"red", "blue", "default"}) {
        const NodeId p = b.Add("Debug.Print");
        b.Default(p, "text", std::string("str:") + pin);
        b.Connect(ss, pin, p, "exec");
    }
    // Select by index; an out-of-range index gives the type's default.
    const NodeId shown = b.Add("Event.Custom", {{"name", "Show"}, {"params", {{{"name", "k"}, {"type", "int"}}}}});
    const NodeId sel = b.Add("Flow.Select:float"), words = b.Add("Flow.Select:string", {{"count", 3}});
    b.Default(sel, "option 0", 1.5).Default(sel, "option 1", 2.5);
    b.Default(words, "option 0", "zero").Default(words, "option 1", "one").Default(words, "option 2", "two");
    b.Connect(shown, "k", sel, "index").Connect(shown, "k", words, "index");
    PrintValue(b, PrintValue(b, shown, "then", sel, "return"), "then", words, "return");

    AETHER_CHECK(g.Run("Event.Custom:Pick", {i32{5}, std::string("blue")}) == (Lines{"int:5", "str:blue"}));
    AETHER_CHECK(g.Run("Event.Custom:Pick", {i32{1}, std::string("green")}) == (Lines{"int:1", "str:default"}));
    AETHER_CHECK(g.Run("Event.Custom:Pick", {i32{3}, std::string("red")}) == (Lines{"int:default", "str:red"}));
    AETHER_CHECK(g.Run("Event.Custom:Show", {i32{1}}) == (Lines{"2.5", "one"}));
    AETHER_CHECK(g.Run("Event.Custom:Show", {i32{7}}) == (Lines{"0", ""}));

    // Bad configs are refused on the node.
    Graphs bad;
    bad.Events().Add("Flow.SwitchInt", {{"cases", {1, 1}}});
    CompileResult r = CompileBlueprint(bad.bp);
    AETHER_CHECK(!r.Ok() && r.diagnostics.Has("BP007"));
    Graphs bad2;
    bad2.Events().Add("Flow.SwitchString", {{"cases", {"default"}}});
    AETHER_CHECK(CompileBlueprint(bad2.bp).diagnostics.Has("BP007"));
    Graphs bad3;
    bad3.Events().Add("Flow.Select:Wildcard");
    AETHER_CHECK(CompileBlueprint(bad3.bp).diagnostics.Has("BP007"));
}

AETHER_TEST(BlueprintNodes_StringsFormatAndConversions) {
    AETHER_CHECK(ParseFormatArgs("Hi {name}, {n} left ({name}) {{x}}") == (std::vector<std::string>{"name", "n"}));

    Graphs g;
    PrintChain c(g);
    GraphBuilder& b = c.b;
    const NodeId append = b.Add("String.Append", {{"count", 3}});
    b.Default(append, "a", "ab").Default(append, "b", "-").Default(append, "c", "cd");
    c.Print(append);
    // Format Text with a number and a vector converted to text, and escaped braces.
    const NodeId fmt = b.Add("Text.Format", {{"format", "{who} has {n} coins at {where} {{ok}}"}});
    const NodeId n = b.Add("Literal.Int", {{"value", 12}}), where = b.Add("Vec3.Make");
    b.Default(fmt, "who", "Ada").Connect(n, "value", fmt, "n").Connect(where, "result", fmt, "where");
    b.Default(where, "x", 1);
    c.Print(fmt);
    const NodeId len = b.Add("String.Length");
    b.Default(len, "text", "héllo"); // bytes, not characters
    c.Print(len);
    const NodeId has = b.Add("String.Contains"), has_nocase = b.Add("String.Contains");
    b.Default(has, "text", "Hello World").Default(has, "substring", "world");
    b.Default(has_nocase, "text", "Hello World").Default(has_nocase, "substring", "world").Default(has_nocase, "ignore_case", true);
    c.Print(has);
    c.Print(has_nocase);
    const NodeId up = b.Add("String.ToUpper"), trim = b.Add("String.Trim"), empty = b.Add("String.IsEmpty");
    b.Default(up, "text", "loud").Default(trim, "text", "  tidy \t").Default(empty, "text", "");
    c.Print(up);
    c.Print(trim);
    c.Print(empty);
    const NodeId to_int = b.Add("String.ToInt"), bad_int = b.Add("String.ToInt"), to_float = b.Add("String.ToFloat");
    b.Default(to_int, "text", "-42").Default(bad_int, "text", "42abc").Default(to_float, "text", "2.5");
    c.Print(to_int);
    c.Print(bad_int, "success");
    c.Print(to_float);
    const NodeId as_text = b.Add("Conv.ToString:bool"), yes = b.Add("Literal.Bool", {{"value", true}});
    b.Connect(yes, "value", as_text, "value");
    c.Print(as_text);

    AETHER_CHECK(g.Run() == (Lines{"ab-cd", "Ada has 12 coins at (1, 0, 0) {ok}", "6", "false", "true", "LOUD", "tidy",
                                   "true", "-42", "false", "2.5", "true"}));
    Graphs bad;
    bad.Events().Add("Conv.ToString:string");
    AETHER_CHECK(CompileBlueprint(bad.bp).diagnostics.Has("BP007"));
}

AETHER_TEST(BlueprintNodes_MoreMathAndRandom) {
    Graphs g;
    PrintChain c(g);
    GraphBuilder& b = c.b;
    auto unary = [&](const char* type, double a, const char* out = "result") {
        const NodeId n = b.Add(type);
        b.Default(n, "a", a);
        c.Print(n, out);
    };
    unary("Math.Sqrt", 16.0);
    unary("Math.Sqrt", -4.0); // safe: 0
    unary("Math.Floor", -1.5);
    unary("Math.Ceil", 1.2);
    unary("Math.Round", 2.5);
    unary("Math.Truncate", -1.9);
    unary("Math.Frac", 3.25);
    unary("Math.RadiansToDegrees", 3.14159265);
    const NodeId pow = b.Add("Math.Power");
    b.Default(pow, "base", 2.0).Default(pow, "exponent", 10.0);
    c.Print(pow);
    const NodeId clamp = b.Add("Math.Clamp:int");
    b.Default(clamp, "value", 15).Default(clamp, "max", 10);
    c.Print(clamp);
    const NodeId near = b.Add("Math.NearlyEqual:float");
    b.Default(near, "a", 1.0).Default(near, "b", 1.00001);
    c.Print(near);
    const NodeId map = b.Add("Math.MapRange:float");
    b.Default(map, "value", 15.0).Default(map, "in_min", 10.0).Default(map, "in_max", 20.0);
    b.Default(map, "out_min", 0.0).Default(map, "out_max", 100.0);
    c.Print(map);
    const NodeId map_clamped = b.Add("Math.MapRange:float");
    b.Default(map_clamped, "value", 99.0).Default(map_clamped, "in_max", 10.0).Default(map_clamped, "out_max", 2.0);
    c.Print(map_clamped);
    const NodeId neg = b.Add("Math.Negate:int");
    b.Default(neg, "a", 5);
    c.Print(neg);

    const Lines out = g.Run();
    AETHER_CHECK(out == (Lines{"4", "0", "-2", "2", "3", "-1", "0.25", "180", "1024", "10", "true", "50", "2", "-5"}));

    // Random: within range, and reproducible for a seed.
    Graphs r;
    PrintChain rc(r);
    for (int i = 0; i < 20; ++i) {
        const NodeId ri = rc.b.Add("Math.RandomIntInRange"), rf = rc.b.Add("Math.RandomFloatInRange");
        rc.b.Default(ri, "min", 3).Default(ri, "max", 6).Default(rf, "min", -1.0).Default(rf, "max", 1.0);
        rc.Print(ri);
        rc.Print(rf);
    }
    const Lines first = r.Run(), second = r.Run();
    AETHER_CHECK(first.size() == 40 && first == second);
    bool all_in_range = true, varied = false;
    for (usize i = 0; i < first.size(); i += 2) {
        const int v = std::stoi(first[i]);
        const float f = std::stof(first[i + 1]);
        all_in_range &= v >= 3 && v <= 6 && f >= -1.0f && f <= 1.0f;
        varied |= first[i] != first[0];
    }
    AETHER_CHECK(all_in_range && varied);
}
