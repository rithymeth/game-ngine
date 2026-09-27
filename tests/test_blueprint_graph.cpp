#include "aether/blueprint/validate.h"
#include "aether/scene/components.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>

using namespace aether;
using namespace aether::bp;
using nlohmann::json;

struct BpTestHealth {
    f32 current = 100.0f;
    i32 max = 100;
    f32 armor = 0.0f; // not BlueprintReadWrite
    f32 Heal(f32 amount) { return current += amount; }
    bool IsFull() const { return current >= static_cast<f32>(max); }
    void Internal() {}
};

AETHER_REFLECT(BpTestHealth, 1,
    AETHER_FIELD(current, Field_EditAnywhere | Field_BlueprintReadWrite),
    AETHER_FIELD(max, Field_EditAnywhere | Field_BlueprintReadWrite),
    AETHER_FIELD(armor),
    AETHER_METHOD(Heal, Fn_BlueprintCallable, {"amount"}),
    AETHER_METHOD(IsFull, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(Internal)
)

namespace {

// BP_Door from docs/ROADMAP_DETAILS.md §A.4, with its Open function.
const char* kDoor = R"({
  "$type": "Blueprint",
  "$version": 1,
  "parent": "native:Entity",
  "variables": [
    { "name": "IsOpen",     "type": "bool",  "default": false, "flags": ["InstanceEditable"] },
    { "name": "open_speed", "type": "float", "default": 1.0,   "flags": ["InstanceEditable"], "tooltip": "Degrees per second" }
  ],
  "graphs": [
    {
      "name": "EventGraph",
      "kind": "EventGraph",
      "nodes": [
        { "id": 1, "type": "Event.OnTriggerEnter", "pos": [0, 0] },
        { "id": 2, "type": "Flow.Branch",          "pos": [260, 0] },
        { "id": 3, "type": "Var.Get:IsOpen",       "pos": [120, 90] },
        { "id": 4, "type": "Call.Self:Open",       "pos": [520, 0] }
      ],
      "links": [
        { "from": [1, "then"],  "to": [2, "exec"] },
        { "from": [3, "value"], "to": [2, "condition"] },
        { "from": [2, "false"], "to": [4, "exec"] }
      ]
    },
    {
      "name": "Open",
      "kind": "Function",
      "inputs": [],
      "outputs": [],
      "nodes": [
        { "id": 1, "type": "Function.Entry", "pos": [0, 0] },
        { "id": 2, "type": "Var.Set:IsOpen", "pos": [200, 0], "defaults": { "value": true } }
      ],
      "links": [ { "from": [1, "then"], "to": [2, "exec"] } ]
    }
  ]
})";

Blueprint Door() {
    Blueprint bp;
    std::string error;
    AETHER_CHECK(BlueprintFromJson(json::parse(kDoor), bp, &error));
    return bp;
}

std::vector<std::string> PinNames(const NodeSignature& s, PinDir dir) {
    std::vector<std::string> names;
    for (const PinDesc& p : s.pins) {
        if (p.dir == dir) names.push_back(p.name);
    }
    return names;
}

NodeSignature Resolve(const Blueprint& bp, const std::string& type, json config = json::object(),
                      const std::string& graph = "EventGraph") {
    Node node;
    node.id = 99;
    node.type = type;
    node.config = std::move(config);
    NodeError error;
    std::optional<NodeSignature> s = ResolveNode(bp, *bp.FindGraph(graph), node, &error);
    AETHER_CHECK(s.has_value());
    return s ? *s : NodeSignature{};
}

// The diagnostic with `code`, or null.
const Diagnostic* Find(const ValidationResult& r, const std::string& code) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.code == code) return &d;
    }
    return nullptr;
}

} // namespace

