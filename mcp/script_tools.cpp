#include "script_tools.h"

#if AETHER_MCP_SCRIPTING

#include "project_host.h"

#include "aether/ecs/component.h"
#include "aether/reflection/serialize.h"
#include "aether/scene/components.h"
#include "aether/scene/entity_guid.h"
#include "aether/scene/lifecycle.h"
#include "aether/scene/script_component.h"
#include "aether/script/completion.h"
#include "aether/script/luau_host.h"
#include "aether/script/script_system.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;
namespace sc = aether::script;

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Source {
    std::string text;
    std::string name; // the chunk name errors carry
    std::string from;
};

// A script's source from `source` (the text) or `asset` (a .luau in the open project).
Source ReadSource(AssetHost& host, const Json& args) {
    Source s;
    if (args.contains("source")) {
        if (!args["source"].is_string()) throw ToolError("\"source\" must be the script's text");
        s.text = args["source"].get<std::string>();
        s.name = "chunk";
        s.from = "source";
        return s;
    }
    if (args.contains("asset")) {
        OpenProject& p = host.Require();
        const AssetRecord& r = FindAsset(p, RequireString(args, "asset"));
        if (r.importer != "Script") throw ToolError(r.path + " is not a script (it is a " + r.importer + ")");
        s.text = ReadAll(p.database->SourcePath(r.guid));
        s.name = fs::path(r.path).filename().string();
        s.from = r.path;
        return s;
    }
    throw ToolError("Give \"source\" (the Luau text) or \"asset\" (a script in the open project)");
}

Json ErrorJson(const std::string& text) {
    const sc::ScriptError e = sc::ScriptError::Parse(text);
    return {{"text", text}, {"chunk", e.chunk}, {"line", e.line}, {"message", e.message}};
}

Json ClassJson(const ScriptClassInfo& info) {
    Json vars = Json::array();
    for (const ExposedVariable& v : info.variables) {
        static const char* kinds[] = {"number", "bool", "string", "vector"};
        Json row = {{"name", v.name}, {"kind", kinds[static_cast<int>(v.kind)]}, {"default", v.default_value}};
        if (v.has_range) row["range"] = Json::array({v.range_min, v.range_max});
        if (!v.tooltip.empty()) row["tooltip"] = v.tooltip;
        vars.push_back(row);
    }
    return {{"variables", vars}, {"callbacks", info.callbacks}};
}

Json ToJson(const sc::ScriptValue& v) {
    struct Visitor {
        Json operator()(std::monostate) const { return nullptr; }
        Json operator()(bool b) const { return b; }
        Json operator()(f64 d) const { return d; }
        Json operator()(const std::string& s) const { return s; }
        Json operator()(const sc::EntityRef& e) const { return e.entity.IsNull() ? Json(nullptr) : Json("Entity(" + std::to_string(e.entity.index) + ")"); }
    };
    return std::visit(Visitor{}, v);
}

sc::ScriptValue FromJson(const Json& j, const std::string& what) {
    if (j.is_null()) return std::monostate{};
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return j.get<std::string>();
    throw ToolError(what + " must be null, a boolean, a number or a string");
}

sc::LuauHost::Options HostOptions(const Json& args) {
    sc::LuauHost::Options o;
    if (args.contains("instruction_budget")) {
        if (!args["instruction_budget"].is_number_integer() || args["instruction_budget"].get<long long>() < 0) throw ToolError("\"instruction_budget\" must be an integer, 0 or more (0 = none)");
        o.instruction_budget = args["instruction_budget"].get<u64>();
    }
    return o;
}

