#include "aether/assets/asset_guid.h"
#include "aether/blueprint/compiler.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <functional>

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

Blueprint WithEvents() {
    Blueprint bp;
    Graph g;
    g.name = "EventGraph";
    bp.graphs.push_back(g);
    return bp;
}

std::shared_ptr<const CompiledBlueprint> Compile(const Blueprint& bp) {
    CompileResult r = CompileBlueprint(bp);
    for (const Diagnostic& d : r.diagnostics.diagnostics) {
        if (d.severity == Severity::Error) std::printf("    %s node %u: %s\n", d.code.c_str(), d.node, d.message.c_str());
    }
    AETHER_CHECK(r.Ok());
    return r.blueprint;
}

// A pure function taking `inputs` and returning bool "result" from `build`.
void AddPredicate(Blueprint& bp, const std::string& name, std::vector<Variable> inputs,
                  const std::function<void(GraphBuilder&, NodeId entry, NodeId ret)>& build) {
    Graph fn;
    fn.name = name;
    fn.kind = GraphKind::Function;
    fn.pure = true;
    fn.inputs = std::move(inputs);
    fn.outputs = {Var("result", "bool")};
    GraphBuilder g(fn);
    const NodeId entry = g.Add("Function.Entry"), ret = g.Add("Function.Return");
    build(g, entry, ret);
    bp.graphs.push_back(fn);
}

std::vector<i32> Ints(const std::vector<VmValue>& items) {
    std::vector<i32> out;
    for (const VmValue& v : items) out.push_back(std::holds_alternative<i32>(v) ? std::get<i32>(v) : -999);
    return out;
}

} // namespace

AETHER_TEST(BlueprintSort_NaturalCustomAndFilter) {
    Blueprint bp = WithEvents();
    bp.variables = {Var("numbers", "Array<int>"), Var("words", "Array<string>"), Var("evens", "Array<int>")};
    // Descending(a, b) = a > b; IsEven(x) = x % 2 == 0.
    AddPredicate(bp, "Descending", {Var("a", "int"), Var("b", "int")}, [](GraphBuilder& g, NodeId entry, NodeId ret) {
        const NodeId gt = g.Add("Math.Greater:int");
        g.Connect(entry, "a", gt, "a").Connect(entry, "b", gt, "b").Connect(gt, "result", ret, "result");
    });
    AddPredicate(bp, "IsEven", {Var("x", "int")}, [](GraphBuilder& g, NodeId entry, NodeId ret) {
        const NodeId mod = g.Add("Math.Modulo:int"), eq = g.Add("Math.Equal:int");
        g.Default(mod, "b", 2);
        g.Connect(entry, "x", mod, "a").Connect(mod, "result", eq, "a").Connect(eq, "result", ret, "result");
    });
    GraphBuilder b(*bp.FindGraph("EventGraph"));
    auto sort_event = [&](const char* event, const char* var, const char* type, json config) {
        const NodeId e = b.Add("Event.Custom", {{"name", event}});
        const NodeId sort = b.Add(std::string("Array.Sort:") + type, config);
        b.Connect(e, "then", sort, "exec").Connect(b.Add(std::string("Var.Get:") + var), "value", sort, "array");
    };
    sort_event("SortUp", "numbers", "int", json::object());
    sort_event("SortDown", "numbers", "int", {{"by", "Descending"}});
    sort_event("SortWords", "words", "string", json::object());
    // Filter: evens = Filter(numbers, IsEven).
    const NodeId pick = b.Add("Event.Custom", {{"name", "PickEvens"}}), filter = b.Add("Array.Filter:int", {{"by", "IsEven"}});
    const NodeId set = b.Add("Var.Set:evens");
    b.Connect(pick, "then", set, "exec").Connect(b.Add("Var.Get:numbers"), "value", filter, "array");
    b.Connect(filter, "result", set, "value");

    World world;
    BlueprintVM vm(world);
    const Entity e = world.CreateEntity();
    AETHER_CHECK(vm.Attach(e, Compile(bp)));
    AETHER_CHECK(vm.SetArray(e, "numbers", {i32{5}, i32{-2}, i32{9}, i32{4}, i32{4}, i32{0}}));
    AETHER_CHECK(vm.SetArray(e, "words", {std::string("pear"), std::string("apple"), std::string("fig")}));

    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:SortUp"));
    AETHER_CHECK(Ints(vm.GetArray(e, "numbers")) == (std::vector<i32>{-2, 0, 4, 4, 5, 9}));
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:SortDown"));
    AETHER_CHECK(Ints(vm.GetArray(e, "numbers")) == (std::vector<i32>{9, 5, 4, 4, 0, -2}));
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:SortWords"));
    const std::vector<VmValue> words = vm.GetArray(e, "words");
    AETHER_CHECK(words.size() == 3 && std::get<std::string>(words[0]) == "apple" && std::get<std::string>(words[2]) == "pear");
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:PickEvens"));
    AETHER_CHECK(Ints(vm.GetArray(e, "evens")) == (std::vector<i32>{4, 4, 0, -2}));
    AETHER_CHECK(vm.Errors().empty());

    // A larger sort agrees with std::sort (the merge sort is stable and complete).
    std::vector<VmValue> many;
    std::vector<i32> expected;
    for (i32 i = 0; i < 100; ++i) {
        const i32 v = (i * 37) % 101 - 50;
        many.push_back(v);
        expected.push_back(v);
    }
    std::sort(expected.begin(), expected.end(), std::greater<i32>());
    AETHER_CHECK(vm.SetArray(e, "numbers", many));
    AETHER_CHECK(vm.Dispatch(e, "Event.Custom:SortDown"));
    AETHER_CHECK(Ints(vm.GetArray(e, "numbers")) == expected);
}