AETHER_TEST(Blueprint_TypesParseConvertAndConnect) {
    for (const char* name : {"exec", "bool", "int", "float", "string", "Vec3", "Quat", "Entity", "Wildcard",
                             "Array<int>", "Array<Vec3>", "Transform", "Array<Transform>"}) {
        std::optional<PinType> type = ParseType(name);
        AETHER_CHECK(type.has_value() && TypeName(*type) == name);
    }
    AETHER_CHECK(!ParseType("Array<Array<int>>") && !ParseType("Array<exec>") && !ParseType("nope"));
    AETHER_CHECK(*PinTypeOf(reflect::Reflect<f32>()) == PinType::Of(ValueType::Float));
    AETHER_CHECK(*PinTypeOf(reflect::Reflect<std::vector<i32>>()) == PinType::ArrayOf(PinType::Of(ValueType::Int)));
    AETHER_CHECK(PinTypeOf(reflect::Reflect<Transform>())->type == ValueType::Struct);

    const PinType i = PinType::Of(ValueType::Int), f = PinType::Of(ValueType::Float), s = PinType::Of(ValueType::String);
    const PinType v = PinType::Of(ValueType::Vec3), e = PinType::Of(ValueType::Entity), x = PinType::Exec();
    AETHER_CHECK(CanConnect(i, i) == Compat::Yes && CanConnect(x, x) == Compat::Yes);
    AETHER_CHECK(CanConnect(i, f) == Compat::Convert && CanConnect(f, i) == Compat::ConvertLossy);
    AETHER_CHECK(CanConnect(v, s) == Compat::Convert && CanConnect(e, s) == Compat::Convert);
    AETHER_CHECK(CanConnect(s, f) == Compat::No && CanConnect(x, i) == Compat::No && CanConnect(v, f) == Compat::No);
    AETHER_CHECK(CanConnect(PinType::ArrayOf(i), PinType::ArrayOf(f)) == Compat::No);
    AETHER_CHECK(CanConnect(v, PinType::Of(ValueType::Wildcard)) == Compat::Yes);

    Value value;
    AETHER_CHECK(ValueFromJson(json(3), f, value) && std::get<f32>(value) == 3.0f);
    AETHER_CHECK(ValueFromJson(json(2.9), i, value) && std::get<i32>(value) == 2);
    AETHER_CHECK(ValueFromJson(json::array({1, 2, 3}), v, value) && std::get<Vec3>(value).z == 3.0f);
    AETHER_CHECK(!ValueFromJson(json("x"), f, value) && !ValueFromJson(json::array({1, 2}), v, value));
    AETHER_CHECK(ValueToJson(Value{Vec3{1, 2, 3}}) == json::array({1.0f, 2.0f, 3.0f}));
    AETHER_CHECK(ValueText(Value{1.5f}) == "1.5" && ValueText(Value{std::string("hi")}) == "\"hi\"");
}

