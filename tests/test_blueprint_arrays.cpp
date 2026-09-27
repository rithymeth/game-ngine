#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

namespace {

using Lines = std::vector<std::string>;

struct ArrayGraphs {
    Blueprint bp;
    World world;
    Lines printed;
    std::unique_ptr<BlueprintVM> vm;
    Entity entity;

    ArrayGraphs() {
        Graph g;
        g.name = "EventGraph";
        bp.graphs.push_back(g);
    }
    GraphBuilder Events() { return GraphBuilder(*bp.FindGraph("EventGraph")); }
    void AddVariable(const std::string& name, const char* type) {
        Variable v;
        v.name = name;
        v.type = *ParseType(type);
        v.default_value = DefaultValue(v.type);
        bp.variables.push_back(v);
    }
    bool Start() {
        CompileResult result = CompileBlueprint(bp);
        for (const Diagnostic& d : result.diagnostics.diagnostics) {
            if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
        }
        if (!result.Ok()) return false;
        vm = std::make_unique<BlueprintVM>(world);
        vm->SetPrintHandler([this](Entity, const std::string& text) { printed.push_back(text); });
        entity = world.CreateEntity();
        return vm->Attach(entity, result.blueprint);
    }
    Lines Fire(const std::string& event) {
        AETHER_CHECK(vm->Dispatch(entity, event));
        Lines out;
        out.swap(printed);
        return out;
    }
};

bool Ints(const std::vector<VmValue>& items, const std::vector<i32>& expected) {
    if (items.size() != expected.size()) return false;
    for (usize i = 0; i < items.size(); ++i) {
        const i32* v = std::get_if<i32>(&items[i]);
        if (v == nullptr || *v != expected[i]) return false;
    }
    return true;
}

bool Texts(const std::vector<VmValue>& items, const std::vector<std::string>& expected) {
    if (items.size() != expected.size()) return false;
    for (usize i = 0; i < items.size(); ++i) {
        const std::string* v = std::get_if<std::string>(&items[i]);
        if (v == nullptr || *v != expected[i]) return false;
    }
    return true;
}

NodeId PrintValue(GraphBuilder& b, NodeId after, const std::string& after_pin, NodeId value, const std::string& pin) {
    const NodeId p = b.Add("Debug.Print");
    b.Connect(after, after_pin, p, "exec").Connect(value, pin, p, "text");
    return p;
}

} // namespace