// An entity with the named components, their fields from JSON (the same way create_entity does it).
Entity MakeEntity(World& world, const Json& components, const std::string& what) {
    Entity e = world.CreateEntity(Transform{Vec3(0, 0, 0), Quaternion::Identity()});
    if (components.is_null()) return e;
    if (!components.is_object()) throw ToolError(what + " must be an object of component name to fields");
    for (auto& [name, value] : components.items()) {
        const ComponentId id = FindComponentIdByName(name);
        if (id == kInvalidComponentId || id >= RegisteredComponentCount() || GetComponentInfo(id).reflected == nullptr) throw ToolError(what + ": unknown component \"" + name + "\" (see list_component_types)");
        if (!world.HasComponentRaw(e, id)) world.AddComponentRaw(e, id);
        reflect::LoadReport report;
        if (!reflect::FromJson(*GetComponentInfo(id).reflected, world.GetComponentRaw(e, id), value, &report)) throw ToolError(what + ": the value does not fit " + name);
    }
    return e;
}

} // namespace

void RegisterScriptTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"script_check",
         "Compile a Luau script and describe what it is: whether it compiles (the error with its line), the variables it exposes (name, kind, default, `--@range` and "
         "`--@tooltip`) and the lifecycle callbacks it defines (OnCreate, OnStart, OnUpdate ...). A script is a module that returns a table (its class). Give the text as "
         "`source`, or `asset` (a .luau in the open project).",
         Schema({{"source", {{"type", "string"}, {"description", "The script's text"}}}, {"asset", {{"type", "string"}, {"description", "A script asset in the open project"}}}}),
         [host](const Json& args) -> Json {
             const Source src = ReadSource(*host, args);
             sc::LuauHost vm;
             const ScriptClassInfo info = sc::ScriptSystem::DescribeSource(vm, src.text, src.name);
             Json out = {{"ok", info.ok}, {"script", src.from}, {"lines", static_cast<usize>(std::count(src.text.begin(), src.text.end(), '\n')) + 1}};
             if (!info.ok) out["error"] = ErrorJson(info.error);
             else out["class"] = ClassJson(info);
             return out;
         }});

    server.AddTool(
        {"script_complete",
         "Code completion for the engine's Luau API, as the editor offers it: what could be typed at a position. After `world:` its methods, after `e:Get(\"` the component "
         "names, after `transform.` the component's reflected fields, `self.` the class's own fields, \"function Class:\" the lifecycle callbacks; otherwise keywords, globals and "
         "the locals in scope. Give `cursor` (a byte offset) or `line` and `column` (1-based). Returns the items (label, kind, detail) and where the replaced text starts.",
         Schema({{"source", {{"type", "string"}}}, {"cursor", {{"type", "integer"}}}, {"line", {{"type", "integer"}}}, {"column", {{"type", "integer"}}}, {"limit", {{"type", "integer"}, {"description", "Default 60"}}}}, {"source"}),
         [](const Json& args) -> Json {
             const std::string text = RequireString(args, "source");
             usize cursor = text.size();
             if (args.contains("cursor")) {
                 if (!args["cursor"].is_number_integer() || args["cursor"].get<long long>() < 0 || args["cursor"].get<usize>() > text.size()) throw ToolError("\"cursor\" must be a byte offset inside the source");
                 cursor = args["cursor"].get<usize>();
             } else if (args.contains("line") && args.contains("column")) {
                 if (!args["line"].is_number_integer() || !args["column"].is_number_integer() || args["line"].get<long long>() < 1 || args["column"].get<long long>() < 1) throw ToolError("\"line\" and \"column\" are 1-based integers");
                 usize line = 1, i = 0;
                 while (i < text.size() && line < args["line"].get<usize>()) {
                     if (text[i++] == '\n') ++line;
                 }
                 if (line != args["line"].get<usize>()) throw ToolError("The source has fewer lines than that");
                 cursor = std::min(text.size(), i + args["column"].get<usize>() - 1);
             }
             usize limit = 60;
             if (args.contains("limit") && args["limit"].is_number_integer()) limit = static_cast<usize>(std::max<long long>(1, args["limit"].get<long long>()));
             const sc::CompletionResult r = sc::CompleteScript(text, cursor);
             static const char* kinds[] = {"keyword", "variable", "global", "module", "function", "method", "field", "component", "callback"};
             Json items = Json::array();
             for (const sc::CompletionItem& it : r.items) {
                 if (items.size() >= limit) break;
                 items.push_back({{"label", it.label}, {"kind", kinds[static_cast<int>(it.kind)]}, {"detail", it.detail}});
             }
             return {{"cursor", cursor}, {"replace_from", r.replace_from}, {"replacing", text.substr(r.replace_from, cursor - r.replace_from)}, {"total", r.items.size()}, {"items", items}};
         }});

    server.AddTool(
        {"script_run",
         "Run Luau in the sandbox and report what happened: everything print() wrote, the values the chunk returned, and the error with its line if it failed. With `call` "
         "({function, args}) a global function the chunk defined is called afterwards, so a function can be tested on its own. A fresh world is bound, so `world:Spawn()`, "
         "`world:EntitiesWith(...)` and entity :Get / :Add work. The sandbox has math, string, table, bit32, utf8, buffer, coroutine and vector, and os.clock; no file or network "
         "access. A runaway script stops at the instruction budget (default 10 million) with an error.",
         Schema({{"source", {{"type", "string"}}}, {"asset", {{"type", "string"}}},
                 {"call", {{"type", "object"}, {"description", "{function: name, args: [..]} to call after the chunk"}}},
                 {"instruction_budget", {{"type", "integer"}}}}),
         [host](const Json& args) -> Json {
             const Source src = ReadSource(*host, args);
             sc::LuauHost vm(HostOptions(args));
             World world;
             GuidIndex guids;
             vm.BindWorld(&world, &guids);
             Json prints = Json::array();
             vm.SetPrintHandler([&prints](const std::string& text) { if (prints.size() < 2000) prints.push_back(text); });
             sc::ScriptResult run = vm.Run(src.text, src.name);
             Json out = {{"ok", run.ok}, {"script", src.from}};
             Json values = Json::array();
             for (const sc::ScriptValue& v : run.values) values.push_back(ToJson(v));
             out["returned"] = values;
             if (!run.ok) out["error"] = ErrorJson(run.error);
             if (run.ok && args.contains("call")) {
                 const Json& call = args["call"];
                 if (!call.is_object() || !call.contains("function") || !call["function"].is_string()) throw ToolError("\"call\" must be {function: name, args: [...]}");
                 std::vector<sc::ScriptValue> call_args;
                 if (call.contains("args")) {
                     if (!call["args"].is_array()) throw ToolError("\"call.args\" must be an array");
                     for (const Json& a : call["args"]) call_args.push_back(FromJson(a, "A call argument"));
                 }
                 const sc::ScriptResult called = vm.Call(call["function"].get<std::string>(), call_args);
                 Json results = Json::array();
                 for (const sc::ScriptValue& v : called.values) results.push_back(ToJson(v));
                 out["call"] = {{"function", call["function"]}, {"ok", called.ok}, {"returned", results}};
                 if (!called.ok) {
                     out["call"]["error"] = ErrorJson(called.error);
                     out["ok"] = false;
                 }
             }
             out["prints"] = prints;
             out["memory_bytes"] = vm.MemoryUsed();
             out["entities_in_world"] = world.EntityCount();
             return out;
         }});

    server.AddTool(
        {"script_simulate",
         "Run a script class on an entity the way the game does, headless: an entity with a Transform and a ScriptComponent is made, play begins (OnCreate, OnEnable, OnStart), "
         "then `ticks` frames of `dt` seconds run (OnFixedUpdate, OnUpdate, OnLateUpdate, timers from Timer.After and Timer.Every), then play ends (OnDisable, OnDestroy). "
         "`variables` overrides the script's exposed variables (checked against their kinds); `components` adds components to the scripted entity ({\"Health\": {...}}); `entities` "
         "adds other entities ([{components: {...}}]) for the script to find with world:EntitiesWith. Returns the prints, errors, the entity's final Transform and every exposed "
         "variable's final value.",
         Schema({{"source", {{"type", "string"}}}, {"asset", {{"type", "string"}}},
                 {"ticks", {{"type", "integer"}, {"description", "0-20000 (default 60)"}}}, {"dt", {{"type", "number"}, {"description", "Seconds per tick (default 1/60)"}}},
                 {"variables", {{"type", "object"}}}, {"components", {{"type", "object"}}}, {"entities", {{"type", "array"}}}, {"instruction_budget", {{"type", "integer"}}}}),
         [host](const Json& args) -> Json {
             const Source src = ReadSource(*host, args);
             auto number = [&](const char* key, double fallback, double lo, double hi) {
                 if (!args.contains(key)) return fallback;
                 if (!args[key].is_number() || !(args[key].get<double>() >= lo) || !(args[key].get<double>() <= hi)) throw ToolError(std::string("\"") + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
                 return args[key].get<double>();
             };
             const unsigned ticks = static_cast<unsigned>(number("ticks", 60, 0, 20000));
             const f32 dt = static_cast<f32>(number("dt", 1.0 / 60.0, 0.0001, 10.0));

             sc::LuauHost vm(HostOptions(args));
             World world;
             GuidIndex guids;
             Json prints = Json::array();
             vm.SetPrintHandler([&prints](const std::string& text) { if (prints.size() < 2000) prints.push_back(text); });

             const assets::AssetGuid script_guid = assets::NewAssetGuid();
             sc::ScriptSystem::SourceLoader loader = [&](const assets::AssetGuid& g, std::string& text, std::string& name) {
                 if (!(g == script_guid)) return false;
                 text = src.text;
                 name = src.name;
                 return true;
             };
             sc::ScriptSystem scripts(vm, world, guids, loader);
             Lifecycle lifecycle(world, guids);
             scripts.Register(lifecycle);

             const ScriptClassInfo info = scripts.ClassInfo(script_guid);
             if (!info.ok) return {{"ok", false}, {"stage", "compile"}, {"script", src.from}, {"error", ErrorJson(info.error)}};

             Entity subject = MakeEntity(world, args.contains("components") ? args["components"] : Json(nullptr), "\"components\"");
             ScriptComponent component;
             component.script.guid = script_guid;
             if (args.contains("variables")) {
                 if (!args["variables"].is_object()) throw ToolError("\"variables\" must be an object of variable name to value");
                 for (auto& [name, value] : args["variables"].items()) {
                     const ExposedVariable* v = info.Find(name);
                     if (v == nullptr) throw ToolError("The script exposes no variable \"" + name + "\" (see script_check)");
                     if (!MatchesKind(v->kind, value)) throw ToolError("Variable \"" + name + "\" does not take that value (see script_check for its kind)");
                     component.SetProperty(name, value);
                 }
             }
             world.AddComponent(subject, component);
             if (args.contains("entities")) {
                 if (!args["entities"].is_array() || args["entities"].size() > 200) throw ToolError("\"entities\" must be an array of up to 200 {components} objects");
                 for (const Json& e : args["entities"]) {
                     if (!e.is_object()) throw ToolError("Each entity must be an object like {components: {...}}");
                     MakeEntity(world, e.contains("components") ? e["components"] : Json(nullptr), "an entity's \"components\"");
                 }
             }

             lifecycle.BeginPlay();
             for (unsigned i = 0; i < ticks; ++i) {
                 lifecycle.FixedUpdate(dt);
                 lifecycle.Update(dt);
                 scripts.Tick(dt);
                 lifecycle.LateUpdate(dt);
             }
             Json fields = Json::object();
             for (const ExposedVariable& v : info.variables) fields[v.name] = ToJson(scripts.GetField(subject, v.name));
             Json transform = nullptr;
             if (world.IsAlive(subject)) {
                 if (const Transform* t = world.GetComponent<Transform>(subject)) {
                     transform = {{"position", Json::array({t->position.x, t->position.y, t->position.z})}, {"rotation", Json::array({t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w})}};
                 }
             }
             const bool alive = world.IsAlive(subject);
             const usize instances = scripts.InstanceCount();
             const usize timers = scripts.TimerCount();
             lifecycle.EndPlay(); // OnDisable and OnDestroy run here, so their errors count too
             Json errors = Json::array();
             for (const std::string& e : scripts.Errors()) errors.push_back(ErrorJson(e));
             return {{"ok", errors.empty()}, {"script", src.from}, {"ticks", ticks}, {"seconds", ticks * dt}, {"callbacks", info.callbacks}, {"prints", prints}, {"errors", errors},
                     {"entity_alive", alive}, {"transform", transform}, {"fields", fields}, {"instances", instances}, {"timers_pending", timers}, {"entities", world.EntityCount()}};
         }});

    server.AddTool(
        {"script_check_project",
         "Check every script in the open project and every ScriptComponent in its scenes and prefabs: each script must compile (errors with their line), and each ScriptComponent must "
         "name a script that exists, with every variable it overrides one the script exposes and a value of the right kind. Lists each script's exposed variables and callbacks.",
         Schema(Json::object()), [host](const Json&) -> Json {
             OpenProject& p = host->Require();
             struct Info {
                 ScriptClassInfo info;
                 std::string path;
             };
             std::map<std::string, Info> by_guid;
             Json scripts = Json::array(), problems = Json::array();
             for (const AssetRecord* r : p.database->All()) {
                 if (r->IsSubAsset() || r->importer != "Script") continue;
                 sc::LuauHost vm;
                 const std::string text = ReadAll(p.database->SourcePath(r->guid));
                 ScriptClassInfo info = sc::ScriptSystem::DescribeSource(vm, text, fs::path(r->path).filename().string());
                 Json row = {{"path", r->path}, {"ok", info.ok}};
                 if (!info.ok) {
                     row["error"] = ErrorJson(info.error);
                     problems.push_back({{"path", r->path}, {"problem", info.error}});
                 } else {
                     row["class"] = ClassJson(info);
                 }
                 scripts.push_back(row);
                 by_guid[assets::ToString(r->guid)] = {info, r->path};
             }
             usize components = 0;
             for (const AssetRecord* r : p.database->All()) {
                 if (r->IsSubAsset() || (r->importer != "Scene" && r->importer != "Prefab")) continue;
                 const fs::path file = p.database->SourcePath(r->guid);
                 if (file.extension() == ".aesc") continue;
                 const Json doc = Json::parse(ReadAll(file), nullptr, false);
                 if (!doc.is_object() || !doc.contains("entities") || !doc["entities"].is_array()) continue;
                 for (const Json& e : doc["entities"]) {
                     if (!e.is_object() || !e.contains("components") || !e["components"].is_object() || !e["components"].contains("ScriptComponent") || !e["components"]["ScriptComponent"].is_object()) continue;
                     ++components;
                     const Json& c = e["components"]["ScriptComponent"];
                     const std::string guid = c.value("script", std::string());
                     if (guid.empty()) {
                         problems.push_back({{"path", r->path}, {"problem", "a ScriptComponent has no script"}});
                         continue;
                     }
                     const auto it = by_guid.find(guid);
                     if (it == by_guid.end()) {
                         problems.push_back({{"path", r->path}, {"problem", "a ScriptComponent's script " + guid + " is not a script in the project"}});
                         continue;
                     }
                     if (!it->second.info.ok || !c.contains("properties") || !c["properties"].is_array()) continue;
                     for (const Json& prop : c["properties"]) {
                         const std::string name = prop.value("name", std::string());
                         const ExposedVariable* v = it->second.info.Find(name);
                         if (v == nullptr) {
                             problems.push_back({{"path", r->path}, {"problem", "overrides \"" + name + "\", which " + it->second.path + " does not expose"}});
                             continue;
                         }
                         const Json value = Json::parse(prop.value("value", std::string("null")), nullptr, false);
                         if (value.is_discarded() || !MatchesKind(v->kind, value)) problems.push_back({{"path", r->path}, {"problem", "the value of \"" + name + "\" does not fit the variable's kind in " + it->second.path}});
                     }
                 }
             }
             return {{"ok", problems.empty()}, {"scripts", scripts}, {"script_components", components}, {"problems", problems}};
         }});
}

} // namespace aether::mcp

#else // no Luau scripting module in this build

namespace aether::mcp {
void RegisterScriptTools(McpServer&, std::shared_ptr<detail::AssetHost>) {}
} // namespace aether::mcp

#endif
