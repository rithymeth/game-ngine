#include "aether/sequencer/sequence.h"
#include "aether/core/json_util.h"

#include "aether/platform/filesystem.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace aether::seq {

namespace {

const char* InterpName(Interp i) {
    switch (i) {
    case Interp::Constant: return "constant";
    case Interp::Linear: return "linear";
    case Interp::Bezier: return "bezier";
    }
    return "linear";
}

const char* AudioActionName(AudioAction a) {
    switch (a) {
    case AudioAction::Play: return "play";
    case AudioAction::Stop: return "stop";
    case AudioAction::FadeIn: return "fade_in";
    case AudioAction::FadeOut: return "fade_out";
    }
    return "play";
}
bool ParseAudioAction(const std::string& s, AudioAction& out) {
    if (s == "play") out = AudioAction::Play;
    else if (s == "stop") out = AudioAction::Stop;
    else if (s == "fade_in") out = AudioAction::FadeIn;
    else if (s == "fade_out") out = AudioAction::FadeOut;
    else return false;
    return true;
}

const char* TrackTypeName(TrackType t) {
    switch (t) {
    case TrackType::Transform: return "transform";
    case TrackType::Property: return "property";
    case TrackType::Event: return "event";
    case TrackType::Visibility: return "visibility";
    case TrackType::Spawn: return "spawn";
    case TrackType::CameraCut: return "cameracut";
    case TrackType::Audio: return "audio";
    case TrackType::Animation: return "animation";
    case TrackType::Fade: return "fade";
    case TrackType::Subsequence: return "subsequence";
    }
    return "transform";
}

bool ParseInterp(const std::string& s, Interp& out) {
    if (s == "constant") out = Interp::Constant;
    else if (s == "linear") out = Interp::Linear;
    else if (s == "bezier") out = Interp::Bezier;
    else return false;
    return true;
}

// The segment [k0, k1] at `time`, by the cubic Hermite basis for Bezier.
f32 Segment(const Key& k0, const Key& k1, f32 time) {
    const f32 dt = k1.time - k0.time;
    if (dt <= 0.0f) return k1.value;
    const f32 u = (time - k0.time) / dt;
    switch (k0.interp) {
    case Interp::Constant: return k0.value;
    case Interp::Linear: return k0.value + (k1.value - k0.value) * u;
    case Interp::Bezier: {
        const f32 u2 = u * u, u3 = u2 * u;
        const f32 h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        return h00 * k0.value + h10 * dt * k0.out_tangent + h01 * k1.value + h11 * dt * k1.in_tangent;
    }
    }
    return k0.value;
}

template <typename K>
bool StrictlyIncreasing(const std::vector<K>& keys) {
    for (usize i = 1; i < keys.size(); ++i) {
        if (!(keys[i].time > keys[i - 1].time)) return false;
    }
    return true;
}

template <typename K>
bool OutsideDuration(const std::vector<K>& keys, f32 duration) {
    for (const K& k : keys) {
        if (k.time < 0.0f || k.time > duration) return true;
    }
    return false;
}

} // namespace

void Channel::Normalize() {
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
}

f32 Channel::Evaluate(f32 time) const {
    if (keys.empty()) return 0.0f;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    // The last key at or before `time`.
    const auto it = std::upper_bound(keys.begin(), keys.end(), time, [](f32 t, const Key& k) { return t < k.time; });
    const Key& k1 = *it;
    const Key& k0 = *(it - 1);
    return Segment(k0, k1, time);
}

void Track::Normalize() {
    for (Channel& c : channels) c.Normalize();
    std::stable_sort(rotation.begin(), rotation.end(), [](const RotationKey& a, const RotationKey& b) { return a.time < b.time; });
    const auto by_time = [](const auto& a, const auto& b) { return a.time < b.time; };
    std::stable_sort(events.begin(), events.end(), by_time);
    std::stable_sort(spawns.begin(), spawns.end(), by_time);
    std::stable_sort(cuts.begin(), cuts.end(), by_time);
    std::stable_sort(audio.begin(), audio.end(), by_time);
    std::stable_sort(anims.begin(), anims.end(), by_time);
    std::stable_sort(subs.begin(), subs.end(), by_time);
}

void LevelSequence::Normalize() {
    for (Track& t : tracks) t.Normalize();
}