AETHER_TEST(Blueprint_DoorLoadsSavesAndValidates) {
    Blueprint bp = Door();
    AETHER_CHECK(bp.variables.size() == 2 && bp.variables[0].flags == Var_InstanceEditable);
    AETHER_CHECK(std::get<f32>(bp.variables[1].default_value) == 1.0f && bp.variables[1].tooltip == "Degrees per second");
    AETHER_CHECK(bp.graphs.size() == 2 && bp.graphs[1].kind == GraphKind::Function);
    AETHER_CHECK(bp.graphs[0].links[1] == (Link{{3, "value"}, {2, "condition"}}));

    // Round trip: saving what was loaded gives the same document.
    const json saved = BlueprintToJson(bp);
    Blueprint again;
    AETHER_CHECK(BlueprintFromJson(saved, again) && BlueprintToJson(again) == saved);
    const auto path = std::filesystem::temp_directory_path() / "aether_bp_door.abp";
    AETHER_CHECK(SaveBlueprint(bp, path));
    Blueprint from_disk;
    AETHER_CHECK(LoadBlueprint(path, from_disk) && BlueprintToJson(from_disk) == saved);
    std::filesystem::remove(path);

    const ValidationResult r = ValidateBlueprint(bp);
    AETHER_CHECK(r.Ok() && r.warnings == 0);

    // Pins come from the node types.
    const NodeSignature branch = Resolve(bp, "Flow.Branch");
    AETHER_CHECK(PinNames(branch, PinDir::In) == (std::vector<std::string>{"exec", "condition"}));
    AETHER_CHECK(PinNames(branch, PinDir::Out) == (std::vector<std::string>{"true", "false"}));
    const NodeSignature get = Resolve(bp, "Var.Get:IsOpen");
    AETHER_CHECK(get.IsPure() && get.pins.size() == 1 && get.pins[0].type == PinType::Of(ValueType::Bool));
    const NodeSignature set = Resolve(bp, "Var.Set:open_speed");
    AETHER_CHECK(std::get<f32>(set.Find("value", PinDir::In)->default_value) == 1.0f);
    const NodeSignature open = Resolve(bp, "Call.Self:Open");
    AETHER_CHECK(open.kind == NodeKind::Impure && open.pins.size() == 2);
    const NodeSignature trigger = Resolve(bp, "Event.OnTriggerEnter");
    AETHER_CHECK(trigger.IsEvent() && trigger.Find("other", PinDir::Out)->type == PinType::Of(ValueType::Entity));

    // Load errors.
    Blueprint bad;
    std::string error;
    AETHER_CHECK(!BlueprintFromJson(json::parse(R"({"$type": "Prefab"})"), bad, &error));
    AETHER_CHECK(!BlueprintFromJson(json::parse(R"({"$type": "Blueprint", "$version": 99})"), bad, &error) &&
                 error.find("newer") != std::string::npos);
    AETHER_CHECK(!BlueprintFromJson(json::parse(R"({"$type": "Blueprint", "variables": [{"name": "a", "type": "Banana"}]})"),
                                    bad, &error) && error.find("Banana") != std::string::npos);
    AETHER_CHECK(!BlueprintFromJson(json::parse(R"({"$type": "Blueprint", "graphs": [{"name": "G", "nodes":
        [{"id": 1, "type": "Flow.Branch"}, {"id": 1, "type": "Flow.Branch"}]}]})"), bad, &error) &&
                 error.find("two nodes") != std::string::npos);
    AETHER_CHECK(!BlueprintFromJson(json::parse(R"({"$type": "Blueprint", "graphs": [{"name": "G",
        "links": [{"from": [1], "to": [2, "exec"]}]}]})"), bad, &error));
}