AETHER_TEST(BlueprintSort_Errors) {
    Blueprint bp = WithEvents();
    bp.variables = {Var("things", "Array<Vec3>"), Var("numbers", "Array<int>")};
    AddPredicate(bp, "Wrong", {Var("a", "float")}, [](GraphBuilder&, NodeId, NodeId) {});
    GraphBuilder b(*bp.FindGraph("EventGraph"));
    const NodeId begin = b.Add("Event.BeginPlay");
    // Vec3 has no natural order: BP007. A function with the wrong signature: BP017. A missing one: BP004.
    const NodeId s1 = b.Add("Array.Sort:Vec3"), s2 = b.Add("Array.Sort:int", {{"by", "Wrong"}});
    const NodeId s3 = b.Add("Array.Sort:int", {{"by", "Missing"}}), f = b.Add("Array.Filter:int");
    b.Connect(begin, "then", s1, "exec").Connect(b.Add("Var.Get:things"), "value", s1, "array");
    for (NodeId n : {s2, s3}) b.Connect(b.Add("Var.Get:numbers"), "value", n, "array");
    b.Connect(b.Add("Var.Get:numbers"), "value", f, "array");
    const CompileResult r = CompileBlueprint(bp);
    AETHER_CHECK(!r.Ok());
    auto code_on = [&](NodeId node) {
        const std::vector<const Diagnostic*> d = r.diagnostics.For("EventGraph", node);
        return d.empty() ? std::string() : d[0]->code;
    };
    AETHER_CHECK(code_on(s1) == "BP007" && code_on(s2) == "BP017" && code_on(s3) == "BP004" && code_on(f) == "BP004");
}

AETHER_TEST(BlueprintSpawn_ExposedVariablesArriveBeforeBeginPlay) {
    // BP_Coin: exposed "value" (int) and "label" (string), printed at BeginPlay.
    Blueprint coin = WithEvents();
    Variable value = Var("value", "int"), label = Var("label", "string"), hidden = Var("hidden", "int");
    value.flags = Var_ExposeOnSpawn;
    label.flags = Var_ExposeOnSpawn;
    coin.variables = {value, label, hidden};
    {
        GraphBuilder b(*coin.FindGraph("EventGraph"));
        const NodeId begin = b.Add("Event.BeginPlay"), fmt = b.Add("Text.Format", {{"format", "{label}={value},{hidden}"}});
        b.Connect(b.Add("Var.Get:label"), "value", fmt, "label").Connect(b.Add("Var.Get:value"), "value", fmt, "value");
        b.Connect(b.Add("Var.Get:hidden"), "value", fmt, "hidden");
        const NodeId p = b.Add("Debug.Print");
        b.Connect(begin, "then", p, "exec").Connect(fmt, "result", p, "text");
    }
    const assets::AssetGuid coin_guid = assets::NewAssetGuid();
    // BP_Bank: spawns a coin worth 25, labelled "gold" (and tries to set "hidden", which isn't exposed).
    Blueprint bank = WithEvents();
    {
        GraphBuilder b(*bank.FindGraph("EventGraph"));
        const NodeId begin = b.Add("Event.BeginPlay");
        const NodeId spawn = b.Add("Entity.Spawn", {{"blueprint", assets::ToString(coin_guid)},
                                                    {"expose", {{{"name", "value"}, {"type", "int"}},
                                                                {{"name", "label"}, {"type", "string"}}}}});
        b.Default(spawn, "value", 25).Default(spawn, "label", "gold");
        b.Connect(begin, "then", spawn, "exec");
    }
    const auto coin_bp = Compile(coin);
    World world;
    BlueprintVM vm(world);
    std::vector<std::string> printed;
    vm.SetPrintHandler([&](Entity, const std::string& t) { printed.push_back(t); });
    json seen;
    vm.SetSpawnHandler([&](const assets::AssetGuid&, const Transform& t, const json& exposed) {
        seen = exposed;
        const Entity c = world.CreateEntity(t);
        json with_hidden = exposed;
        with_hidden["hidden"] = 99; // not expose-on-spawn: ignored
        vm.Attach(c, coin_bp, with_hidden);
        vm.Dispatch(c, "Event.BeginPlay");
        return c;
    });
    const Entity b = world.CreateEntity();
    AETHER_CHECK(vm.Attach(b, Compile(bank)));
    AETHER_CHECK(vm.Dispatch(b, "Event.BeginPlay"));
    AETHER_CHECK(seen == json({{"value", 25}, {"label", "gold"}}));
    AETHER_CHECK(printed == std::vector<std::string>{"gold=25,0"});

    // Bad exposed pins are refused.
    Blueprint bad = WithEvents();
    GraphBuilder(*bad.FindGraph("EventGraph"))
        .Add("Entity.Spawn", {{"blueprint", assets::ToString(coin_guid)}, {"expose", {{{"name", "location"}, {"type", "int"}}}}});
    AETHER_CHECK(CompileBlueprint(bad).diagnostics.Has("BP007"));
}
