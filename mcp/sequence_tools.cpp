#include "sequence_tools.h"

#include "project_host.h"

#include "aether/ecs/component.h"
#include "aether/reflection/reflection.h"
#include "aether/sequencer/sequence.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <set>
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

seq::LevelSequence ParseSequence(const std::string& text) {
    const Json parsed = Json::parse(text, nullptr, false);
    if (parsed.is_discarded()) throw ToolError("The sequence is not valid JSON");
    seq::LevelSequence sequence;
    std::string error;
    if (!seq::SequenceFromJson(parsed, sequence, &error)) throw ToolError("Not a valid sequence: " + error);
    return sequence;
}

// A sequence from `definition` (an object or its text) or `asset` (an .asequence in the open project).
seq::LevelSequence ReadSequence(AssetHost& host, const Json& args, std::string* from) {
    if (args.contains("definition")) {
        const Json& d = args["definition"];
        if (!d.is_object() && !d.is_string()) throw ToolError("\"definition\" must be a JSON object or a string of JSON");
        if (from != nullptr) *from = "definition";
        return ParseSequence(d.is_object() ? d.dump() : d.get<std::string>());
    }
    if (args.contains("asset")) {
        OpenProject& p = host.Require();
        const AssetRecord& r = FindAsset(p, RequireString(args, "asset"));
        if (r.importer != "Sequence") throw ToolError(r.path + " is not a level sequence (it is a " + r.importer + ")");
        if (from != nullptr) *from = r.path;
        return ParseSequence(ReadAll(p.database->SourcePath(r.guid)));
    }
    throw ToolError("Give \"definition\" (the sequence as JSON) or \"asset\" (a sequence in the open project)");
}

const char* TrackTypeName(seq::TrackType t) {
    switch (t) {
    case seq::TrackType::Transform: return "transform";
    case seq::TrackType::Property: return "property";
    case seq::TrackType::Event: return "event";
    case seq::TrackType::Visibility: return "visibility";
    case seq::TrackType::Spawn: return "spawn";
    case seq::TrackType::CameraCut: return "camera_cut";
    case seq::TrackType::Audio: return "audio";
    case seq::TrackType::Animation: return "animation";
    case seq::TrackType::Fade: return "fade";
    case seq::TrackType::Subsequence: return "subsequence";
    case seq::TrackType::Subtitle: return "subtitle";
    }
    return "transform";
}

// "SQ002: message" -> {code, message}.
Json DiagnosticJson(const std::string& line) {
    const usize colon = line.find(": ");
    if (colon != std::string::npos && colon <= 8) return {{"code", line.substr(0, colon)}, {"message", line.substr(colon + 2)}};
    return {{"code", ""}, {"message", line}};
}

Json QuatJson(const Quaternion& q) { return Json::array({q.x, q.y, q.z, q.w}); }

} // namespace

Json SequenceSampleJson() {
    using namespace seq;
    LevelSequence s;
    s.name = "Door opens";
    s.fps = 24.0f;
    s.duration = 4.0f;

    Track door;
    door.id = "door-move";
    door.name = "Door";
    door.type = TrackType::Transform;
    ParseEntityGuid("00000000-0000-4000-8000-000000000001", door.binding);
    Channel x, y, z;
    x.name = "x";
    y.name = "y";
    z.name = "z";
    y.keys = {{0.0f, 0.0f, Interp::Linear, 0, 0}, {2.0f, 3.0f, Interp::Linear, 0, 0}};
    door.channels = {x, y, z}; // a channel with no keys leaves that axis alone
    s.tracks.push_back(door);

    Track sfx;
    sfx.id = "creak";
    sfx.name = "Creak";
    sfx.type = TrackType::Event;
    sfx.events = {{1.0f, "door_creak", "loud"}};
    s.tracks.push_back(sfx);

    Track fade;
    fade.id = "fade";
    fade.name = "Fade in";
    fade.type = TrackType::Fade;
    Channel amount;
    amount.name = "amount";
    amount.keys = {{0.0f, 1.0f, Interp::Linear, 0, 0}, {1.0f, 0.0f, Interp::Linear, 0, 0}};
    fade.channels = {amount};
    s.tracks.push_back(fade);
    s.Normalize();
    return seq::SequenceToJson(s);
}

void RegisterSequenceTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"seq_validate",
         "Check a level sequence: the sequencer's own diagnostics (SQ001...: no tracks, keys out of order or outside the duration, a Property track with no field, "
         "duplicate track ids ...), plus a summary (effective duration, tracks by type). With a project open it also checks what the sequence names against it: "
         "a Property track's component and field must exist, a Spawn key's prefab, an Audio key's cue and a Subsequence key's sequence must be assets, and sequences "
         "must not play themselves in a loop. With `scene` (a scene asset) every track's bound entity must exist in it. Give the sequence as `definition` or `asset`.",
         Schema({{"definition", {{"description", "The sequence: a JSON object, or its text"}}},
                 {"asset", {{"type", "string"}, {"description", "A sequence asset in the open project"}}},
                 {"scene", {{"type", "string"}, {"description", "A scene asset whose entities the tracks must be bound to"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const seq::LevelSequence sequence = ReadSequence(*host, args, &from);
             Json diagnostics = Json::array();
             for (const std::string& line : seq::ValidateSequence(sequence)) diagnostics.push_back(DiagnosticJson(line));
             Json problems = Json::array();
             auto problem = [&](const std::string& track, const std::string& message) { problems.push_back({{"track", track}, {"problem", message}}); };

             // Property tracks: the component and field must exist (reflection).
             for (const seq::Track& t : sequence.tracks) {
                 if (t.type != seq::TrackType::Property || t.component.empty() || t.field.empty()) continue;
                 const ComponentId id = FindComponentIdByName(t.component);
                 if (id == kInvalidComponentId || id >= RegisteredComponentCount() || GetComponentInfo(id).reflected == nullptr) {
                     problem(t.id, "there is no component \"" + t.component + "\" (see list_component_types)");
                     continue;
                 }
                 bool found = false;
                 for (const reflect::FieldInfo& f : GetComponentInfo(id).reflected->fields) found = found || t.field == f.name;
                 if (!found) problem(t.id, "component " + t.component + " has no field \"" + t.field + "\"");
             }

             if (host->project != nullptr) {
                 OpenProject& p = *host->project;
                 std::set<std::string> cues, prefabs, sequences;
                 for (const AssetRecord* r : p.database->All()) {
                     if (r->IsSubAsset()) continue;
                     if (r->importer == "SoundCue") cues.insert(r->path);
                     else if (r->importer == "Prefab") prefabs.insert(r->path);
                     else if (r->importer == "Sequence") sequences.insert(r->path);
                 }
                 for (const seq::Track& t : sequence.tracks) {
                     for (const seq::AudioKey& k : t.audio) {
                         if (!k.cue.empty() && cues.count(k.cue) == 0) problem(t.id, "the audio cue \"" + k.cue + "\" is not a sound cue in the project");
                     }
                     for (const seq::SpawnKey& k : t.spawns) {
                         AssetGuid guid;
                         const bool is_guid = assets::ParseAssetGuid(k.prefab, guid) && p.database->Find(guid) != nullptr;
                         if (!k.prefab.empty() && !is_guid && prefabs.count(k.prefab) == 0) problem(t.id, "the prefab \"" + k.prefab + "\" is not a prefab in the project");
                     }
                     for (const seq::SubKey& k : t.subs) {
                         if (!k.sequence.empty() && sequences.count(k.sequence) == 0) problem(t.id, "the sub-sequence \"" + k.sequence + "\" is not a sequence in the project");
                     }
                 }
                 // A sequence that plays itself, directly or through others, never ends: follow the sub-sequence edges.
                 if (!from.empty() && from != "definition") {
                     std::map<std::string, std::vector<std::string>> edges;
                     for (const AssetRecord* r : p.database->All()) {
                         if (r->IsSubAsset() || r->importer != "Sequence") continue;
                         seq::LevelSequence other;
                         std::string error;
                         if (!seq::SequenceFromJson(Json::parse(ReadAll(p.database->SourcePath(r->guid)), nullptr, false), other, &error)) continue;
                         for (const seq::Track& t : other.tracks)
                             for (const seq::SubKey& k : t.subs) edges[r->path].push_back(k.sequence);
                     }
                     std::set<std::string> visiting, done;
                     std::function<bool(const std::string&)> cyclic = [&](const std::string& node) {
                         if (visiting.count(node)) return true;
                         if (done.count(node)) return false;
                         visiting.insert(node);
                         for (const std::string& next : edges[node])
                             if (cyclic(next)) return true;
                         visiting.erase(node);
                         done.insert(node);
                         return false;
                     };
                     if (cyclic(from)) problems.push_back({{"track", ""}, {"problem", "this sequence plays itself through its sub-sequences, so it can never finish"}});
                 }
                 if (args.contains("scene")) {
                     const AssetRecord& scene = FindAsset(p, RequireString(args, "scene"));
                     if (scene.importer != "Scene") throw ToolError(scene.path + " is not a scene");
                     std::set<std::string> guids;
                     const Json doc = Json::parse(ReadAll(p.database->SourcePath(scene.guid)), nullptr, false);
                     if (doc.is_object() && doc.contains("entities") && doc["entities"].is_array()) {
                         for (const Json& e : doc["entities"]) {
                             if (!e.is_object()) continue;
                             // A saved entity carries its GUID as a top-level "guid".
                             if (e.contains("guid") && e["guid"].is_string()) guids.insert(e["guid"].get<std::string>());
                             if (e.contains("components") && e["components"].is_object() && e["components"].contains("IdComponent") && e["components"]["IdComponent"].is_object()) {
                                 guids.insert(e["components"]["IdComponent"].value("guid", std::string()));
                             }
                         }
                     }
                     for (const seq::Track& t : sequence.tracks) {
                         if (t.binding.IsNull()) continue;
                         if (guids.count(ToString(t.binding)) == 0) problem(t.id, "bound to entity " + ToString(t.binding) + ", which is not in " + scene.path);
                     }
                 }
             } else if (args.contains("scene")) {
                 throw ToolError("\"scene\" needs a project: call project_open first");
             }

             Json types = Json::object();
             for (const seq::Track& t : sequence.tracks) types[TrackTypeName(t.type)] = types.value(TrackTypeName(t.type), 0) + 1;
             const bool ok = diagnostics.empty() && problems.empty();
             return {{"ok", ok}, {"sequence", from}, {"name", sequence.name}, {"fps", sequence.fps}, {"duration", sequence.EffectiveDuration()},
                     {"tracks", sequence.tracks.size()}, {"by_type", types}, {"diagnostics", diagnostics}, {"problems", problems}};
         }});

    server.AddTool(
        {"seq_evaluate",
         "What a sequence is doing at given times, without playing it: for each time, every unmuted track's value. Transform: position (axes that are keyed) and rotation; "
         "Property: each channel's value; Visibility: shown or hidden; Fade: amount and colour; Camera Cut: the camera in force; Spawn: the keys alive; Subsequence: the keys "
         "playing, with the child's own time. Event, Audio and Animation keys are moments, not values: see seq_events. Evaluation is a pure function of time, as in the player.",
         Schema({{"definition", {{"description", "The sequence: a JSON object, or its text"}}}, {"asset", {{"type", "string"}}},
                 {"times", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "Seconds (up to 100); default 0, a quarter, half, three quarters and the end"}}},
                 {"tracks", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Only these track ids"}}}}),
         [host](const Json& args) -> Json {
             std::string from;
             const seq::LevelSequence sequence = ReadSequence(*host, args, &from);
             const f32 duration = sequence.EffectiveDuration();
             std::vector<f32> times;
             if (args.contains("times")) {
                 if (!args["times"].is_array() || args["times"].empty() || args["times"].size() > 100) throw ToolError("\"times\" must be 1 to 100 numbers");
                 for (const Json& t : args["times"]) {
                     if (!t.is_number()) throw ToolError("\"times\" must be numbers");
                     times.push_back(t.get<f32>());
                 }
             } else {
                 for (f32 f : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) times.push_back(duration * f);
             }
             std::set<std::string> only;
             if (args.contains("tracks")) {
                 if (!args["tracks"].is_array()) throw ToolError("\"tracks\" must be an array of track ids");
                 for (const Json& t : args["tracks"]) {
                     if (!t.is_string()) throw ToolError("\"tracks\" must be an array of track ids");
                     only.insert(t.get<std::string>());
                 }
             }
             Json out = Json::array();
             for (const f32 time : times) {
                 Json tracks = Json::object();
                 for (const seq::Track& t : sequence.tracks) {
                     if (t.mute || (!only.empty() && only.count(t.id) == 0)) continue;
                     Json v = Json::object();
                     switch (t.type) {
                     case seq::TrackType::Transform: {
                         Json pos = Json::object();
                         const char* axes[] = {"x", "y", "z"};
                         for (usize i = 0; i < t.channels.size() && i < 3; ++i) {
                             if (!t.channels[i].Empty()) pos[axes[i]] = t.channels[i].Evaluate(time);
                         }
                         if (!pos.empty()) v["position"] = pos;
                         if (!t.rotation.empty()) v["rotation"] = QuatJson(seq::EvaluateRotation(t.rotation, time));
                         break;
                     }
                     case seq::TrackType::Property: {
                         for (const seq::Channel& c : t.channels) v[c.name] = c.Evaluate(time);
                         v["component"] = t.component;
                         v["field"] = t.field;
                         break;
                     }
                     case seq::TrackType::Visibility:
                         if (!t.channels.empty() && !t.channels[0].Empty()) v["visible"] = t.channels[0].Evaluate(time) >= 0.5f;
                         break;
                     case seq::TrackType::Fade:
                         if (!t.channels.empty()) v["amount"] = t.channels[0].Evaluate(time);
                         v["color"] = Json::array({t.fade_color.x, t.fade_color.y, t.fade_color.z, t.fade_color.w});
                         break;
                     case seq::TrackType::CameraCut: {
                         const seq::CutKey* active = nullptr;
                         for (const seq::CutKey& c : t.cuts)
                             if (c.time <= time && (active == nullptr || c.time >= active->time)) active = &c;
                         v["camera"] = active ? Json(ToString(active->camera)) : Json(nullptr);
                         break;
                     }
                     case seq::TrackType::Spawn: {
                         Json alive = Json::array();
                         for (usize i = 0; i < t.spawns.size(); ++i) {
                             const seq::SpawnKey& k = t.spawns[i];
                             if (time >= k.time && (k.duration <= 0.0f || time < k.time + k.duration)) alive.push_back({{"prefab", k.prefab}, {"key", i}});
                         }
                         v["alive"] = alive;
                         break;
                     }
                     case seq::TrackType::Subsequence: {
                         Json playing = Json::array();
                         for (usize i = 0; i < t.subs.size(); ++i) {
                             const seq::SubKey& k = t.subs[i];
                             if (time >= k.time && time < k.time + k.duration) playing.push_back({{"sequence", k.sequence}, {"child_time", (time - k.time) * k.scale + k.offset}, {"key", i}});
                         }
                         v["playing"] = playing;
                         break;
                     }
                     case seq::TrackType::Subtitle: {
                         const seq::SubtitleKey* shown = nullptr;
                         for (const seq::SubtitleKey& k : t.subtitles)
                             if (k.time <= time && time < k.time + k.duration && (shown == nullptr || k.time >= shown->time)) shown = &k;
                         v["text"] = shown ? Json(shown->text) : Json(nullptr);
                         break;
                     }
                     case seq::TrackType::Event:
                     case seq::TrackType::Audio:
                     case seq::TrackType::Animation: continue;
                     }
                     tracks[t.id] = v;
                 }
                 out.push_back({{"time", time}, {"tracks", tracks}});
             }
             return {{"sequence", from}, {"duration", duration}, {"samples", out}};
         }});

    server.AddTool(
        {"seq_events",
         "Everything a sequence fires or changes at a moment, in time order: Event keys (name, payload), Audio keys (cue, action, fade), Animation keys (montage, action, rate), "
         "camera cuts, prefabs spawning and despawning, sub-sequences starting and ending, and the times a fade starts and ends. `from` and `to` limit the window.",
         Schema({{"definition", {{"description", "The sequence: a JSON object, or its text"}}}, {"asset", {{"type", "string"}}}, {"from", {{"type", "number"}}}, {"to", {{"type", "number"}}}}),
         [host](const Json& args) -> Json {
             std::string from_name;
             const seq::LevelSequence sequence = ReadSequence(*host, args, &from_name);
             const f32 lo = args.contains("from") && args["from"].is_number() ? args["from"].get<f32>() : -1.0e30f;
             const f32 hi = args.contains("to") && args["to"].is_number() ? args["to"].get<f32>() : 1.0e30f;
             struct Moment {
                 f32 time;
                 Json what;
             };
             std::vector<Moment> moments;
             auto add = [&](f32 time, const seq::Track& t, const char* kind, Json detail) {
                 if (t.mute || time < lo || time > hi) return;
                 detail["kind"] = kind;
                 detail["track"] = t.id;
                 detail["time"] = time;
                 moments.push_back({time, std::move(detail)});
             };
             const char* audio_actions[] = {"play", "stop", "fade_in", "fade_out"};
             for (const seq::Track& t : sequence.tracks) {
                 for (const seq::EventKey& e : t.events) add(e.time, t, "event", {{"name", e.name}, {"payload", e.payload}});
                 for (const seq::AudioKey& a : t.audio) add(a.time, t, "audio", {{"cue", a.cue}, {"action", audio_actions[static_cast<usize>(a.action)]}, {"volume_db", a.volume_db}, {"fade", a.fade}});
                 for (const seq::AnimKey& a : t.anims) add(a.time, t, "animation", {{"montage", a.montage}, {"action", a.action == seq::AnimAction::Play ? "play" : "stop"}, {"rate", a.rate}});
                 for (const seq::CutKey& c : t.cuts) add(c.time, t, "camera_cut", {{"camera", ToString(c.camera)}});
                 for (const seq::SpawnKey& s : t.spawns) {
                     add(s.time, t, "spawn", {{"prefab", s.prefab}});
                     if (s.duration > 0.0f) add(s.time + s.duration, t, "despawn", {{"prefab", s.prefab}});
                 }
                 for (const seq::SubKey& s : t.subs) {
                     add(s.time, t, "subsequence_start", {{"sequence", s.sequence}});
                     add(s.time + s.duration, t, "subsequence_end", {{"sequence", s.sequence}});
                 }
                 if (t.type == seq::TrackType::Fade && !t.channels.empty() && !t.channels[0].keys.empty()) {
                     const std::vector<seq::Key>& keys = t.channels[0].keys;
                     for (const seq::Key& k : keys) add(k.time, t, "fade_key", {{"amount", k.value}});
                 }
             }
             std::stable_sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.time < b.time; });
             Json out = Json::array();
             for (Moment& m : moments) out.push_back(std::move(m.what));
             return {{"sequence", from_name}, {"duration", sequence.EffectiveDuration()}, {"count", out.size()}, {"moments", out}};
         }});
}

} // namespace aether::mcp