AETHER_TEST(Blueprint_SignaturesFromConfigAndReflection) {
    (void)GetComponentId<BpTestHealth>();
    Blueprint bp = Door();
    Graph& events = *bp.FindGraph("EventGraph");
    GraphBuilder(events).Add("Event.Custom", {{"name", "Hit"}, {"params", {{{"name", "amount"}, {"type", "float"}}}}});

    const NodeSignature seq = Resolve(bp, "Flow.Sequence", {{"count", 3}});
    AETHER_CHECK(PinNames(seq, PinDir::Out) == (std::vector<std::string>{"then 0", "then 1", "then 2"}));
    AETHER_CHECK(PinNames(Resolve(bp, "Flow.Sequence"), PinDir::Out).size() == 2);

    // Custom events and their calls share parameters.
    const NodeSignature custom = Resolve(bp, "Event.Custom", {{"name", "Hit"}, {"params", {{{"name", "amount"}, {"type", "float"}}}}});
    AETHER_CHECK(custom.event_key == "Event.Custom:Hit" && custom.Find("amount", PinDir::Out) != nullptr);
    const NodeSignature call = Resolve(bp, "Call.Custom:Hit");
    AETHER_CHECK(PinNames(call, PinDir::In) == (std::vector<std::string>{"exec", "amount"}));

    // Math families by operand type.
    const NodeSignature add = Resolve(bp, "Math.Add:Vec3");
    AETHER_CHECK(add.IsPure() && add.Find("result", PinDir::Out)->type == PinType::Of(ValueType::Vec3));
    AETHER_CHECK(Resolve(bp, "Math.Less:float").Find("result", PinDir::Out)->type == PinType::Of(ValueType::Bool));
    Node bad_add;
    bad_add.type = "Math.Divide:Vec3";
    NodeError error;
    AETHER_CHECK(!ResolveNode(bp, events, bad_add, &error) && error.code == "BP007");
    AETHER_CHECK(std::get<f32>(Resolve(bp, "Literal.Float", {{"value", 2.5}}).pins[0].default_value) == 2.5f);

    // Reflected functions: BlueprintCallable only; pure ones have no exec pins.
    const NodeSignature heal = Resolve(bp, "Call.Native:BpTestHealth.Heal");
    AETHER_CHECK(PinNames(heal, PinDir::In) == (std::vector<std::string>{"exec", "target", "amount"}));
    AETHER_CHECK(PinNames(heal, PinDir::Out) == (std::vector<std::string>{"then", "return"}));
    AETHER_CHECK(heal.function != nullptr && heal.owner == &reflect::Reflect<BpTestHealth>());
    AETHER_CHECK((heal.Find("target", PinDir::In)->flags & Pin_Self) != 0);
    const NodeSignature full = Resolve(bp, "Call.Native:BpTestHealth.IsFull");
    AETHER_CHECK(full.IsPure() && PinNames(full, PinDir::In) == std::vector<std::string>{"target"});
    Node internal;
    internal.type = "Call.Native:BpTestHealth.Internal";
    AETHER_CHECK(!ResolveNode(bp, events, internal, &error) && error.code == "BP004");
    AETHER_CHECK(error.message.find("CoreRedirects") != std::string::npos);

    // BlueprintReadWrite fields get Get/Set nodes.
    const NodeSignature current = Resolve(bp, "Comp.Get:BpTestHealth.current");
    AETHER_CHECK(current.IsPure() && current.Find("value", PinDir::Out)->type == PinType::Of(ValueType::Float));
    AETHER_CHECK(Resolve(bp, "Comp.Set:BpTestHealth.max").Find("value", PinDir::In)->type == PinType::Of(ValueType::Int));
    Node armor;
    armor.type = "Comp.Get:BpTestHealth.armor";
    AETHER_CHECK(!ResolveNode(bp, events, armor, &error) && error.code == "BP004");

    // Function graphs: entry/return pins from the signature; pure calls.
    Graph fn;
    fn.name = "Damage";
    fn.kind = GraphKind::Function;
    fn.pure = true;
    Variable amount, left;
    amount.name = "amount";
    left.name = "left";
    fn.inputs = {amount};
    fn.outputs = {left};
    bp.graphs.push_back(fn);
    const NodeSignature damage = Resolve(bp, "Call.Self:Damage");
    AETHER_CHECK(damage.IsPure() && PinNames(damage, PinDir::In) == std::vector<std::string>{"amount"});
    AETHER_CHECK(PinNames(Resolve(bp, "Function.Entry", {}, "Damage"), PinDir::Out) ==
                 (std::vector<std::string>{"then", "amount"}));
    AETHER_CHECK(PinNames(Resolve(bp, "Function.Return", {}, "Damage"), PinDir::In) == std::vector<std::string>{"left"});

    // Literal pin defaults (regression: a float default was once taken for pin flags).
    AETHER_CHECK(std::get<f32>(Resolve(bp, "Latent.Delay").Find("duration", PinDir::In)->default_value) == 0.2f);
    AETHER_CHECK(std::get<f32>(Resolve(bp, "Math.Clamp:float").Find("max", PinDir::In)->default_value) == 1.0f);
    AETHER_CHECK(std::get<i32>(Resolve(bp, "Flow.DoN").Find("n", PinDir::In)->default_value) == 1);
    AETHER_CHECK(Resolve(bp, "Flow.DoN").Find("n", PinDir::In)->flags == Pin_None);
    AETHER_CHECK((Resolve(bp, "Flow.Branch").Find("condition", PinDir::In)->flags & Pin_WarnIfUnconnected) != 0);

    // Custom node types can be registered.
    RegisterNodeType("Test.Ping", [](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        NodeSignature s;
        s.title = "Ping";
        s.category = "Test";
        s.pins = {{"exec", PinDir::In, PinType::Exec(), {}, 0}, {"then", PinDir::Out, PinType::Exec(), {}, 0}};
        return s;
    });
    AETHER_CHECK(Resolve(bp, "Test.Ping").title == "Ping");

    // The palette lists everything this Blueprint can use, sorted.
    const std::vector<PaletteEntry> palette = ListNodeTypes(bp);
    auto has = [&](const std::string& id) {
        return std::any_of(palette.begin(), palette.end(), [&](const PaletteEntry& e) { return e.id == id; });
    };
    AETHER_CHECK(has("Flow.Branch") && has("Var.Get:IsOpen") && has("Var.Set:open_speed") && has("Call.Self:Open"));
    AETHER_CHECK(has("Call.Custom:Hit") && has("Call.Native:BpTestHealth.Heal") && has("Comp.Get:BpTestHealth.current"));
    AETHER_CHECK(has("Math.Add:float") && has("Event.Custom") && has("Test.Ping"));
    AETHER_CHECK(!has("Call.Native:BpTestHealth.Internal") && !has("Comp.Get:BpTestHealth.armor"));
    AETHER_CHECK(std::is_sorted(palette.begin(), palette.end(), [](const PaletteEntry& a, const PaletteEntry& b) {
        return a.category != b.category ? a.category < b.category : a.title < b.title;
    }));
}

