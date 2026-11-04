#include "animation_tools.h"

#include "project_host.h"

#include "aether/animation/anim_graph.h"
#include "aether/animation/blend_space.h"
#include "aether/animation/montage.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;

// The data from `definition` (an object or its text) or `file` (under Content/ of the open project, or absolute).
Json ReadDefinition(AssetHost& host, const Json& args) {
    if (args.contains("definition")) {
        const Json& d = args["definition"];
        if (d.is_object()) return d;
        if (!d.is_string()) throw ToolError("\"definition\" must be a JSON object or a string of JSON");
        const Json parsed = Json::parse(d.get<std::string>(), nullptr, false);
        if (parsed.is_discarded()) throw ToolError("\"definition\" is not valid JSON");
        return parsed;
    }
    if (args.contains("file")) {
        const std::string name = RequireString(args, "file");
        fs::path path(name);
        if (!path.is_absolute()) path = host.Require().paths.content / CleanRelative(name);
        std::ifstream in(path, std::ios::binary);
        if (!in) throw ToolError("Can't read " + path.string());
        std::stringstream ss;
        ss << in.rdbuf();
        const Json parsed = Json::parse(ss.str(), nullptr, false);
        if (parsed.is_discarded()) throw ToolError(name + " is not valid JSON");
        return parsed;
    }
    throw ToolError("Give \"definition\" (the data as JSON) or \"file\" (a file under Content/)");
}

anim::BlendSpace ParseBlendSpace(const Json& j) {
    anim::BlendSpace space;
    std::string error;
    if (!anim::BlendSpaceFromJson(j, space, &error)) throw ToolError("Not a valid blend space: " + error);
    return space;
}
anim::Montage ParseMontage(const Json& j) {
    anim::Montage m;
    std::string error;
    if (!anim::MontageFromJson(j, m, &error)) throw ToolError("Not a valid montage: " + error);
    return m;
}
anim::AnimGraph ParseGraph(const Json& j) {
    anim::AnimGraph g;
    std::string error;
    if (!anim::AnimGraphFromJson(j, g, &error)) throw ToolError("Not a valid animation graph: " + error);
    return g;
}

// "MN002: message" -> {code, message}.
Json LineJson(const std::string& line) {
    const usize colon = line.find(": ");
    if (colon != std::string::npos && colon <= 8) return {{"code", line.substr(0, colon)}, {"error", true}, {"message", line.substr(colon + 2)}};
    return {{"code", ""}, {"error", true}, {"message", line}};
}

// Blend spaces named by a graph, from `blend_spaces` (name -> definition).
std::map<std::string, anim::BlendSpace> ReadBlendSpaces(const Json& args) {
    std::map<std::string, anim::BlendSpace> out;
    if (!args.contains("blend_spaces")) return out;
    if (!args["blend_spaces"].is_object()) throw ToolError("\"blend_spaces\" must be an object of name -> blend space");
    for (const auto& item : args["blend_spaces"].items()) out[item.key()] = ParseBlendSpace(item.value());
    return out;
}

// Stand-in clips: one bone, no keys, a length and some notifies.
struct ClipBank {
    std::map<std::string, anim::AnimationClip> clips;
    f32 default_duration = 1.0f;

    explicit ClipBank(const Json& args) {
        if (args.contains("default_clip_duration") && args["default_clip_duration"].is_number()) default_duration = args["default_clip_duration"].get<f32>();
        if (!(default_duration > 0.0f)) throw ToolError("default_clip_duration must be above 0");
        if (!args.contains("clips")) return;
        if (!args["clips"].is_object()) throw ToolError("\"clips\" must be an object of name -> seconds or {duration, notifies}");
        for (const auto& item : args["clips"].items()) {
            const std::string& name = item.key();
            const Json& c = item.value();
            anim::AnimationClip clip = Make(name);
            if (c.is_number()) {
                clip.duration = c.get<f32>();
            } else if (c.is_object()) {
                if (c.contains("duration")) clip.duration = c["duration"].get<f32>();
                if (c.contains("notifies")) {
                    for (const Json& n : c["notifies"]) {
                        anim::AnimNotify a;
                        a.name = n.value("name", std::string{});
                        a.time = n.value("time", 0.0f);
                        a.duration = n.value("duration", 0.0f);
                        clip.notifies.push_back(a);
                    }
                }
            }
            if (!(clip.duration > 0.0f)) throw ToolError("Clip " + name + " needs a duration above 0");
            clips[name] = clip;
        }
    }
    anim::AnimationClip Make(const std::string& name) const {
        anim::AnimationClip c;
        c.name = name;
        c.duration = default_duration;
        c.tracks.resize(1);
        return c;
    }
    const anim::AnimationClip* Get(const std::string& name) {
        auto it = clips.find(name);
        if (it == clips.end()) it = clips.emplace(name, Make(name)).first;
        return &it->second;
    }
};