AETHER_TEST(BlueprintArrays_VariablesChangeInPlace) {
    ArrayGraphs g;
    g.AddVariable("scores", "Array<int>");
    g.AddVariable("names", "Array<string>");
    GraphBuilder b = g.Events();
    // Fill: scores += 5, 7, 5 (AddUnique skips the second 5); names += "ann", "bob".
    const NodeId fill = b.Add("Event.Custom", {{"name", "Fill"}});
    NodeId last = fill;
    std::string pin = "then";
    auto step = [&](const std::string& type, const std::string& var, json item, const char* extra_pin = nullptr,
                    json extra = nullptr) {
        const NodeId get = b.Add("Var.Get:" + var), node = b.Add(type);
        b.Connect(get, "value", node, "array").Connect(last, pin, node, "exec");
        if (!item.is_null()) b.Default(node, "item", item);
        if (extra_pin != nullptr) b.Default(node, extra_pin, extra);
        last = node;
        pin = "then";
        return node;
    };
    step("Array.Add:int", "scores", 5);
    const NodeId add7 = step("Array.Add:int", "scores", 7);
    const NodeId unique = step("Array.AddUnique:int", "scores", 5);
    step("Array.Add:string", "names", "ann");
    step("Array.Add:string", "names", "bob");
    last = PrintValue(b, last, "then", add7, "index");     // 1
    last = PrintValue(b, last, "then", unique, "index");   // -1: already there
    // Query: length, get, find, contains, valid index, last index.
    const NodeId len = b.Add("Array.Length:int"), get1 = b.Add("Array.Get:int"), find = b.Add("Array.Find:string");
    const NodeId has = b.Add("Array.Contains:string"), valid = b.Add("Array.IsValidIndex:int"), lasti = b.Add("Array.LastIndex:int");
    for (NodeId n : {len, get1, valid, lasti}) b.Connect(b.Add("Var.Get:scores"), "value", n, "array");
    for (NodeId n : {find, has}) b.Connect(b.Add("Var.Get:names"), "value", n, "array");
    b.Default(get1, "index", 1).Default(find, "item", "bob").Default(has, "item", "cy").Default(valid, "index", 2);
    last = PrintValue(b, last, "then", len, "result");
    last = PrintValue(b, last, "then", get1, "item");
    last = PrintValue(b, last, "then", find, "index");
    last = PrintValue(b, last, "then", has, "result");
    last = PrintValue(b, last, "then", valid, "result");
    last = PrintValue(b, last, "then", lasti, "result");
    // Edit: insert 9 at 0, set [2] = 8, remove item 5, remove index 5 (out of range: nothing), reverse names.
    const NodeId edit = b.Add("Event.Custom", {{"name", "Edit"}});
    last = edit;
    step("Array.Insert:int", "scores", 9, "index", 0);
    const NodeId set = step("Array.Set:int", "scores", 8, "index", 2);
    (void)set;
    const NodeId removed = step("Array.RemoveItem:int", "scores", 5);
    step("Array.RemoveIndex:int", "scores", nullptr, "index", 5);
    step("Array.Reverse:string", "names", nullptr);
    PrintValue(b, last, "then", removed, "removed");
    // Wipe: clear names.
    const NodeId wipe = b.Add("Event.Custom", {{"name", "Wipe"}});
    last = wipe;
    step("Array.Clear:string", "names", nullptr);
    AETHER_CHECK(g.Start());

    AETHER_CHECK(g.Fire("Event.Custom:Fill") == (Lines{"1", "-1", "2", "7", "1", "false", "false", "1"}));
    AETHER_CHECK(Ints(g.vm->GetArray(g.entity, "scores"), {5, 7}));
    AETHER_CHECK(g.Fire("Event.Custom:Edit") == Lines{"true"});
    // [9, 5, 7] -> set [2] = 8 -> [9, 5, 8] -> remove 5 -> [9, 8]
    AETHER_CHECK(Ints(g.vm->GetArray(g.entity, "scores"), {9, 8}));
    AETHER_CHECK(Texts(g.vm->GetArray(g.entity, "names"), {"bob", "ann"}));
    g.Fire("Event.Custom:Wipe");
    AETHER_CHECK(g.vm->GetArray(g.entity, "names").empty());
    AETHER_CHECK(g.vm->Errors().empty());

    // From C++.
    AETHER_CHECK(g.vm->SetArray(g.entity, "names", {std::string("x"), std::string("y")}));
    AETHER_CHECK(!g.vm->SetArray(g.entity, "names", {i32{1}}) && !g.vm->SetArray(g.entity, "nope", {}));
    AETHER_CHECK(std::holds_alternative<std::monostate>(g.vm->GetVariable(g.entity, "scores")));
}