AETHER_TEST(Blueprint_ValidationReportsEachProblemOnItsNode) {
    // Each case: a small Event Graph with one problem.
    auto check = [](const std::function<void(Blueprint&, GraphBuilder&)>& build, const std::string& code,
                    NodeId node, const std::string& pin = "", Severity severity = Severity::Error) {
        Blueprint bp = Door();
        Graph g;
        g.name = "Test";
        bp.graphs.push_back(g);
        GraphBuilder b(*bp.FindGraph("Test"));
        build(bp, b);
        const ValidationResult r = ValidateBlueprint(bp);
        const Diagnostic* d = Find(r, code);
        AETHER_CHECK(d != nullptr);
        if (d == nullptr) {
            for (const Diagnostic& x : r.diagnostics) std::printf("    got %s: %s\n", x.code.c_str(), x.message.c_str());
            return;
        }
        AETHER_CHECK(d->graph == "Test" && d->node == node && d->severity == severity);
        if (!pin.empty()) AETHER_CHECK(d->pin == pin);
    };

    // BP001: a Vec3 into a bool.
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId v = b.Add("Vec3.Make"), br = b.Add("Flow.Branch");
        b.Connect(v, "result", br, "condition");
    }, "BP001", 2, "condition");
    // BP001: exec into data.
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId e = b.Add("Event.BeginPlay"), p = b.Add("Debug.Print");
        b.Connect(e, "then", p, "text");
    }, "BP001", 2, "text");
    // BP003: pure nodes in a loop.
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId a = b.Add("Math.Add:float"), m = b.Add("Math.Multiply:float");
        b.Connect(a, "result", m, "a").Connect(m, "result", a, "a");
    }, "BP003", 1);
    // BP004: a function that's gone.
    check([](Blueprint&, GraphBuilder& b) { b.Add("Call.Self:Close"); }, "BP004", 1);
    // BP005: a deleted variable.
    check([](Blueprint&, GraphBuilder& b) { b.Add("Var.Get:Speed"); }, "BP005", 1);
    // BP006: a second BeginPlay (and a second custom event of one name).
    check([](Blueprint&, GraphBuilder& b) { b.Add("Event.BeginPlay"); b.Add("Event.BeginPlay"); }, "BP006", 2);
    check([](Blueprint&, GraphBuilder& b) {
        b.Add("Event.Custom", {{"name", "Go"}});
        b.Add("Event.Custom", {{"name", "Stop"}});
        b.Add("Event.Custom", {{"name", "Go"}});
    }, "BP006", 3);
    // BP007: unknown type; bad config.
    check([](Blueprint&, GraphBuilder& b) { b.Add("Flow.Teleport"); }, "BP007", 1);
    check([](Blueprint&, GraphBuilder& b) { b.Add("Flow.Sequence", {{"count", 0}}); }, "BP007", 1);
    // BP008: missing pin, output used as input, default for a non-input, wrong default type, missing node.
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId e = b.Add("Event.BeginPlay"), br = b.Add("Flow.Branch");
        b.Connect(e, "then", br, "maybe");
    }, "BP008", 2, "maybe");
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId e = b.Add("Event.BeginPlay"), br = b.Add("Flow.Branch");
        b.Connect(br, "exec", e, "then");
    }, "BP008", 2, "exec");
    check([](Blueprint&, GraphBuilder& b) { b.Default(b.Add("Debug.Print"), "color", "red"); }, "BP008", 1, "color");
    check([](Blueprint&, GraphBuilder& b) { b.Default(b.Add("Debug.Print"), "text", 12); }, "BP008", 1, "text");
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId e = b.Add("Event.BeginPlay");
        b.Connect(e, "then", 42, "exec");
    }, "BP008", 1);
    // BP009: two links into a data input; one exec output to two nodes.
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId x = b.Add("Var.Get:IsOpen"), y = b.Add("Var.Get:IsOpen"), br = b.Add("Flow.Branch");
        b.Connect(x, "value", br, "condition").Connect(y, "value", br, "condition");
    }, "BP009", 3, "condition");
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId e = b.Add("Event.BeginPlay"), p = b.Add("Debug.Print"), q = b.Add("Debug.Print");
        b.Connect(e, "then", p, "exec").Connect(e, "then", q, "exec");
    }, "BP009", 1, "then");
    // BP101: Branch's condition left unconnected (a warning, with the default).
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId e = b.Add("Event.BeginPlay"), br = b.Add("Flow.Branch");
        b.Connect(e, "then", br, "exec");
    }, "BP101", 2, "condition", Severity::Warning);
    // BP104: float into int.
    check([](Blueprint&, GraphBuilder& b) {
        const NodeId f = b.Add("Literal.Float"), add = b.Add("Math.Add:int");
        b.Connect(f, "value", add, "a");
    }, "BP104", 2, "a", Severity::Warning);
}

