#include "blueprint_tools.h"

#include "project_host.h"

#include "aether/blueprint/compiler.h"
#include "aether/blueprint/graph.h"
#include "aether/blueprint/nodes.h"
#include "aether/blueprint/validate.h"
#include "aether/blueprint/vm.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <span>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A Blueprint from `definition` (an object or its text) or `asset` (a .abp in the open project).
bp::Blueprint ReadBlueprint(AssetHost& host, const Json& args, std::string* from) {
    std::string text;
    if (args.contains("definition")) {
        const Json& d = args["definition"];
        if (d.is_object()) text = d.dump();
        else if (d.is_string()) text = d.get<std::string>();
        else throw ToolError("\"definition\" must be a JSON object or a string of JSON");
        if (from != nullptr) *from = "definition";
    } else if (args.contains("asset")) {
        OpenProject& p = host.Require();
        const AssetRecord& r = FindAsset(p, RequireString(args, "asset"));
        if (r.importer != "Blueprint") throw ToolError(r.path + " is not a Blueprint (it is a " + r.importer + ")");
        text = ReadAll(p.database->SourcePath(r.guid));
        if (from != nullptr) *from = r.path;
    } else {
        throw ToolError("Give \"definition\" (the Blueprint as JSON) or \"asset\" (a Blueprint in the open project)");
    }
    const Json parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded()) throw ToolError("The Blueprint is not valid JSON");
    bp::Blueprint blueprint;
    std::string error;
    if (!bp::BlueprintFromJson(parsed, blueprint, &error)) throw ToolError("Not a valid Blueprint: " + error);
    return blueprint;
}

Json DiagnosticsJson(const bp::ValidationResult& r) {
    Json out = Json::array();
    for (const bp::Diagnostic& d : r.diagnostics) {
        Json row = {{"code", d.code}, {"severity", d.severity == bp::Severity::Error ? "error" : "warning"}, {"graph", d.graph}, {"message", d.message}};
        if (d.node != 0) row["node"] = d.node;
        if (!d.pin.empty()) row["pin"] = d.pin;
        out.push_back(row);
    }
    return out;
}

Json PinJson(const bp::PinDesc& pin) {
    Json j = {{"name", pin.name}, {"direction", pin.dir == bp::PinDir::In ? "in" : "out"}, {"type", bp::TypeName(pin.type)}};
    if (pin.dir == bp::PinDir::In && !pin.type.IsExec()) {
        const Json def = bp::ValueToJson(pin.default_value);
        if (!def.is_null()) j["default"] = def;
    }
    if (pin.flags & bp::Pin_WarnIfUnconnected) j["warns_if_unconnected"] = true;
    if (pin.flags & bp::Pin_Self) j["defaults_to_self"] = true;
    if (pin.flags & bp::Pin_ByRef) j["by_reference"] = true;
    return j;
}

const char* KindName(bp::NodeKind k) {
    switch (k) {
    case bp::NodeKind::Event: return "event";
    case bp::NodeKind::Impure: return "impure";
    case bp::NodeKind::Pure: return "pure";
    case bp::NodeKind::Latent: return "latent";
    case bp::NodeKind::FunctionEntry: return "function_entry";
    case bp::NodeKind::FunctionReturn: return "function_return";
    case bp::NodeKind::Tunnel: return "tunnel";
    }
    return "impure";
}

// A JSON value as a VM argument or override.
bp::VmValue ToVm(const Json& j, const std::string& what) {
    if (j.is_null()) return std::monostate{};
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_integer()) return static_cast<i32>(j.get<long long>());
    if (j.is_number()) return j.get<f32>();
    if (j.is_string()) return j.get<std::string>();
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) return Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
    if (j.is_array() && j.size() == 4 && j[0].is_number() && j[1].is_number() && j[2].is_number() && j[3].is_number()) return Quaternion(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>(), j[3].get<f32>());
    throw ToolError(what + " must be a bool, number, string, [x,y,z] or [x,y,z,w]");
}

Json FromVm(const bp::VmValue& v) {
    struct Visitor {
        Json operator()(std::monostate) const { return nullptr; }
        Json operator()(bool b) const { return b; }
        Json operator()(i32 i) const { return i; }
        Json operator()(f32 f) const { return f; }
        Json operator()(const std::string& s) const { return s; }
        Json operator()(const Vec3& p) const { return Json::array({p.x, p.y, p.z}); }
        Json operator()(const Quaternion& q) const { return Json::array({q.x, q.y, q.z, q.w}); }
        Json operator()(const Entity& e) const { return e.IsNull() ? Json(nullptr) : Json("Entity(" + std::to_string(e.index) + ")"); }
    };
    return std::visit(Visitor{}, v);
}

} // namespace