AETHER_TEST(BlueprintArrays_MakeForEachAndOutOfRange) {
    ArrayGraphs g;
    g.AddVariable("items", "Array<float>");
    g.AddVariable("total", "float");
    GraphBuilder b = g.Events();
    const NodeId begin = b.Add("Event.BeginPlay");
    // items = Make[1.5, 2.5, 3]; for each: total += element, print "index:element".
    const NodeId make = b.Add("Array.Make:float", {{"count", 3}}), set = b.Add("Var.Set:items");
    b.Default(make, "item 0", 1.5).Default(make, "item 1", 2.5).Default(make, "item 2", 3.0);
    b.Connect(begin, "then", set, "exec").Connect(make, "array", set, "value");
    const NodeId each = b.Add("Flow.ForEach:float"), get = b.Add("Var.Get:items");
    b.Connect(set, "then", each, "exec").Connect(get, "value", each, "array");
    const NodeId fmt = b.Add("Text.Format", {{"format", "{i}:{e}"}});
    b.Connect(each, "index", fmt, "i").Connect(each, "element", fmt, "e");
    const NodeId p = PrintValue(b, each, "loop_body", fmt, "result");
    // The body also appends to the array: For Each iterates a copy, so it still runs 3 times.
    const NodeId grow = b.Add("Array.Add:float"), get2 = b.Add("Var.Get:items");
    b.Connect(p, "then", grow, "exec").Connect(get2, "value", grow, "array");
    const NodeId sum_get = b.Add("Var.Get:total"), add = b.Add("Math.Add:float"), sum_set = b.Add("Var.Set:total");
    b.Connect(grow, "then", sum_set, "exec").Connect(sum_get, "value", add, "a").Connect(each, "element", add, "b");
    b.Connect(add, "result", sum_set, "value");
    // Completed: an out-of-range Get gives 0 and BP205 (once), then the length.
    const NodeId oob = b.Add("Array.Get:float"), get3 = b.Add("Var.Get:items");
    b.Default(oob, "index", 99);
    b.Connect(get3, "value", oob, "array");
    const NodeId p2 = PrintValue(b, each, "completed", oob, "item");
    const NodeId len = b.Add("Array.Length:float"), get4 = b.Add("Var.Get:items");
    b.Connect(get4, "value", len, "array");
    PrintValue(b, p2, "then", len, "result");
    AETHER_CHECK(g.Start());

    AETHER_CHECK(g.Fire("Event.BeginPlay") == (Lines{"0:1.5", "1:2.5", "2:3", "0", "6"}));
    AETHER_CHECK(std::get<f32>(g.vm->GetVariable(g.entity, "total")) == 7.0f);
    AETHER_CHECK(g.vm->Errors().size() == 1 && g.vm->Errors()[0].code == "BP205" && g.vm->Errors()[0].node == oob);

    // Entity arrays default to "no entity" out of range.
    ArrayGraphs e;
    e.AddVariable("targets", "Array<Entity>");
    GraphBuilder eb = e.Events();
    const NodeId start = eb.Add("Event.BeginPlay"), eget = eb.Add("Array.Get:Entity"), valid = eb.Add("Entity.IsValid");
    eb.Connect(eb.Add("Var.Get:targets"), "value", eget, "array").Connect(eget, "item", valid, "entity");
    PrintValue(eb, start, "then", valid, "result");
    AETHER_CHECK(e.Start());
    const Entity other = e.world.CreateEntity();
    (void)other;
    AETHER_CHECK(e.Fire("Event.BeginPlay") == Lines{"false"});
}

AETHER_TEST(BlueprintArrays_RefPinsMustBeVariables) {
    // BP013: an Add fed by Make Array (not a variable) is refused on the node.
    ArrayGraphs g;
    GraphBuilder b = g.Events();
    const NodeId begin = b.Add("Event.BeginPlay"), make = b.Add("Array.Make:int"), add = b.Add("Array.Add:int");
    b.Connect(begin, "then", add, "exec").Connect(make, "array", add, "array");
    const CompileResult r = CompileBlueprint(g.bp);
    AETHER_CHECK(!r.Ok() && r.diagnostics.Has("BP013"));
    const std::vector<const Diagnostic*> on_add = r.diagnostics.For("EventGraph", add);
    AETHER_CHECK(on_add.size() == 1 && on_add[0]->pin == "array");

    ArrayGraphs unconnected;
    GraphBuilder u = unconnected.Events();
    const NodeId clear = u.Add("Array.Clear:int");
    u.Connect(u.Add("Event.BeginPlay"), "then", clear, "exec");
    AETHER_CHECK(CompileBlueprint(unconnected.bp).diagnostics.Has("BP013"));

    // Element types arrays can't hold, and too many Make items.
    ArrayGraphs bad;
    bad.Events().Add("Array.Length:Transform");
    bad.Events().Add("Array.Make:int", {{"count", 17}});
    const CompileResult rb = CompileBlueprint(bad.bp);
    AETHER_CHECK(rb.diagnostics.errors == 2 && rb.diagnostics.Has("BP007"));

    // A type mismatch between array types is BP001.
    ArrayGraphs mismatch;
    mismatch.AddVariable("floats", "Array<float>");
    GraphBuilder m = mismatch.Events();
    const NodeId len = m.Add("Array.Length:int");
    m.Connect(m.Add("Var.Get:floats"), "value", len, "array");
    AETHER_CHECK(CompileBlueprint(mismatch.bp).diagnostics.Has("BP001"));
}