AETHER_TEST(Blueprint_FunctionGraphRules) {
    Blueprint bp = Door();
    GraphBuilder(*bp.FindGraph("Open")).Add("Event.Tick");
    ValidationResult r = ValidateBlueprint(bp);
    const Diagnostic* d = Find(r, "BP010");
    AETHER_CHECK(d != nullptr && d->graph == "Open" && d->node == 3);

    Blueprint two = Door();
    GraphBuilder(*two.FindGraph("Open")).Add("Function.Entry");
    r = ValidateBlueprint(two);
    d = Find(r, "BP011");
    AETHER_CHECK(d != nullptr && d->graph == "Open" && d->node == 0 && d->message.find("has 2") != std::string::npos);

    // Function nodes outside a function graph.
    Blueprint stray = Door();
    GraphBuilder(*stray.FindGraph("EventGraph")).Add("Function.Return");
    r = ValidateBlueprint(stray);
    AETHER_CHECK(Find(r, "BP010") != nullptr && Find(r, "BP010")->graph == "EventGraph");

    // Implicit conversions are fine: an int into a float, anything into Print's text.
    Blueprint ok = Door();
    GraphBuilder b(*ok.FindGraph("EventGraph"));
    const NodeId begin = b.Add("Event.BeginPlay"), i = b.Add("Literal.Int", {{"value", 3}});
    const NodeId add = b.Add("Math.Add:float"), print = b.Add("Debug.Print");
    b.Connect(i, "value", add, "a").Connect(add, "result", print, "text").Connect(begin, "then", print, "exec");
    r = ValidateBlueprint(ok);
    AETHER_CHECK(r.Ok() && r.warnings == 0);
    AETHER_CHECK(r.For("EventGraph", print).empty());
}