Json BlueprintSampleJson() {
    // Built with the real node library, then saved: if a node id or pin changes, the sample stops validating and the test says so.
    // BeginPlay prints a greeting and sets Count to 5; the custom event "Hit" adds one to Count and prints.
    bp::Blueprint blueprint;
    bp::Variable count;
    count.name = "Count";
    count.type = bp::PinType::Of(bp::ValueType::Int);
    count.default_value = i32(0);
    blueprint.variables.push_back(count);
    bp::Graph graph;
    graph.name = "EventGraph";
    graph.kind = bp::GraphKind::EventGraph;
    bp::GraphBuilder b(graph);
    const bp::NodeId begin = b.Add("Event.BeginPlay", Json::object(), 0, 0);
    const bp::NodeId hello = b.Add("Debug.Print", Json::object(), 240, 0);
    const bp::NodeId set_five = b.Add("Var.Set:Count", Json::object(), 480, 0);
    b.Default(hello, "text", "Hello from BeginPlay");
    b.Default(set_five, "value", 5);
    b.Connect(begin, "then", hello, "exec").Connect(hello, "then", set_five, "exec");
    const bp::NodeId hit = b.Add("Event.Custom", Json{{"name", "Hit"}}, 0, 200);
    const bp::NodeId set_count = b.Add("Var.Set:Count", Json::object(), 240, 200);
    const bp::NodeId add = b.Add("Math.Add:int", Json::object(), 0, 340);
    const bp::NodeId get_count = b.Add("Var.Get:Count", Json::object(), -240, 340);
    const bp::NodeId hit_print = b.Add("Debug.Print", Json::object(), 480, 200);
    b.Default(add, "b", 1);
    b.Default(hit_print, "text", "Hit!");
    b.Connect(hit, "then", set_count, "exec").Connect(set_count, "then", hit_print, "exec");
    b.Connect(get_count, "value", add, "a").Connect(add, "result", set_count, "value");
    blueprint.graphs.push_back(graph);
    return bp::BlueprintToJson(blueprint);
}

void RegisterBlueprintTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"bp_node_types",
         "The node types you can put in a Blueprint graph: id (\"Flow.Branch\", \"Var.Get:Count\", \"Call.Native:...\"), title and category, sorted by category. "
         "Give `blueprint` (a definition or asset) to include its own variables, functions and custom events. `search` filters by id, title or category "
         "(case-insensitive); `limit` caps the list (default 150).",
         Schema({{"search", {{"type", "string"}}},
                 {"category", {{"type", "string"}, {"description", "Only this palette category, e.g. \"Flow Control\""}}},
                 {"definition", {{"description", "A Blueprint whose variables, functions and events join the list"}}},
                 {"asset", {{"type", "string"}}},
                 {"limit", {{"type", "integer"}}}}),
         [host](const Json& args) -> Json {
             bp::Blueprint blueprint;
             if (args.contains("definition") || args.contains("asset")) blueprint = ReadBlueprint(*host, args, nullptr);
             const std::string search = args.contains("search") && args["search"].is_string() ? args["search"].get<std::string>() : "";
             const std::string category = args.contains("category") && args["category"].is_string() ? args["category"].get<std::string>() : "";
             auto lower = [](std::string s) {
                 std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                 return s;
             };
             const std::string needle = lower(search);
             usize limit = 150;
             if (args.contains("limit") && args["limit"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(1, args["limit"].get<long long>()));
             Json out = Json::array();
             usize total = 0;
             std::set<std::string> categories;
             for (const bp::PaletteEntry& e : bp::ListNodeTypes(blueprint)) {
                 categories.insert(e.category);
                 if (!category.empty() && e.category != category) continue;
                 if (!needle.empty() && lower(e.id).find(needle) == std::string::npos && lower(e.title).find(needle) == std::string::npos && lower(e.category).find(needle) == std::string::npos) continue;
                 ++total;
                 if (out.size() < limit) out.push_back({{"id", e.id}, {"title", e.title}, {"category", e.category}});
             }
             return {{"total", total}, {"nodes", out}, {"categories", categories}};
         }});

    server.AddTool(
        {"bp_node_info",
         "The pins of a node type: each pin's name, direction, type (exec, bool, int, float, string, Vec3, Quat, Entity, a struct, Array<T>) and default, and the node's "
         "title, category and kind (event, impure, pure, latent...). `type` is a node id from bp_node_types; `config` is the node's settings where it has any "
         "(a Sequence's count, a custom event's name). Give `blueprint` for nodes that depend on one (Var.Get:Count, custom events, function calls).",
         Schema({{"type", {{"type", "string"}}}, {"config", {{"type", "object"}}}, {"definition", {{"description", "A Blueprint for variable, function and event nodes"}}}, {"asset", {{"type", "string"}}}}, {"type"}),
         [host](const Json& args) -> Json {
             bp::Blueprint blueprint;
             if (args.contains("definition") || args.contains("asset")) blueprint = ReadBlueprint(*host, args, nullptr);
             bp::Graph graph;
             graph.name = "EventGraph";
             bp::Node node;
             node.id = 1;
             node.type = RequireString(args, "type");
             if (args.contains("config")) {
                 if (!args["config"].is_object()) throw ToolError("\"config\" must be an object");
                 node.config = args["config"];
             }
             bp::NodeError error;
             const std::optional<bp::NodeSignature> sig = bp::ResolveNode(blueprint, graph, node, &error);
             if (!sig) throw ToolError("Cannot resolve \"" + node.type + "\": " + error.code + " " + error.message);
             Json pins = Json::array();
             for (const bp::PinDesc& p : sig->pins) pins.push_back(PinJson(p));
             Json out = {{"type", node.type}, {"title", sig->title}, {"category", sig->category}, {"kind", KindName(sig->kind)}, {"pins", pins}};
             if (!sig->event_key.empty()) out["event_key"] = sig->event_key;
             return out;
         }});

    server.AddTool(
        {"bp_validate",
         "Check a Blueprint graph with the Blueprint module's own diagnostics: each has a code (BP001...), severity, graph, node, pin and message. Give the "
         "Blueprint as `definition` or `asset`. ok is true when there are no errors (warnings may remain).",
         Schema({{"definition", {{"description", "The Blueprint: a JSON object, or its text"}}}, {"asset", {{"type", "string"}, {"description", "A Blueprint asset in the open project"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const bp::Blueprint blueprint = ReadBlueprint(*host, args, &from);
             const bp::ValidationResult result = bp::ValidateBlueprint(blueprint);
             return {{"ok", result.Ok()}, {"blueprint", from}, {"errors", result.errors}, {"warnings", result.warnings}, {"diagnostics", DiagnosticsJson(result)}};
         }});

    server.AddTool(
        {"bp_compile",
         "Compile a Blueprint to bytecode (validating first): whether it compiled, the diagnostics, and for each event and function its instruction count; with "
         "`disassemble` the listing too.",
         Schema({{"definition", {{"description", "The Blueprint: a JSON object, or its text"}}}, {"asset", {{"type", "string"}}}, {"disassemble", {{"type", "boolean"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const bp::Blueprint blueprint = ReadBlueprint(*host, args, &from);
             const bp::CompileResult result = bp::CompileBlueprint(blueprint);
             Json out = {{"ok", result.Ok()}, {"blueprint", from}, {"diagnostics", DiagnosticsJson(result.diagnostics)}};
             if (result.Ok()) {
                 Json functions = Json::array();
                 const bool dis = args.contains("disassemble") && args["disassemble"].is_boolean() && args["disassemble"].get<bool>();
                 for (const bp::CompiledFunction& f : result.blueprint->functions) {
                     Json row = {{"name", f.name}, {"graph", f.graph}, {"instructions", f.code.size()}};
                     if (dis) row["listing"] = bp::Disassemble(*result.blueprint, f);
                     functions.push_back(row);
                 }
                 Json events = Json::array();
                 for (const auto& [key, index] : result.blueprint->events) events.push_back(key);
                 out["functions"] = functions;
                 out["events"] = events;
                 out["variables"] = result.blueprint->variables.size();
             }
             return out;
         }});

    server.AddTool(
        {"bp_run",
         "Run a Blueprint headless on a test entity and report what happened: it is compiled (errors are returned), attached to an entity, its BeginPlay runs "
         "(unless `begin_play` is false), then each of `events` is dispatched in order ({event: \"Event.Custom:Hit\", args: [...]}), then `ticks` frames of "
         "`dt` seconds (Event Tick, Delay and other latent actions). Returns everything Print String printed, every variable's final value, runtime errors "
         "(BP201 invalid entity, BP202 instruction budget, BP203 call depth ...), the instructions run, the game time and the latent actions still pending. "
         "`variables` overrides instance-editable variables; `seed` makes Random nodes repeatable. Nothing physical exists here: physics traces find nothing.",
         Schema({{"definition", {{"description", "The Blueprint: a JSON object, or its text"}}},
                 {"asset", {{"type", "string"}}},
                 {"begin_play", {{"type", "boolean"}, {"description", "Run Event BeginPlay first (default true)"}}},
                 {"events", {{"type", "array"}, {"description", "[{event, args?}]"}}},
                 {"ticks", {{"type", "integer"}, {"description", "Frames of Event Tick to run, 0-10000 (default 0)"}}},
                 {"dt", {{"type", "number"}, {"description", "Seconds per tick (default 1/60)"}}},
                 {"variables", {{"type", "object"}, {"description", "Overrides for instance-editable variables"}}},
                 {"seed", {{"type", "integer"}}},
                 {"instruction_budget", {{"type", "integer"}, {"description", "Per dispatch (default 1,000,000; 0 = none)"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const bp::Blueprint blueprint = ReadBlueprint(*host, args, &from);
             const bp::CompileResult compiled = bp::CompileBlueprint(blueprint);
             if (!compiled.Ok()) return {{"ok", false}, {"stage", "compile"}, {"blueprint", from}, {"diagnostics", DiagnosticsJson(compiled.diagnostics)}};

             auto number = [&](const char* key, double fallback, double lo, double hi) {
                 if (!args.contains(key)) return fallback;
                 if (!args[key].is_number() || !(args[key].get<double>() >= lo) || !(args[key].get<double>() <= hi)) {
                     throw ToolError(std::string("\"") + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
                 }
                 return args[key].get<double>();
             };
             const unsigned ticks = static_cast<unsigned>(number("ticks", 0, 0, 10000));
             const double dt = number("dt", 1.0 / 60.0, 0.0001, 10.0);
             bp::BlueprintVM::Options options;
             options.instruction_budget = static_cast<u64>(number("instruction_budget", 1000000, 0, 1.0e10));
             options.random_seed = static_cast<u32>(number("seed", 1, 0, 4.0e9));
             Json overrides = Json::object();
             if (args.contains("variables")) {
                 if (!args["variables"].is_object()) throw ToolError("\"variables\" must be an object");
                 overrides = args["variables"];
             }
             const bool begin_play = !args.contains("begin_play") || !args["begin_play"].is_boolean() || args["begin_play"].get<bool>();
             std::vector<std::pair<std::string, std::vector<bp::VmValue>>> dispatches;
             if (args.contains("events")) {
                 if (!args["events"].is_array() || args["events"].size() > 200) throw ToolError("\"events\" must be an array of up to 200 {event, args} objects");
                 for (const Json& ev : args["events"]) {
                     if (!ev.is_object() || !ev.contains("event") || !ev["event"].is_string()) throw ToolError("Each event needs a string \"event\", e.g. \"Event.Custom:Hit\"");
                     std::vector<bp::VmValue> values;
                     if (ev.contains("args")) {
                         if (!ev["args"].is_array()) throw ToolError("An event's \"args\" must be an array");
                         for (const Json& a : ev["args"]) values.push_back(ToVm(a, "An event argument"));
                     }
                     dispatches.emplace_back(ev["event"].get<std::string>(), std::move(values));
                 }
             }

             World world;
             Entity entity = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
             bp::BlueprintVM vm(world, options);
             Json prints = Json::array();
             vm.SetPrintHandler([&prints](Entity, const std::string& text) { if (prints.size() < 2000) prints.push_back(text); });
             if (!vm.Attach(entity, compiled.blueprint, overrides)) throw ToolError("Could not attach the Blueprint to the test entity");
             Json events_run = Json::array();
             auto run_event = [&](const std::string& name, std::span<const bp::VmValue> values) {
                 Json row = {{"event", name}};
                 if (compiled.blueprint->events.find(name) == compiled.blueprint->events.end()) {
                     row["ran"] = false;
                     row["reason"] = "the Blueprint has no such event";
                 } else {
                     row["ran"] = vm.Dispatch(entity, name, values);
                 }
                 events_run.push_back(row);
             };
             if (begin_play) run_event("Event.BeginPlay", {});
             for (const auto& [name, values] : dispatches) run_event(name, values);
             for (unsigned i = 0; i < ticks; ++i) vm.Tick(static_cast<f32>(dt));

             Json variables = Json::object();
             for (const bp::Variable& v : blueprint.variables) {
                 if (v.type.is_array) {
                     Json items = Json::array();
                     for (const bp::VmValue& item : vm.GetArray(entity, v.name)) items.push_back(FromVm(item));
                     variables[v.name] = items;
                 } else {
                     variables[v.name] = FromVm(vm.GetVariable(entity, v.name));
                 }
             }
             Json errors = Json::array();
             for (const bp::RuntimeError& e : vm.Errors()) {
                 errors.push_back({{"code", e.code}, {"function", e.function}, {"node", e.node}, {"message", e.message}});
             }
             return {{"ok", errors.empty()}, {"blueprint", from}, {"events", events_run}, {"prints", prints}, {"variables", variables}, {"errors", errors},
                     {"instructions_run", vm.InstructionsRun()}, {"game_time", vm.GameTime()}, {"pending_latent_actions", vm.PendingLatentActions()},
                     {"warnings", DiagnosticsJson(compiled.diagnostics)}};
         }});
}

} // namespace aether::mcp