const Track* LevelSequence::FindTrack(const std::string& id) const {
    for (const Track& t : tracks) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

f32 LevelSequence::EffectiveDuration() const {
    f32 last = 0.0f;
    for (const Track& t : tracks) {
        for (const Channel& c : t.channels) {
            if (!c.keys.empty()) last = std::max(last, c.keys.back().time);
        }
        if (!t.rotation.empty()) last = std::max(last, t.rotation.back().time);
        if (!t.events.empty()) last = std::max(last, t.events.back().time);
        for (const SpawnKey& k : t.spawns) last = std::max(last, k.time + k.duration);
        if (!t.cuts.empty()) last = std::max(last, t.cuts.back().time);
        if (!t.audio.empty()) last = std::max(last, t.audio.back().time);
        if (!t.anims.empty()) last = std::max(last, t.anims.back().time);
        for (const SubKey& k : t.subs) last = std::max(last, k.time + k.duration);
    }
    return duration > 0.0f ? std::max(duration, last) : last;
}

Quaternion Slerp(const Quaternion& a, const Quaternion& b, f32 t) {
    f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Quaternion end = b;
    if (dot < 0.0f) { // the other way round the sphere is shorter
        dot = -dot;
        end = Quaternion(-b.x, -b.y, -b.z, -b.w);
    }
    f32 wa, wb;
    if (dot > 0.9995f) { // nearly the same: a straight blend is accurate and avoids dividing by ~0
        wa = 1.0f - t;
        wb = t;
    } else {
        const f32 theta = std::acos(std::min(dot, 1.0f));
        const f32 s = std::sin(theta);
        wa = std::sin((1.0f - t) * theta) / s;
        wb = std::sin(t * theta) / s;
    }
    return Quaternion(a.x * wa + end.x * wb, a.y * wa + end.y * wb, a.z * wa + end.z * wb, a.w * wa + end.w * wb).Normalized();
}

Quaternion EvaluateRotation(const std::vector<RotationKey>& keys, f32 time) {
    if (keys.empty()) return Quaternion::Identity();
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    const auto it = std::upper_bound(keys.begin(), keys.end(), time, [](f32 t, const RotationKey& k) { return t < k.time; });
    const RotationKey& k1 = *it;
    const RotationKey& k0 = *(it - 1);
    if (k0.interp == Interp::Constant) return k0.value;
    const f32 dt = k1.time - k0.time;
    return Slerp(k0.value, k1.value, dt > 0.0f ? (time - k0.time) / dt : 1.0f);
}

std::vector<std::string> ValidateSequence(const LevelSequence& s) {
    std::vector<std::string> out;
    const auto add = [&](const char* code, const std::string& message) { out.push_back(std::string(code) + ": " + message); };
    if (s.tracks.empty()) add("SQ001", "the sequence has no tracks");
    if (s.duration < 0.0f || !(s.fps > 0.0f)) add("SQ006", "the duration is negative or the frame rate isn't above 0");
    std::set<std::string> ids;
    for (const Track& t : s.tracks) {
        const std::string label = "track '" + (t.id.empty() ? t.name : t.id) + "'";
        if (t.id.empty()) add("SQ007", label + " has no id");
        else if (!ids.insert(t.id).second) add("SQ005", label + " repeats an id");
        const bool optional_binding = t.type == TrackType::Event || t.type == TrackType::Spawn || t.type == TrackType::CameraCut || t.type == TrackType::Audio ||
                                      t.type == TrackType::Fade || t.type == TrackType::Subsequence;
        if (t.binding.IsNull() && !optional_binding) add("SQ008", label + " is bound to no entity");

        // Keys on a track of the wrong kind.
        const auto wrong = [&](bool has, TrackType owner, const char* what) {
            if (has && t.type != owner) add("SQ013", label + " has " + what + " keys but isn't that kind of track");
        };
        wrong(!t.events.empty(), TrackType::Event, "event");
        wrong(!t.rotation.empty(), TrackType::Transform, "rotation");
        wrong(!t.spawns.empty(), TrackType::Spawn, "spawn");
        wrong(!t.cuts.empty(), TrackType::CameraCut, "camera cut");
        wrong(!t.audio.empty(), TrackType::Audio, "audio");
        wrong(!t.anims.empty(), TrackType::Animation, "animation");
        wrong(!t.subs.empty(), TrackType::Subsequence, "subsequence");

        const auto ordered = [&](const auto& keys, const char* what) {
            if (!StrictlyIncreasing(keys)) add("SQ002", label + ", " + what + ": keys aren't in increasing time order");
            if (s.duration > 0.0f && OutsideDuration(keys, s.duration)) add("SQ003", label + ", " + what + ": a key is outside the duration");
        };
        const bool key_list_track = t.type == TrackType::Event || t.type == TrackType::Spawn || t.type == TrackType::CameraCut ||
                                    t.type == TrackType::Audio || t.type == TrackType::Animation || t.type == TrackType::Subsequence;
        if (key_list_track && !t.channels.empty()) add("SQ013", label + " has value channels but isn't a value track");

        switch (t.type) {
        case TrackType::Transform:
            if (t.channels.size() != 3) add("SQ009", label + " needs three position channels (x, y, z)");
            break;
        case TrackType::Event:
            for (const EventKey& e : t.events) {
                if (e.name.empty()) add("SQ011", label + " has an event key with no name");
            }
            ordered(t.events, "events");
            break;
        case TrackType::Visibility: {
            bool bad = t.channels.size() != 1;
            if (!bad) {
                for (const Key& k : t.channels[0].keys) bad = bad || (k.value != 0.0f && k.value != 1.0f);
            }
            if (bad) add("SQ012", label + " needs exactly one channel of 0 (hidden) and 1 (shown) keys");
            break;
        }
        case TrackType::Spawn:
            for (const SpawnKey& k : t.spawns) {
                if (k.prefab.empty()) add("SQ014", label + " has a spawn key with no prefab");
                if (k.duration < 0.0f) add("SQ015", label + " has a spawn key with a negative duration");
            }
            ordered(t.spawns, "spawns");
            break;
        case TrackType::CameraCut:
            for (const CutKey& k : t.cuts) {
                if (k.camera.IsNull()) add("SQ016", label + " has a cut to no camera");
            }
            ordered(t.cuts, "cuts");
            break;
        case TrackType::Audio:
            for (const AudioKey& k : t.audio) {
                if (k.cue.empty()) add("SQ017", label + " has an audio key with no cue");
            }
            ordered(t.audio, "audio");
            break;
        case TrackType::Animation:
            for (const AnimKey& k : t.anims) {
                if (k.montage.empty()) add("SQ018", label + " has an animation key with no montage");
            }
            ordered(t.anims, "animations");
            break;
        case TrackType::Fade: {
            bool bad = false;
            if (t.channels.size() != 1) add("SQ019", label + " needs exactly one channel of fade amounts");
            else {
                for (const Key& k : t.channels[0].keys) bad = bad || k.value < 0.0f || k.value > 1.0f;
            }
            if (bad) add("SQ020", label + " has a fade amount outside 0 to 1");
            break;
        }
        case TrackType::Subsequence:
            for (const SubKey& k : t.subs) {
                if (k.sequence.empty()) add("SQ021", label + " has a subsequence key with no sequence");
                if (!(k.duration > 0.0f)) add("SQ022", label + " has a subsequence key whose duration isn't above 0");
                if (!(k.scale > 0.0f)) add("SQ023", label + " has a subsequence key whose scale isn't above 0");
            }
            ordered(t.subs, "subsequences");
            break;
        case TrackType::Property:
            if (t.component.empty() || t.field.empty()) add("SQ004", label + " names no component or field");
            if (t.channels.empty()) add("SQ010", label + " has no channels");
            break;
        }
        for (const Channel& c : t.channels) {
            if (!StrictlyIncreasing(c.keys)) add("SQ002", label + ", channel '" + c.name + "': keys aren't in increasing time order");
            if (s.duration > 0.0f && OutsideDuration(c.keys, s.duration)) add("SQ003", label + ", channel '" + c.name + "': a key is outside the duration");
        }
        ordered(t.rotation, "rotation");
    }
    return out;
}

nlohmann::json SequenceToJson(const LevelSequence& s) {
    nlohmann::json tracks = nlohmann::json::array();
    for (const Track& t : s.tracks) {
        nlohmann::json channels = nlohmann::json::array();
        for (const Channel& c : t.channels) {
            nlohmann::json keys = nlohmann::json::array();
            for (const Key& k : c.keys) {
                nlohmann::json key = {{"time", k.time}, {"value", k.value}, {"interp", InterpName(k.interp)}};
                if (k.interp == Interp::Bezier) {
                    key["in"] = k.in_tangent;
                    key["out"] = k.out_tangent;
                }
                keys.push_back(std::move(key));
            }
            channels.push_back({{"name", c.name}, {"keys", std::move(keys)}});
        }
        nlohmann::json track = {{"id", t.id},
                                {"name", t.name},
                                {"type", TrackTypeName(t.type)},
                                {"binding", ToString(t.binding)},
                                {"mute", t.mute},
                                {"locked", t.locked},
                                {"channels", std::move(channels)}};
        if (t.type == TrackType::Property) {
            track["component"] = t.component;
            track["field"] = t.field;
        } else if (t.type == TrackType::Event) {
            nlohmann::json events = nlohmann::json::array();
            for (const EventKey& e : t.events) events.push_back({{"time", e.time}, {"name", e.name}, {"payload", e.payload}});
            track["events"] = std::move(events);
        } else if (t.type == TrackType::Spawn) {
            nlohmann::json spawns = nlohmann::json::array();
            for (const SpawnKey& k : t.spawns) {
                spawns.push_back({{"time", k.time}, {"duration", k.duration}, {"prefab", k.prefab},
                                  {"position", {k.position.x, k.position.y, k.position.z}},
                                  {"rotation", {k.rotation.x, k.rotation.y, k.rotation.z, k.rotation.w}}});
            }
            track["spawns"] = std::move(spawns);
        } else if (t.type == TrackType::CameraCut) {
            nlohmann::json cuts = nlohmann::json::array();
            for (const CutKey& k : t.cuts) cuts.push_back({{"time", k.time}, {"camera", ToString(k.camera)}});
            track["cuts"] = std::move(cuts);
        } else if (t.type == TrackType::Audio) {
            nlohmann::json keys = nlohmann::json::array();
            for (const AudioKey& k : t.audio) {
                keys.push_back({{"time", k.time}, {"cue", k.cue}, {"action", AudioActionName(k.action)}, {"volume_db", k.volume_db}, {"fade", k.fade}});
            }
            track["audio"] = std::move(keys);
        } else if (t.type == TrackType::Fade) {
            track["fade_color"] = {t.fade_color.x, t.fade_color.y, t.fade_color.z, t.fade_color.w};
        } else if (t.type == TrackType::Subsequence) {
            nlohmann::json subs = nlohmann::json::array();
            for (const SubKey& k : t.subs) {
                subs.push_back({{"time", k.time}, {"duration", k.duration}, {"sequence", k.sequence}, {"offset", k.offset}, {"scale", k.scale}});
            }
            track["subs"] = std::move(subs);
        } else if (t.type == TrackType::Animation) {
            nlohmann::json keys = nlohmann::json::array();
            for (const AnimKey& k : t.anims) {
                keys.push_back({{"time", k.time}, {"montage", k.montage}, {"action", k.action == AnimAction::Play ? "play" : "stop"}, {"rate", k.rate}});
            }
            track["animation"] = std::move(keys);
        } else if (t.type == TrackType::Transform) {
            nlohmann::json rotation = nlohmann::json::array();
            for (const RotationKey& r : t.rotation) {
                rotation.push_back({{"time", r.time}, {"value", {r.value.x, r.value.y, r.value.z, r.value.w}}, {"interp", InterpName(r.interp)}});
            }
            track["rotation"] = std::move(rotation);
        }
        tracks.push_back(std::move(track));
    }
    return {{"$type", "LevelSequence"}, {"$version", LevelSequence::kVersion}, {"name", s.name},
            {"fps", s.fps},             {"duration", s.duration},              {"tracks", std::move(tracks)}};
}

static bool SequenceFromJsonImpl(const nlohmann::json& j, LevelSequence& out, std::string* error);

bool SequenceFromJson(const nlohmann::json& j, LevelSequence& out, std::string* error) {
    return GuardJsonLoader(error, "sequence", [&] { return SequenceFromJsonImpl(j, out, error); });
}

static bool SequenceFromJsonImpl(const nlohmann::json& j, LevelSequence& out, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "LevelSequence") return fail("not a level sequence");
    const u32 version = static_cast<u32>(JsonVersion(j));
    if (version == 0 || version > LevelSequence::kVersion) return fail("sequence version " + std::to_string(version) + " isn't supported");
    LevelSequence s;
    s.name = j.value("name", "");
    s.fps = j.value("fps", 24.0f);
    s.duration = j.value("duration", 0.0f);
    if (!j.contains("tracks") || !j["tracks"].is_array()) return fail("the sequence has no track list");
    for (const nlohmann::json& tj : j["tracks"]) {
        Track t;
        t.id = tj.value("id", "");
        t.name = tj.value("name", "");
        const std::string type = tj.value("type", "");
        if (type == "transform") t.type = TrackType::Transform;
        else if (type == "property") t.type = TrackType::Property;
        else if (type == "event") t.type = TrackType::Event;
        else if (type == "visibility") t.type = TrackType::Visibility;
        else if (type == "spawn") t.type = TrackType::Spawn;
        else if (type == "cameracut") t.type = TrackType::CameraCut;
        else if (type == "audio") t.type = TrackType::Audio;
        else if (type == "animation") t.type = TrackType::Animation;
        else if (type == "fade") t.type = TrackType::Fade;
        else if (type == "subsequence") t.type = TrackType::Subsequence;
        else return fail("track '" + t.id + "' has an unknown type '" + type + "'");
        const bool optional_binding = t.type != TrackType::Transform && t.type != TrackType::Property && t.type != TrackType::Visibility &&
                                      t.type != TrackType::Animation && tj.value("binding", "").empty();
        if (t.type == TrackType::Fade && tj.contains("fade_color")) {
            const nlohmann::json& c = tj["fade_color"];
            if (!c.is_array() || c.size() != 4) return fail("track '" + t.id + "': a fade colour needs four numbers");
            t.fade_color = Vec4(c[0].get<f32>(), c[1].get<f32>(), c[2].get<f32>(), c[3].get<f32>());
        }
        if (!optional_binding && !ParseEntityGuid(tj.value("binding", ""), t.binding)) return fail("track '" + t.id + "' has a bad entity GUID");
        t.mute = tj.value("mute", false);
        t.locked = tj.value("locked", false);
        t.component = tj.value("component", "");
        t.field = tj.value("field", "");
        for (const nlohmann::json& cj : tj.value("channels", nlohmann::json::array())) {
            Channel c;
            c.name = cj.value("name", "");
            for (const nlohmann::json& kj : cj.value("keys", nlohmann::json::array())) {
                if (!kj.contains("time") || !kj.contains("value")) return fail("track '" + t.id + "': a key needs a time and a value");
                Key k;
                k.time = kj["time"].get<f32>();
                k.value = kj["value"].get<f32>();
                if (!ParseInterp(kj.value("interp", "linear"), k.interp)) return fail("track '" + t.id + "': unknown interpolation '" + kj.value("interp", "") + "'");
                k.in_tangent = kj.value("in", 0.0f);
                k.out_tangent = kj.value("out", 0.0f);
                c.keys.push_back(k);
            }
            t.channels.push_back(std::move(c));
        }
        for (const nlohmann::json& rj : tj.value("rotation", nlohmann::json::array())) {
            if (!rj.contains("time") || !rj.contains("value") || !rj["value"].is_array() || rj["value"].size() != 4) {
                return fail("track '" + t.id + "': a rotation key needs a time and a four-number value");
            }
            RotationKey r;
            r.time = rj["time"].get<f32>();
            r.value = Quaternion(rj["value"][0].get<f32>(), rj["value"][1].get<f32>(), rj["value"][2].get<f32>(), rj["value"][3].get<f32>());
            if (!ParseInterp(rj.value("interp", "linear"), r.interp)) return fail("track '" + t.id + "': unknown interpolation");
            t.rotation.push_back(r);
        }
        for (const nlohmann::json& ej : tj.value("events", nlohmann::json::array())) {
            if (!ej.contains("time") || !ej["time"].is_number()) return fail("track '" + t.id + "': an event key needs a time");
            EventKey e;
            e.time = ej["time"].get<f32>();
            e.name = ej.value("name", "");
            e.payload = ej.value("payload", "");
            t.events.push_back(std::move(e));
        }
        const auto vec3 = [](const nlohmann::json& j, Vec3& v) {
            if (!j.is_array() || j.size() != 3) return false;
            v = Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
            return true;
        };
        for (const nlohmann::json& kj : tj.value("spawns", nlohmann::json::array())) {
            if (!kj.contains("time") || !kj["time"].is_number()) return fail("track '" + t.id + "': a spawn key needs a time");
            SpawnKey k;
            k.time = kj["time"].get<f32>();
            k.duration = kj.value("duration", 0.0f);
            k.prefab = kj.value("prefab", "");
            if (kj.contains("position") && !vec3(kj["position"], k.position)) return fail("track '" + t.id + "': a spawn position needs three numbers");
            if (kj.contains("rotation")) {
                const nlohmann::json& r = kj["rotation"];
                if (!r.is_array() || r.size() != 4) return fail("track '" + t.id + "': a spawn rotation needs four numbers");
                k.rotation = Quaternion(r[0].get<f32>(), r[1].get<f32>(), r[2].get<f32>(), r[3].get<f32>());
            }
            t.spawns.push_back(std::move(k));
        }
        for (const nlohmann::json& kj : tj.value("cuts", nlohmann::json::array())) {
            if (!kj.contains("time") || !kj["time"].is_number()) return fail("track '" + t.id + "': a cut needs a time");
            CutKey k;
            k.time = kj["time"].get<f32>();
            if (!ParseEntityGuid(kj.value("camera", ""), k.camera)) return fail("track '" + t.id + "': a cut has a bad camera GUID");
            t.cuts.push_back(k);
        }
        for (const nlohmann::json& kj : tj.value("audio", nlohmann::json::array())) {
            if (!kj.contains("time") || !kj["time"].is_number()) return fail("track '" + t.id + "': an audio key needs a time");
            AudioKey k;
            k.time = kj["time"].get<f32>();
            k.cue = kj.value("cue", "");
            if (!ParseAudioAction(kj.value("action", "play"), k.action)) return fail("track '" + t.id + "': unknown audio action '" + kj.value("action", "") + "'");
            k.volume_db = kj.value("volume_db", 0.0f);
            k.fade = kj.value("fade", 0.0f);
            t.audio.push_back(std::move(k));
        }
        for (const nlohmann::json& kj : tj.value("animation", nlohmann::json::array())) {
            if (!kj.contains("time") || !kj["time"].is_number()) return fail("track '" + t.id + "': an animation key needs a time");
            AnimKey k;
            k.time = kj["time"].get<f32>();
            k.montage = kj.value("montage", "");
            const std::string action = kj.value("action", "play");
            if (action == "play") k.action = AnimAction::Play;
            else if (action == "stop") k.action = AnimAction::Stop;
            else return fail("track '" + t.id + "': unknown animation action '" + action + "'");
            k.rate = kj.value("rate", 1.0f);
            t.anims.push_back(std::move(k));
        }
        for (const nlohmann::json& kj : tj.value("subs", nlohmann::json::array())) {
            if (!kj.contains("time") || !kj["time"].is_number() || !kj.contains("duration") || !kj["duration"].is_number()) {
                return fail("track '" + t.id + "': a subsequence key needs a time and a duration");
            }
            SubKey k;
            k.time = kj["time"].get<f32>();
            k.duration = kj["duration"].get<f32>();
            k.sequence = kj.value("sequence", "");
            k.offset = kj.value("offset", 0.0f);
            k.scale = kj.value("scale", 1.0f);
            t.subs.push_back(std::move(k));
        }
        s.tracks.push_back(std::move(t));
    }
    out = std::move(s);
    return true;
}

bool LoadSequence(const std::string& file, LevelSequence& out, std::string* error) {
    std::string text;
    if (!fs::ReadFileText(file, text)) {
        if (error) *error = "Couldn't read " + file;
        return false;
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (error) *error = file + " isn't valid JSON";
        return false;
    }
    return SequenceFromJson(j, out, error);
}

bool SaveSequence(const std::string& file, const LevelSequence& sequence, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        if (error) *error = "Couldn't write " + file;
        return false;
    }
    out << SequenceToJson(sequence).dump(2);
    return true;
}

} // namespace aether::seq