anim::Skeleton OneBone() {
    anim::Skeleton s;
    anim::Bone b;
    b.name = "root";
    s.bones.push_back(b);
    return s;
}

Json DiagnosticJson(const anim::AnimDiagnostic& d) {
    Json j = {{"code", d.code}, {"error", d.error}, {"message", d.message}};
    if (d.node != 0) j["node"] = d.node;
    if (!d.machine.empty()) j["machine"] = d.machine;
    return j;
}

Json EventJson(const anim::AnimEvent& e) {
    Json j = {{"type", anim::AnimEventTypeName(e.type)}, {"name", e.name}};
    if (!e.detail.empty()) j["detail"] = e.detail;
    if (!e.machine.empty()) j["machine"] = e.machine;
    if (e.interrupted) j["interrupted"] = true;
    return j;
}

bool Truthy(const Json& v) { return v.is_boolean() ? v.get<bool>() : v.is_number() && v.get<f32>() != 0.0f; }

} // namespace

void RegisterAnimationTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"anim_validate",
         "Check animation data with the animation module's own diagnostics. type \"graph\" (.aanim): AG001..AG011 (no output node, bad node inputs, loops, unknown "
         "or wrongly typed variables, state machine problems, unreachable states, duplicate names; with `clips` or `blend_spaces` it also checks the assets the "
         "graph names exist). type \"blend_space\" (.ablend): BS001..BS006 (no samples, samples in the same place, collinear 2D samples, bad axes). type \"montage\" "
         "(.amontage): MN001..MN004 (no clip, a section's next naming no section, duplicate sections, negative blend times). Give `definition` (JSON) or `file`.",
         Schema({{"type", {{"type", "string"}, {"enum", Json::array({"graph", "blend_space", "montage"})}}},
                 {"definition", {{"description", "The data: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}, {"description", "A file under the open project's Content/"}}},
                 {"clips", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "graph: the clip names that exist; any other clip the graph names is reported (AG010)"}}},
                 {"blend_spaces", {{"type", "object"}, {"description", "graph: name -> blend space definition; the blend spaces the graph's nodes may name"}}}},
                {"type"}),
         [host](const Json& args) -> Json {
             const std::string type = RequireString(args, "type");
             const Json data = ReadDefinition(*host, args);
             Json diagnostics = Json::array();
             bool ok = true;
             if (type == "graph") {
                 const anim::AnimGraph graph = ParseGraph(data);
                 const std::map<std::string, anim::BlendSpace> spaces = ReadBlendSpaces(args);
                 const std::vector<std::string> names = OptStrings(args, "clips");
                 const std::set<std::string> clip_names(names.begin(), names.end());
                 const anim::AnimationClip dummy;
                 anim::AnimAssets assets;
                 const bool check_assets = args.contains("clips") || args.contains("blend_spaces");
                 assets.clip = [&](const std::string& n) { return clip_names.count(n) ? &dummy : nullptr; };
                 assets.blend_space = [&](const std::string& n) -> const anim::BlendSpace* {
                     auto it = spaces.find(n);
                     return it == spaces.end() ? nullptr : &it->second;
                 };
                 for (const anim::AnimDiagnostic& d : anim::ValidateAnimGraph(graph, check_assets ? &assets : nullptr)) {
                     if (d.error) ok = false;
                     diagnostics.push_back(DiagnosticJson(d));
                 }
                 const Json summary = {{"variables", graph.variables.size()}, {"nodes", graph.nodes.size()}, {"machines", graph.machines.size()}, {"output", graph.output}};
                 return {{"ok", ok}, {"diagnostics", diagnostics}, {"summary", summary}};
             }
             if (type == "blend_space") {
                 const anim::BlendSpace space = ParseBlendSpace(data);
                 for (const anim::BlendDiagnostic& d : anim::ValidateBlendSpace(space)) {
                     if (d.error) ok = false;
                     Json j = {{"code", d.code}, {"error", d.error}, {"message", d.message}};
                     if (d.sample >= 0) j["sample"] = d.sample;
                     diagnostics.push_back(j);
                 }
                 const Json summary = {{"dimensions", space.dimensions}, {"samples", space.samples.size()}};
                 return {{"ok", ok}, {"diagnostics", diagnostics}, {"summary", summary}};
             }
             if (type == "montage") {
                 const anim::Montage m = ParseMontage(data);
                 for (const std::string& line : anim::ValidateMontage(m)) {
                     ok = false;
                     diagnostics.push_back(LineJson(line));
                 }
                 const Json summary = {{"clip", m.clip}, {"slot", m.slot}, {"sections", m.sections.size()}};
                 return {{"ok", ok}, {"diagnostics", diagnostics}, {"summary", summary}};
             }
             throw ToolError("type must be graph, blend_space or montage");
         }});

    server.AddTool(
        {"anim_blend_weights",
         "The clip weights a blend space gives at points (heaviest first, summing to 1). Parameters are clamped to the axes. Give `points` ([{x, y}]) or "
         "`grid` (points per axis: samples the whole parameter range evenly). Also returns the triangulation for a 2D space. Give `definition` or `file`.",
         Schema({{"definition", {{"description", "The blend space: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}}},
                 {"points", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                 {"grid", {{"type", "integer"}, {"description", "Points per axis across the range, 2..20"}}}}),
         [host](const Json& args) -> Json {
             anim::BlendSpace space = ParseBlendSpace(ReadDefinition(*host, args));
             for (const anim::BlendDiagnostic& d : anim::ValidateBlendSpace(space)) {
                 if (d.error) throw ToolError("The blend space is invalid (" + d.code + "): " + d.message);
             }
             if (space.dimensions == 2) anim::Triangulate(space);
             std::vector<std::pair<f32, f32>> points;
             if (args.contains("points")) {
                 if (!args["points"].is_array()) throw ToolError("\"points\" must be an array of {x, y}");
                 for (const Json& p : args["points"]) points.push_back({p.value("x", 0.0f), p.value("y", 0.0f)});
             } else {
                 const i32 requested = args.contains("grid") && args["grid"].is_number_integer() ? args["grid"].get<i32>() : 5;
                 const u32 n = static_cast<u32>(std::min(20, std::max(2, requested)));
                 for (u32 j = 0; j < (space.dimensions == 2 ? n : 1u); ++j) {
                     for (u32 i = 0; i < n; ++i) {
                         const f32 x = space.x.min + (space.x.max - space.x.min) * static_cast<f32>(i) / static_cast<f32>(n - 1);
                         const f32 y = space.dimensions == 2 ? space.y.min + (space.y.max - space.y.min) * static_cast<f32>(j) / static_cast<f32>(n - 1) : 0.0f;
                         points.push_back({x, y});
                     }
                 }
             }
             if (points.size() > 400) throw ToolError("At most 400 points");
             Json out = Json::array();
             for (const auto& pt : points) {
                 Json weights = Json::array();
                 for (const anim::BlendWeight& w : anim::ComputeWeights(space, pt.first, pt.second)) {
                     weights.push_back({{"clip", space.samples[w.sample].clip}, {"sample", w.sample}, {"weight", w.weight}});
                 }
                 Json p = {{"x", pt.first}, {"weights", weights}};
                 if (space.dimensions == 2) p["y"] = pt.second;
                 out.push_back(p);
             }
             Json tris = Json::array();
             for (const auto& t : space.triangles) tris.push_back(Json::array({t[0], t[1], t[2]}));
             const Json x_axis = {{"name", space.x.name}, {"min", space.x.min}, {"max", space.x.max}};
             return {{"dimensions", space.dimensions}, {"x_axis", x_axis}, {"points", out}, {"triangles", tris}};
         }});

    server.AddTool(
        {"anim_graph_simulate",
         "Run an animation graph headless and see what its state machines do. There is no skeleton or real clip: every clip the graph names is a one-bone clip, 1 second "
         "long unless `clips` ({name: seconds or {duration, notifies: [{name, time, duration}]}}) or `default_clip_duration` says otherwise. Give `steps`; each one may "
         "`set` variables ({name: value}; bools as true/false), `trigger` (names), `play_montage` (a montage definition; with `rate` and `section`), `stop_montage` (a slot), "
         "`jump_section` ({section, slot}), then advances `dt` seconds (default 1/30), `repeat` times (default 1). The result has, per step, the state each state machine "
         "is in (and whether it is blending), the state changes and the events (notifies, montage start / section / end) during it. Blend spaces the graph "
         "uses go in `blend_spaces`. Give the graph as `definition` or `file`.",
         Schema({{"definition", {{"description", "The graph: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}}},
                 {"clips", {{"type", "object"}}},
                 {"default_clip_duration", {{"type", "number"}}},
                 {"blend_spaces", {{"type", "object"}}},
                 {"steps", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
                {"steps"}),
         [host](const Json& args) -> Json {
             const anim::AnimGraph graph = ParseGraph(ReadDefinition(*host, args));
             const std::map<std::string, anim::BlendSpace> spaces = ReadBlendSpaces(args);
             ClipBank bank(args);
             anim::AnimAssets assets;
             assets.clip = [&](const std::string& n) { return bank.Get(n); };
             assets.blend_space = [&](const std::string& n) -> const anim::BlendSpace* {
                 auto it = spaces.find(n);
                 return it == spaces.end() ? nullptr : &it->second;
             };
             for (const anim::AnimDiagnostic& d : anim::ValidateAnimGraph(graph, &assets)) {
                 if (d.error) throw ToolError("The graph is invalid (" + d.code + "): " + d.message);
             }
             if (!args.contains("steps") || !args["steps"].is_array() || args["steps"].empty()) throw ToolError("Give a non-empty \"steps\" array");
             if (args["steps"].size() > 500) throw ToolError("At most 500 steps");

             const anim::Skeleton skeleton = OneBone();
             anim::AnimGraphInstance instance(graph, skeleton, assets);
             anim::Pose pose = anim::RestPose(skeleton);
             std::vector<const anim::AnimNode*> machines;
             for (const anim::AnimNode& n : graph.nodes) {
                 if (n.kind == anim::AnimNodeKind::StateMachine) machines.push_back(&n);
             }

             f32 time = 0.0f;
             u32 index = 0;
             Json out = Json::array();
             for (const Json& step : args["steps"]) {
                 const std::string where = "Step " + std::to_string(index) + ": ";
                 Json notes = Json::array();
                 if (step.contains("set")) {
                     for (const auto& item : step["set"].items()) {
                         const anim::AnimVariable* var = graph.FindVariable(item.key());
                         if (var == nullptr) throw ToolError(where + "no variable \"" + item.key() + "\"");
                         const Json& v = item.value();
                         switch (var->type) {
                         case anim::VarType::Bool: instance.SetBool(item.key(), Truthy(v)); break;
                         case anim::VarType::Int: instance.SetInt(item.key(), v.get<i32>()); break;
                         case anim::VarType::Float: instance.SetFloat(item.key(), v.get<f32>()); break;
                         case anim::VarType::Trigger:
                             if (Truthy(v)) instance.SetTrigger(item.key());
                             break;
                         }
                     }
                 }
                 if (step.contains("trigger")) {
                     for (const Json& t : step["trigger"]) {
                         if (!instance.SetTrigger(t.get<std::string>())) throw ToolError(where + "no trigger \"" + t.get<std::string>() + "\"");
                     }
                 }
                 if (step.contains("play_montage")) {
                     const anim::Montage m = ParseMontage(step["play_montage"]);
                     if (!instance.PlayMontage(m, step.value("rate", 1.0f), step.value("section", std::string{}))) notes.push_back("play_montage was refused");
                 }
                 if (step.contains("stop_montage")) instance.StopMontage(step["stop_montage"].get<std::string>());
                 if (step.contains("jump_section")) {
                     const Json& j = step["jump_section"];
                     if (!instance.JumpToSection(j.value("section", std::string{}), j.value("slot", std::string("Default")))) notes.push_back("jump_section found no such section playing");
                 }
                 const f32 dt = step.value("dt", 1.0f / 30.0f);
                 if (!(dt > 0.0f)) throw ToolError(where + "dt must be above 0");
                 const u32 repeat = static_cast<u32>(std::min(5000, std::max(1, step.value("repeat", 1))));
                 Json changes = Json::array(), events = Json::array();
                 for (u32 r = 0; r < repeat; ++r) {
                     instance.Update(dt, pose);
                     time += dt;
                     for (const anim::StateChange& c : instance.Changes()) changes.push_back({{"time", time}, {"machine", c.machine}, {"from", c.from}, {"to", c.to}});
                     for (const anim::AnimEvent& e : instance.Events()) {
                         if (e.type == anim::AnimEventType::StateChanged) continue; // already in state_changes
                         Json ej = EventJson(e);
                         ej["time"] = time;
                         events.push_back(ej);
                     }
                 }
                 Json states = Json::object();
                 for (const anim::AnimNode* m : machines) {
                     states[m->machine.empty() ? "node " + std::to_string(m->id) : m->machine] = {{"state", instance.CurrentState(m->id)}, {"blending", instance.InTransition(m->id)}};
                 }
                 Json row = {{"step", index}, {"time", time}, {"states", states}};
                 if (!changes.empty()) row["state_changes"] = changes;
                 if (!events.empty()) row["events"] = events;
                 if (!notes.empty()) row["notes"] = notes;
                 if (instance.IsPlayingMontage()) row["montage_section"] = instance.CurrentSection();
                 out.push_back(row);
                 ++index;
             }
             Json vars = Json::object();
             for (const anim::AnimVariable& v : graph.variables) vars[v.name] = instance.Get(v.name);
             return {{"steps", out}, {"variables", vars}, {"time", time}};
         }});
}

} // namespace aether::mcp
