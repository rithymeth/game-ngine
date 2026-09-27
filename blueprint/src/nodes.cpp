#include "aether/blueprint/nodes.h"

#include "aether/reflection/reflection.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

namespace aether::bp {

using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// Pin helpers
// ---------------------------------------------------------------------------

PinDesc ExecIn(const std::string& name = "exec") { return {name, PinDir::In, PinType::Exec(), {}, Pin_None}; }
PinDesc ExecOut(const std::string& name = "then") { return {name, PinDir::Out, PinType::Exec(), {}, Pin_None}; }
// The flags are a PinFlags, not an integer, so a literal default such as
// 0.2f can't be taken for flags by the overload below it.
PinDesc In(const std::string& name, PinType type, PinFlags flags = Pin_None) {
    return {name, PinDir::In, type, DefaultValue(type), flags};
}
PinDesc In(const std::string& name, PinType type, Value value, PinFlags flags = Pin_None) {
    return {name, PinDir::In, type, std::move(value), flags};
}
PinDesc Out(const std::string& name, PinType type) { return {name, PinDir::Out, type, {}, Pin_None}; }

PinType T(ValueType type) { return PinType::Of(type); }

NodeSignature Sig(std::string title, std::string category, NodeKind kind, std::vector<PinDesc> pins) {
    NodeSignature s;
    s.title = std::move(title);
    s.category = std::move(category);
    s.kind = kind;
    s.pins = std::move(pins);
    return s;
}

std::optional<NodeSignature> Fail(NodeError& error, const char* code, std::string message) {
    error.code = code;
    error.message = std::move(message);
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

struct Family {
    NodeFactory factory;
    FamilyLister lister;
};

struct Registry {
    std::map<std::string, NodeFactory> exact;
    std::map<std::string, Family> families; // by prefix
};

void RegisterBuiltins(Registry& r);

Registry& GetRegistry() {
    static Registry registry;
    static std::once_flag once;
    std::call_once(once, [] { RegisterBuiltins(registry); });
    return registry;
}

// ---------------------------------------------------------------------------
// Built-in node types
// ---------------------------------------------------------------------------

void AddEvent(Registry& r, const std::string& id, const std::string& title, std::vector<PinDesc> outputs) {
    r.exact[id] = [id, title, outputs](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        std::vector<PinDesc> pins{ExecOut()};
        pins.insert(pins.end(), outputs.begin(), outputs.end());
        NodeSignature s = Sig(title, "Events", NodeKind::Event, std::move(pins));
        s.event_key = id;
        return s;
    };
}

std::optional<PinType> OperandType(std::string_view suffix, const std::vector<ValueType>& allowed) {
    std::optional<PinType> type = ParseType(suffix);
    if (!type || type->IsExec() || type->is_array ||
        std::find(allowed.begin(), allowed.end(), type->type) == allowed.end()) {
        return std::nullopt;
    }
    return type;
}

void AddMathFamily(Registry& r, const std::string& prefix, const std::string& title,
                   std::initializer_list<ValueType> allowed, bool returns_bool) {
    const std::vector<ValueType> types(allowed);
    r.families[prefix] = {
        [title, types, returns_bool](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> type = OperandType(c.suffix, types);
            if (!type) {
                return Fail(error, "BP007", "'" + title + "' doesn't take " + std::string(c.suffix) + " values.");
            }
            const PinType result = returns_bool ? T(ValueType::Bool) : *type;
            return Sig(title, "Math|" + TypeName(*type), NodeKind::Pure,
                       {In("a", *type), In("b", *type), Out("result", result)});
        },
        [prefix, title, types](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (ValueType t : types) {
                const std::string name = TypeName(T(t));
                entries.push_back({prefix + name, title + " (" + name + ")", "Math|" + name});
            }
            return entries;
        }};
}

void AddPure(Registry& r, const std::string& id, const std::string& title, const std::string& category,
             std::vector<PinDesc> pins) {
    r.exact[id] = [title, category, pins](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig(title, category, NodeKind::Pure, pins);
    };
}

std::vector<PinDesc> ParamsFrom(const std::vector<Variable>& vars, PinDir dir) {
    std::vector<PinDesc> pins;
    for (const Variable& v : vars) {
        pins.push_back({v.name, dir, v.type, dir == PinDir::In ? v.default_value : Value{}, Pin_None});
    }
    return pins;
}

// Custom event parameters from config: [{"name": "amount", "type": "float"}].
bool CustomParams(const json& config, std::vector<Variable>& out, std::string& error) {
    for (const json& p : config.value("params", json::array())) {
        Variable v;
        v.name = p.value("name", "");
        std::optional<PinType> type = ParseType(p.value("type", ""));
        if (v.name.empty() || !type || type->IsExec()) {
            error = "a parameter has no name or an unknown type";
            return false;
        }
        v.type = *type;
        v.default_value = DefaultValue(v.type);
        out.push_back(std::move(v));
    }
    return true;
}

const Node* FindCustomEvent(const Blueprint& bp, std::string_view name) {
    for (const Graph& g : bp.graphs) {
        for (const Node& n : g.nodes) {
            if (n.type == "Event.Custom" && n.config.value("name", "") == name) return &n;
        }
    }
    return nullptr;
}

// "Health.Heal" -> the reflected type and member name (type names may contain "::").
bool SplitMember(std::string_view suffix, const reflect::TypeInfo*& type, std::string& member) {
    const usize dot = suffix.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= suffix.size()) {
        return false;
    }
    type = reflect::TypeRegistry::Find(suffix.substr(0, dot));
    member = std::string(suffix.substr(dot + 1));
    return type != nullptr && type->kind == reflect::TypeKind::Struct;
}

void RegisterBuiltins(Registry& r) {
    const PinType kBool = T(ValueType::Bool), kInt = T(ValueType::Int), kFloat = T(ValueType::Float),
                  kString = T(ValueType::String), kVec3 = T(ValueType::Vec3), kEntity = T(ValueType::Entity);

    // --- Events (BLUEPRINT_NODES.md §1) ------------------------------------
    AddEvent(r, "Event.BeginPlay", "Event BeginPlay", {});
    AddEvent(r, "Event.EndPlay", "Event EndPlay", {});
    AddEvent(r, "Event.Tick", "Event Tick", {Out("delta_seconds", kFloat)});
    AddEvent(r, "Event.FixedTick", "Event FixedTick", {Out("fixed_delta", kFloat)});
    AddEvent(r, "Event.OnTriggerEnter", "Event OnTriggerEnter", {Out("other", kEntity)});
    AddEvent(r, "Event.OnTriggerExit", "Event OnTriggerExit", {Out("other", kEntity)});
    AddEvent(r, "Event.OnCollisionBegin", "Event OnCollisionBegin", {Out("other", kEntity)});
    AddEvent(r, "Event.OnCollisionEnd", "Event OnCollisionEnd", {Out("other", kEntity)});
    r.exact["Event.Custom"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const std::string name = c.node.config.value("name", "");
        if (name.empty()) return Fail(error, "BP007", "A Custom Event needs a name.");
        std::vector<Variable> params;
        std::string message;
        if (!CustomParams(c.node.config, params, message)) return Fail(error, "BP007", "Custom Event '" + name + "': " + message + ".");
        std::vector<PinDesc> pins{ExecOut()};
        for (PinDesc& p : ParamsFrom(params, PinDir::Out)) pins.push_back(std::move(p));
        NodeSignature s = Sig(name, "Events", NodeKind::Event, std::move(pins));
        s.event_key = "Event.Custom:" + name;
        return s;
    };
    r.families["Call.Custom:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Node* event = FindCustomEvent(c.blueprint, c.suffix);
            if (event == nullptr) {
                return Fail(error, "BP004", "The custom event '" + std::string(c.suffix) + "' no longer exists.");
            }
            std::vector<Variable> params;
            std::string message;
            CustomParams(event->config, params, message);
            std::vector<PinDesc> pins{ExecIn(), ExecOut()};
            for (PinDesc& p : ParamsFrom(params, PinDir::In)) pins.push_back(std::move(p));
            return Sig("Call " + std::string(c.suffix), "Events", NodeKind::Impure, std::move(pins));
        },
        [](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Graph& g : bp.graphs)
                for (const Node& n : g.nodes)
                    if (n.type == "Event.Custom" && !n.config.value("name", "").empty()) {
                        const std::string name = n.config.value("name", "");
                        entries.push_back({"Call.Custom:" + name, "Call " + name, "Events"});
                    }
            return entries;
        }};

    // --- Flow control (§2) -------------------------------------------------
    r.exact["Flow.Branch"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Branch", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("condition", kBool, Pin_WarnIfUnconnected), ExecOut("true"), ExecOut("false")});
    };
    r.exact["Flow.Sequence"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json count = c.node.config.value("count", json(2));
        if (!count.is_number_integer() || count.get<int>() < 1 || count.get<int>() > 32) {
            return Fail(error, "BP007", "Sequence needs between 1 and 32 outputs.");
        }
        std::vector<PinDesc> pins{ExecIn()};
        for (int i = 0; i < count.get<int>(); ++i) pins.push_back(ExecOut("then " + std::to_string(i)));
        return Sig("Sequence", "Flow Control", NodeKind::Impure, std::move(pins));
    };

    // Stateful flow nodes: their state lives per instance (a compiler "state slot").
    r.exact["Flow.DoOnce"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Do Once", "Flow Control", NodeKind::Impure,
                   {ExecIn(), ExecIn("reset"), In("start_closed", kBool), ExecOut("completed")});
    };
    r.exact["Flow.DoN"] = [kInt](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Do N", "Flow Control", NodeKind::Impure,
                   {ExecIn("enter"), In("n", kInt, i32{1}), ExecIn("reset"), ExecOut("exit"), Out("counter", kInt)});
    };
    r.exact["Flow.Gate"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Gate", "Flow Control", NodeKind::Impure,
                   {ExecIn("enter"), ExecIn("open"), ExecIn("close"), ExecIn("toggle"), In("start_closed", kBool),
                    ExecOut("exit")});
    };
    r.exact["Flow.FlipFlop"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Flip Flop", "Flow Control", NodeKind::Impure,
                   {ExecIn(), ExecOut("A"), ExecOut("B"), Out("is_a", kBool)});
    };

    // --- Latent nodes (§3) -------------------------------------------------
    r.exact["Latent.Delay"] = [kFloat](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Delay", "Latent", NodeKind::Latent, {ExecIn(), In("duration", kFloat, 0.2f), ExecOut("completed")});
    };
    r.exact["Latent.RetriggerableDelay"] = [kFloat](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Retriggerable Delay", "Latent", NodeKind::Latent,
                   {ExecIn(), In("duration", kFloat, 0.2f), ExecOut("completed")});
    };
    r.exact["Latent.DelayNextTick"] = [](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Delay Until Next Tick", "Latent", NodeKind::Latent, {ExecIn(), ExecOut("completed")});
    };

    // --- Variables (§4) ----------------------------------------------------
    auto variable_lister = [](const std::string& prefix, const std::string& verb) {
        return [prefix, verb](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Variable& v : bp.variables) entries.push_back({prefix + v.name, verb + " " + v.name, "Variables"});
            return entries;
        };
    };
    r.families["Var.Get:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Variable* v = c.blueprint.FindVariable(c.suffix);
            if (v == nullptr) {
                return Fail(error, "BP005", "The variable '" + std::string(c.suffix) + "' was deleted. Replace or remove this node.");
            }
            return Sig("Get " + v->name, "Variables", NodeKind::Pure, {Out("value", v->type)});
        },
        variable_lister("Var.Get:", "Get")};
    r.families["Var.Set:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Variable* v = c.blueprint.FindVariable(c.suffix);
            if (v == nullptr) {
                return Fail(error, "BP005", "The variable '" + std::string(c.suffix) + "' was deleted. Replace or remove this node.");
            }
            return Sig("Set " + v->name, "Variables", NodeKind::Impure,
                       {ExecIn(), In("value", v->type, v->default_value), ExecOut(), Out("value", v->type)});
        },
        variable_lister("Var.Set:", "Set")};

    // Literals: config {"value": ...}.
    for (const auto& [id, type] : std::vector<std::pair<std::string, PinType>>{
             {"Literal.Bool", kBool}, {"Literal.Int", kInt}, {"Literal.Float", kFloat},
             {"Literal.String", kString}, {"Literal.Vec3", kVec3}}) {
        r.exact[id] = [type](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            Value value = DefaultValue(type);
            if (c.node.config.contains("value") && !ValueFromJson(c.node.config["value"], type, value)) {
                return Fail(error, "BP007", "The literal's value isn't a " + TypeName(type) + ".");
            }
            PinDesc out = Out("value", type);
            out.default_value = value;
            return Sig(ValueText(value), "Literals", NodeKind::Pure, {out});
        };
    }

    // --- Math (§5) ---------------------------------------------------------
    using V = ValueType;
    AddMathFamily(r, "Math.Add:", "Add", {V::Int, V::Float, V::Vec3}, false);
    AddMathFamily(r, "Math.Subtract:", "Subtract", {V::Int, V::Float, V::Vec3}, false);
    AddMathFamily(r, "Math.Multiply:", "Multiply", {V::Int, V::Float, V::Vec3}, false);
    AddMathFamily(r, "Math.Divide:", "Divide", {V::Int, V::Float}, false);
    AddMathFamily(r, "Math.Modulo:", "Modulo", {V::Int}, false);
    AddMathFamily(r, "Math.Min:", "Min", {V::Int, V::Float}, false);
    AddMathFamily(r, "Math.Max:", "Max", {V::Int, V::Float}, false);
    AddMathFamily(r, "Math.Less:", "<", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.LessEqual:", "<=", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.Greater:", ">", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.GreaterEqual:", ">=", {V::Int, V::Float}, true);
    AddMathFamily(r, "Math.Equal:", "==", {V::Bool, V::Int, V::Float, V::String, V::Entity}, true);
    AddMathFamily(r, "Math.NotEqual:", "!=", {V::Bool, V::Int, V::Float, V::String, V::Entity}, true);
    AddPure(r, "Math.And", "AND", "Math|bool", {In("a", kBool), In("b", kBool), Out("result", kBool)});
    AddPure(r, "Math.Or", "OR", "Math|bool", {In("a", kBool), In("b", kBool), Out("result", kBool)});
    AddPure(r, "Math.Xor", "XOR", "Math|bool", {In("a", kBool), In("b", kBool), Out("result", kBool)});
    AddPure(r, "Math.Not", "NOT", "Math|bool", {In("a", kBool), Out("result", kBool)});
    AddPure(r, "Math.Negate:float", "Negate (float)", "Math|float", {In("a", kFloat), Out("result", kFloat)});
    AddPure(r, "Math.Abs:float", "Abs (float)", "Math|float", {In("a", kFloat), Out("result", kFloat)});
    AddPure(r, "Math.Clamp:float", "Clamp (float)", "Math|float",
            {In("value", kFloat), In("min", kFloat), In("max", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.Lerp:float", "Lerp (float)", "Math|float",
            {In("a", kFloat), In("b", kFloat, 1.0f), In("alpha", kFloat), Out("result", kFloat)});
    AddPure(r, "Vec3.Make", "Make Vec3", "Math|Vec3",
            {In("x", kFloat), In("y", kFloat), In("z", kFloat), Out("result", kVec3)});
    AddPure(r, "Vec3.Break", "Break Vec3", "Math|Vec3",
            {In("vector", kVec3), Out("x", kFloat), Out("y", kFloat), Out("z", kFloat)});
    AddPure(r, "Vec3.Scale", "Vec3 * float", "Math|Vec3", {In("vector", kVec3), In("scale", kFloat, 1.0f), Out("result", kVec3)});
    AddPure(r, "Vec3.Length", "Length", "Math|Vec3", {In("vector", kVec3), Out("result", kFloat)});
    AddPure(r, "Vec3.Dot", "Dot", "Math|Vec3", {In("a", kVec3), In("b", kVec3), Out("result", kFloat)});
    AddPure(r, "Vec3.Cross", "Cross", "Math|Vec3", {In("a", kVec3), In("b", kVec3), Out("result", kVec3)});
    AddPure(r, "Vec3.Normalize", "Normalize", "Math|Vec3", {In("vector", kVec3), Out("result", kVec3)});

    // More math (§5): unary float functions, rounding to int, int variants,
    // random numbers.
    for (const char* name : {"Sin", "Cos", "Tan", "Asin", "Acos", "Atan", "Sqrt", "Exp", "Log", "Frac",
                             "DegreesToRadians", "RadiansToDegrees"}) {
        AddPure(r, std::string("Math.") + name, name, "Math|float", {In("a", kFloat), Out("result", kFloat)});
    }
    AddPure(r, "Math.Atan2", "Atan2", "Math|float", {In("y", kFloat), In("x", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.Power", "Power", "Math|float", {In("base", kFloat), In("exponent", kFloat, 1.0f), Out("result", kFloat)});
    for (const char* name : {"Floor", "Ceil", "Round", "Truncate"}) {
        AddPure(r, std::string("Math.") + name, name, "Math|float", {In("a", kFloat), Out("result", kInt)});
    }
    AddPure(r, "Math.Negate:int", "Negate (int)", "Math|int", {In("a", kInt), Out("result", kInt)});
    AddPure(r, "Math.Abs:int", "Abs (int)", "Math|int", {In("a", kInt), Out("result", kInt)});
    AddPure(r, "Math.Clamp:int", "Clamp (int)", "Math|int",
            {In("value", kInt), In("min", kInt), In("max", kInt, i32{1}), Out("result", kInt)});
    AddPure(r, "Math.NearlyEqual:float", "Nearly Equal (float)", "Math|float",
            {In("a", kFloat), In("b", kFloat), In("tolerance", kFloat, 0.0001f), Out("result", kBool)});
    AddPure(r, "Math.MapRange:float", "Map Range Clamped", "Math|float",
            {In("value", kFloat), In("in_min", kFloat), In("in_max", kFloat, 1.0f), In("out_min", kFloat),
             In("out_max", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.RandomFloatInRange", "Random Float in Range", "Math|Random",
            {In("min", kFloat), In("max", kFloat, 1.0f), Out("result", kFloat)});
    AddPure(r, "Math.RandomIntInRange", "Random Int in Range", "Math|Random",
            {In("min", kInt), In("max", kInt, i32{1}), Out("result", kInt)});
    AddPure(r, "Math.RandomBool", "Random Bool", "Math|Random", {Out("result", kBool)});

    // --- Loops and switches (§2) --------------------------------------------
    r.exact["Flow.ForLoop"] = [kInt](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("For Loop", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("first", kInt), In("last", kInt), ExecOut("loop_body"), Out("index", kInt),
                    ExecOut("completed")});
    };
    r.exact["Flow.ForLoopWithBreak"] = [kInt](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("For Loop with Break", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("first", kInt), In("last", kInt), ExecIn("break"), ExecOut("loop_body"),
                    Out("index", kInt), ExecOut("completed")});
    };
    r.exact["Flow.WhileLoop"] = [kBool](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("While Loop", "Flow Control", NodeKind::Impure,
                   {ExecIn(), In("condition", kBool, Pin_WarnIfUnconnected), ExecOut("loop_body"), ExecOut("completed")});
    };
    r.exact["Flow.SwitchInt"] = [kInt](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json cases = c.node.config.value("cases", json::array({0, 1}));
        std::vector<PinDesc> pins{ExecIn(), In("selection", kInt)};
        std::set<i32> seen;
        if (!cases.is_array()) return Fail(error, "BP007", "Switch on Int needs a list of cases.");
        for (const json& k : cases) {
            if (!k.is_number_integer() || !seen.insert(k.get<i32>()).second) {
                return Fail(error, "BP007", "Switch on Int's cases must be distinct whole numbers.");
            }
            pins.push_back(ExecOut(std::to_string(k.get<i32>())));
        }
        pins.push_back(ExecOut("default"));
        return Sig("Switch on Int", "Flow Control", NodeKind::Impure, std::move(pins));
    };
    r.exact["Flow.SwitchString"] = [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json cases = c.node.config.value("cases", json::array());
        std::vector<PinDesc> pins{ExecIn(), In("selection", kString)};
        std::set<std::string> seen;
        if (!cases.is_array()) return Fail(error, "BP007", "Switch on String needs a list of cases.");
        for (const json& k : cases) {
            if (!k.is_string() || k.get<std::string>().empty() || k.get<std::string>() == "default" ||
                !seen.insert(k.get<std::string>()).second) {
                return Fail(error, "BP007", "Switch on String's cases must be distinct, non-empty, and not 'default'.");
            }
            pins.push_back(ExecOut(k.get<std::string>()));
        }
        pins.push_back(ExecOut("default"));
        return Sig("Switch on String", "Flow Control", NodeKind::Impure, std::move(pins));
    };
    r.families["Flow.Select:"] = {
        [kInt](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> type = ParseType(c.suffix);
            if (!type || type->IsExec() || type->is_array || type->type == ValueType::Wildcard) {
                return Fail(error, "BP007", "Select can't choose between " + std::string(c.suffix) + " values.");
            }
            const json count = c.node.config.value("count", json(2));
            if (!count.is_number_integer() || count.get<int>() < 2 || count.get<int>() > 16) {
                return Fail(error, "BP007", "Select needs between 2 and 16 options.");
            }
            std::vector<PinDesc> pins{In("index", kInt)};
            for (int i = 0; i < count.get<int>(); ++i) pins.push_back(In("option " + std::to_string(i), *type));
            pins.push_back(Out("return", *type));
            return Sig("Select", "Flow Control", NodeKind::Pure, std::move(pins));
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const char* t : {"bool", "int", "float", "string", "Vec3", "Entity"})
                entries.push_back({std::string("Flow.Select:") + t, std::string("Select (") + t + ")", "Flow Control"});
            return entries;
        }};

    // --- Strings and text (§9) ----------------------------------------------
    r.exact["String.Append"] = [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json count = c.node.config.value("count", json(2));
        if (!count.is_number_integer() || count.get<int>() < 2 || count.get<int>() > 16) {
            return Fail(error, "BP007", "Append needs between 2 and 16 inputs.");
        }
        std::vector<PinDesc> pins;
        for (int i = 0; i < count.get<int>(); ++i) pins.push_back(In(std::string(1, static_cast<char>('a' + i)), kString));
        pins.push_back(Out("result", kString));
        return Sig("Append", "String", NodeKind::Pure, std::move(pins));
    };
    AddPure(r, "String.Length", "Length", "String", {In("text", kString), Out("result", kInt)});
    AddPure(r, "String.IsEmpty", "Is Empty", "String", {In("text", kString), Out("result", kBool)});
    AddPure(r, "String.Contains", "Contains", "String",
            {In("text", kString), In("substring", kString), In("ignore_case", kBool), Out("result", kBool)});
    AddPure(r, "String.ToUpper", "To Upper", "String", {In("text", kString), Out("result", kString)});
    AddPure(r, "String.ToLower", "To Lower", "String", {In("text", kString), Out("result", kString)});
    AddPure(r, "String.Trim", "Trim", "String", {In("text", kString), Out("result", kString)});
    AddPure(r, "String.ToInt", "String to Int", "String",
            {In("text", kString), Out("result", kInt), Out("success", kBool)});
    AddPure(r, "String.ToFloat", "String to Float", "String",
            {In("text", kString), Out("result", kFloat), Out("success", kBool)});
    r.exact["Text.Format"] = [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        const json format = c.node.config.value("format", json(""));
        if (!format.is_string()) return Fail(error, "BP007", "Format Text needs a format string.");
        std::vector<PinDesc> pins;
        for (const std::string& arg : ParseFormatArgs(format.get<std::string>())) {
            if (arg == "result") return Fail(error, "BP007", "Format Text can't have a {result} placeholder.");
            pins.push_back(In(arg, kString)); // anything connects: it's converted to text
        }
        pins.push_back(Out("result", kString));
        return Sig("Format Text", "String", NodeKind::Pure, std::move(pins));
    };
    r.families["Conv.ToString:"] = {
        [kString](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const std::optional<PinType> type = ParseType(c.suffix);
            if (!type || type->IsExec() || type->is_array || CanConnect(*type, PinType::Of(ValueType::String)) == Compat::No ||
                type->type == ValueType::String) {
                return Fail(error, "BP007", "There's no text form for " + std::string(c.suffix) + " values.");
            }
            return Sig("To String (" + TypeName(*type) + ")", "Conversions", NodeKind::Pure,
                       {In("value", *type), Out("result", kString)});
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const char* t : {"bool", "int", "float", "Vec3", "Quat", "Entity"})
                entries.push_back({std::string("Conv.ToString:") + t, std::string("To String (") + t + ")", "Conversions"});
            return entries;
        }};

    // --- Entity (§6) and debug (§11) ---------------------------------------
    AddPure(r, "Entity.Self", "Get Self", "Entity", {Out("self", kEntity)});
    AddPure(r, "Entity.IsValid", "Is Valid", "Entity", {In("entity", kEntity), Out("result", kBool)});
    r.exact["Debug.Print"] = [kString](const NodeContext&, NodeError&) -> std::optional<NodeSignature> {
        return Sig("Print String", "Debug", NodeKind::Impure,
                   {ExecIn(), In("text", kString, std::string("Hello")), ExecOut()});
    };

    // --- Functions (§12) ---------------------------------------------------
    r.exact["Function.Entry"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        if (c.graph.kind != GraphKind::Function) return Fail(error, "BP010", "Function Entry belongs in a function graph.");
        std::vector<PinDesc> pins{ExecOut()};
        for (PinDesc& p : ParamsFrom(c.graph.inputs, PinDir::Out)) pins.push_back(std::move(p));
        return Sig(c.graph.name, "Functions", NodeKind::FunctionEntry, std::move(pins));
    };
    r.exact["Function.Return"] = [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
        if (c.graph.kind != GraphKind::Function) return Fail(error, "BP010", "Return Node belongs in a function graph.");
        std::vector<PinDesc> pins;
        if (!c.graph.pure) pins.push_back(ExecIn());
        for (PinDesc& p : ParamsFrom(c.graph.outputs, PinDir::In)) pins.push_back(std::move(p));
        return Sig("Return Node", "Functions", NodeKind::FunctionReturn, std::move(pins));
    };
    r.families["Call.Self:"] = {
        [](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const Graph* fn = c.blueprint.FindGraph(c.suffix);
            if (fn == nullptr || fn->kind != GraphKind::Function) {
                return Fail(error, "BP004", "The function '" + std::string(c.suffix) + "' no longer exists on this Blueprint.");
            }
            std::vector<PinDesc> pins;
            if (!fn->pure) pins.push_back(ExecIn());
            for (PinDesc& p : ParamsFrom(fn->inputs, PinDir::In)) pins.push_back(std::move(p));
            if (!fn->pure) pins.push_back(ExecOut());
            for (PinDesc& p : ParamsFrom(fn->outputs, PinDir::Out)) pins.push_back(std::move(p));
            return Sig(fn->name, "Functions", fn->pure ? NodeKind::Pure : NodeKind::Impure, std::move(pins));
        },
        [](const Blueprint& bp) {
            std::vector<PaletteEntry> entries;
            for (const Graph& g : bp.graphs)
                if (g.kind == GraphKind::Function) entries.push_back({"Call.Self:" + g.name, g.name, "Functions"});
            return entries;
        }};

    // --- Reflection: native calls and component fields (§12.3) -------------
    r.families["Call.Native:"] = {
        [kEntity](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
            const reflect::TypeInfo* type = nullptr;
            std::string name;
            const reflect::FunctionInfo* fn = nullptr;
            if (SplitMember(c.suffix, type, name)) fn = type->FindFunction(name);
            if (fn == nullptr || !fn->HasFlag(reflect::Fn_BlueprintCallable)) {
                const std::string owner = type != nullptr ? type->name : std::string(c.suffix.substr(0, c.suffix.rfind('.')));
                return Fail(error, "BP004", "The function '" + name + "' no longer exists on '" + owner +
                                                "'. It may have been renamed; check CoreRedirects.");
            }
            const bool pure = fn->HasFlag(reflect::Fn_Pure);
            std::vector<PinDesc> pins;
            if (!pure) pins.push_back(ExecIn());
            if (!fn->HasFlag(reflect::Fn_Static)) pins.push_back(In("target", kEntity, Pin_Self));
            for (const reflect::ParamInfo& param : fn->params) {
                std::optional<PinType> pt = param.type != nullptr ? PinTypeOf(*param.type) : std::nullopt;
                if (!pt) return Fail(error, "BP007", std::string("'") + fn->name + "' takes a parameter Blueprints can't pass.");
                pins.push_back(In(param.name, *pt));
            }
            if (!pure) pins.push_back(ExecOut());
            if (fn->return_type != nullptr) {
                std::optional<PinType> pt = PinTypeOf(*fn->return_type);
                if (!pt) return Fail(error, "BP007", std::string("'") + fn->name + "' returns a type Blueprints can't use.");
                pins.push_back(Out("return", *pt));
            }
            NodeSignature s = Sig(fn->name, std::string("Components|") + type->name, pure ? NodeKind::Pure : NodeKind::Impure,
                                  std::move(pins));
            s.owner = type;
            s.function = fn;
            return s;
        },
        [](const Blueprint&) {
            std::vector<PaletteEntry> entries;
            for (const reflect::TypeInfo* type : reflect::TypeRegistry::AllTypes())
                for (const reflect::FunctionInfo& fn : type->functions)
                    if (fn.HasFlag(reflect::Fn_BlueprintCallable))
                        entries.push_back({std::string("Call.Native:") + type->name + "." + fn.name, fn.name,
                                           std::string("Components|") + type->name});
            return entries;
        }};
    auto field_family = [kEntity](bool set) -> Family {
        const std::string prefix = set ? "Comp.Set:" : "Comp.Get:";
        return {[kEntity, set](const NodeContext& c, NodeError& error) -> std::optional<NodeSignature> {
                    const reflect::TypeInfo* type = nullptr;
                    std::string name;
                    const reflect::FieldInfo* field = nullptr;
                    if (SplitMember(c.suffix, type, name)) field = type->FindField(name);
                    if (field == nullptr || !field->HasFlag(reflect::Field_BlueprintReadWrite)) {
                        return Fail(error, "BP004", "The field '" + name + "' no longer exists on '" +
                                                        (type != nullptr ? std::string(type->name) : std::string(c.suffix)) +
                                                        "' or isn't BlueprintReadWrite.");
                    }
                    std::optional<PinType> pt = PinTypeOf(*field->type);
                    if (!pt) return Fail(error, "BP007", std::string("Blueprints can't use the type of '") + field->name + "'.");
                    NodeSignature s = set ? Sig(std::string("Set ") + field->name, std::string("Components|") + type->name,
                                                NodeKind::Impure,
                                                {ExecIn(), In("target", kEntity, Pin_Self), In("value", *pt), ExecOut(),
                                                 Out("value", *pt)})
                                          : Sig(std::string("Get ") + field->name, std::string("Components|") + type->name,
                                                NodeKind::Pure, {In("target", kEntity, Pin_Self), Out("value", *pt)});
                    s.owner = type;
                    s.field = field;
                    return s;
                },
                [prefix, set](const Blueprint&) {
                    std::vector<PaletteEntry> entries;
                    for (const reflect::TypeInfo* type : reflect::TypeRegistry::AllTypes())
                        for (const reflect::FieldInfo& field : type->fields)
                            if (field.HasFlag(reflect::Field_BlueprintReadWrite))
                                entries.push_back({prefix + type->name + "." + field.name,
                                                   std::string(set ? "Set " : "Get ") + field.name,
                                                   std::string("Components|") + type->name});
                    return entries;
                }};
    };
    r.families["Comp.Get:"] = field_family(false);
    r.families["Comp.Set:"] = field_family(true);
}

} // namespace

std::vector<std::string> ParseFormatArgs(std::string_view format) {
    std::vector<std::string> args;
    for (usize i = 0; i < format.size(); ++i) {
        if (format[i] == '{' && i + 1 < format.size() && format[i + 1] == '{') {
            ++i;
            continue;
        }
        if (format[i] != '{') continue;
        const usize end = format.find('}', i + 1);
        if (end == std::string_view::npos) break;
        const std::string name(format.substr(i + 1, end - i - 1));
        if (!name.empty() && std::find(args.begin(), args.end(), name) == args.end()) args.push_back(name);
        i = end;
    }
    return args;
}

const PinDesc* NodeSignature::Find(std::string_view pin, PinDir dir) const {
    for (const PinDesc& p : pins) {
        if (p.dir == dir && p.name == pin) return &p;
    }
    return nullptr;
}

const PinDesc* NodeSignature::Find(std::string_view pin) const {
    for (const PinDesc& p : pins) {
        if (p.name == pin) return &p;
    }
    return nullptr;
}

void RegisterNodeType(const std::string& id, NodeFactory factory) { GetRegistry().exact[id] = std::move(factory); }

void RegisterNodeFamily(const std::string& prefix, NodeFactory factory, FamilyLister lister) {
    GetRegistry().families[prefix] = {std::move(factory), std::move(lister)};
}

std::optional<NodeSignature> ResolveNode(const Blueprint& blueprint, const Graph& graph, const Node& node,
                                         NodeError* error) {
    Registry& r = GetRegistry();
    NodeError local;
    NodeError& e = error != nullptr ? *error : local;
    if (auto it = r.exact.find(node.type); it != r.exact.end()) {
        return it->second(NodeContext{blueprint, graph, node, {}}, e);
    }
    const usize colon = node.type.find(':');
    if (colon != std::string::npos) {
        if (auto it = r.families.find(node.type.substr(0, colon + 1)); it != r.families.end()) {
            return it->second.factory(
                NodeContext{blueprint, graph, node, std::string_view(node.type).substr(colon + 1)}, e);
        }
    }
    return Fail(e, "BP007", "Unknown node type '" + node.type + "'.");
}

std::vector<PaletteEntry> ListNodeTypes(const Blueprint& blueprint) {
    Registry& r = GetRegistry();
    std::vector<PaletteEntry> entries;
    Graph scratch;
    scratch.kind = GraphKind::EventGraph;
    for (const auto& [id, factory] : r.exact) {
        Node node;
        node.type = id;
        NodeError error;
        if (std::optional<NodeSignature> s = factory(NodeContext{blueprint, scratch, node, {}}, error)) {
            entries.push_back({id, id == "Event.Custom" ? "Custom Event" : s->title, s->category});
        } else if (id == "Event.Custom") {
            entries.push_back({id, "Custom Event", "Events"});
        } else if (id.rfind("Function.", 0) == 0) {
            entries.push_back({id, id == "Function.Entry" ? "Function Entry" : "Return Node", "Functions"});
        }
    }
    for (const auto& [prefix, family] : r.families) {
        if (family.lister) {
            for (PaletteEntry& e : family.lister(blueprint)) entries.push_back(std::move(e));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const PaletteEntry& a, const PaletteEntry& b) {
        return a.category != b.category ? a.category < b.category : a.title != b.title ? a.title < b.title : a.id < b.id;
    });
    return entries;
}

} // namespace aether::bp
