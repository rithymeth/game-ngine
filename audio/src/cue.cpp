#include "aether/audio/cue.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace aether::audio {

namespace {

constexpr f64 kForever = std::numeric_limits<f64>::infinity();
constexpr usize kMaxItems = 4096; // a runaway Loop can't flood the mixer
constexpr u32 kMaxLoopCount = 1000;
constexpr int kMaxDepth = 64;

constexpr const char* kTypeNames[] = {"Wave", "Random", "Sequence", "Modulator", "Concatenator", "Loop", "Mix", "Delay"};
constexpr const char* kModelNames[] = {"None", "Inverse", "Linear", "Logarithmic", "Custom"};
constexpr const char* kVirtualNames[] = {"Continue", "Restart", "Stop"};

template <usize N>
int IndexOf(const char* const (&names)[N], const std::string& s) {
    for (usize i = 0; i < N; ++i) {
        if (s == names[i]) return static_cast<int>(i);
    }
    return -1;
}

// Children a node type takes: {min, max}.
std::pair<usize, usize> ChildRange(CueNodeType t) {
    switch (t) {
    case CueNodeType::Wave: return {0, 0};
    case CueNodeType::Modulator:
    case CueNodeType::Loop:
    case CueNodeType::Delay: return {1, 1};
    default: return {1, std::numeric_limits<usize>::max()};
    }
}

} // namespace

const char* CueNodeTypeName(CueNodeType t) { return kTypeNames[static_cast<usize>(t)]; }

bool ParseCueNodeType(const std::string& name, CueNodeType& out) {
    const int i = IndexOf(kTypeNames, name);
    if (i < 0) return false;
    out = static_cast<CueNodeType>(i);
    return true;
}

const CueNode* SoundCue::Find(u32 id) const {
    for (const CueNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}
CueNode* SoundCue::Find(u32 id) { return const_cast<CueNode*>(static_cast<const SoundCue&>(*this).Find(id)); }

u32 SoundCue::NextId() const {
    u32 next = 1;
    for (const CueNode& n : nodes) next = std::max(next, n.id + 1);
    return next;
}

// --- Validation -------------------------------------------------------------------------------

namespace {

// True when the node never finishes, whatever the random choices.
bool AlwaysEndless(const SoundCue& cue, u32 id, int depth) {
    const CueNode* n = cue.Find(id);
    if (n == nullptr || depth > kMaxDepth) return false;
    auto child = [&](usize i) { return i < n->children.size() && AlwaysEndless(cue, n->children[i], depth + 1); };
    switch (n->type) {
    case CueNodeType::Wave: return n->looping;
    case CueNodeType::Loop: return n->count == 0 || child(0);
    case CueNodeType::Modulator:
    case CueNodeType::Delay: return child(0);
    case CueNodeType::Concatenator:
    case CueNodeType::Mix:
        for (usize i = 0; i < n->children.size(); ++i) {
            if (child(i)) return true;
        }
        return false;
    case CueNodeType::Random:
    case CueNodeType::Sequence:
        for (usize i = 0; i < n->children.size(); ++i) {
            if (!child(i)) return false;
        }
        return !n->children.empty();
    }
    return false;
}

} // namespace

std::vector<CueDiagnostic> ValidateCue(const SoundCue& cue, const std::function<bool(const std::string&)>& sound_exists) {
    std::vector<CueDiagnostic> out;
    auto add = [&](const char* code, const std::string& message, u32 node, bool error = true) { out.push_back({code, message, node, error}); };
    auto label = [](const CueNode& n) { return std::string(CueNodeTypeName(n.type)) + " node " + std::to_string(n.id); };

    if (cue.Find(cue.root) == nullptr) add("CU001", "The cue has no output: connect a node to it.", cue.root);
    std::set<u32> seen;
    for (const CueNode& n : cue.nodes) {
        if (!seen.insert(n.id).second) add("CU002", "More than one node has id " + std::to_string(n.id) + ".", n.id);
    }
    if (!(cue.pitch > 0.0f)) add("CU008", "The cue's pitch must be above 0.", 0);
    for (const CueNode& n : cue.nodes) {
        for (u32 c : n.children) {
            if (cue.Find(c) == nullptr) add("CU003", label(n) + " is connected to node " + std::to_string(c) + ", which doesn't exist.", n.id);
        }
        const auto [lo, hi] = ChildRange(n.type);
        if (n.children.size() < lo || n.children.size() > hi) {
            const std::string want = hi == 0 ? "no inputs" : hi == 1 ? "exactly one input" : "at least one input";
            add("CU005", label(n) + " needs " + want + " (it has " + std::to_string(n.children.size()) + ").", n.id);
        }
        switch (n.type) {
        case CueNodeType::Wave:
            if (n.sound.empty()) {
                add("CU006", label(n) + " has no sound.", n.id);
            } else if (sound_exists && !sound_exists(n.sound)) {
                add("CU007", label(n) + " plays '" + n.sound + "', which doesn't exist.", n.id);
            }
            break;
        case CueNodeType::Modulator:
            if (n.volume_min_db > n.volume_max_db) add("CU008", label(n) + ": the volume range's minimum is above its maximum.", n.id);
            if (n.pitch_min > n.pitch_max) add("CU008", label(n) + ": the pitch range's minimum is above its maximum.", n.id);
            if (!(n.pitch_min > 0.0f)) add("CU008", label(n) + ": pitch must be above 0.", n.id);
            break;
        case CueNodeType::Delay:
            if (n.delay_min > n.delay_max || n.delay_min < 0.0f) add("CU008", label(n) + ": the delay range must be 0 or more, minimum first.", n.id);
            break;
        case CueNodeType::Random: {
            f32 total = 0.0f;
            bool negative = false;
            for (usize i = 0; i < n.children.size(); ++i) {
                const f32 w = i < n.weights.size() ? n.weights[i] : 1.0f;
                negative |= w < 0.0f;
                total += std::max(w, 0.0f);
            }
            if (negative) add("CU009", label(n) + " has a negative weight.", n.id);
            else if (!n.children.empty() && total <= 0.0f) add("CU009", label(n) + "'s weights are all 0, so nothing can play.", n.id);
            break;
        }
        case CueNodeType::Concatenator:
            for (usize i = 0; i + 1 < n.children.size(); ++i) {
                if (AlwaysEndless(cue, n.children[i], 0)) {
                    add("CU010", label(n) + ": input " + std::to_string(i + 1) + " never ends, so the ones after it never play.", n.id, false);
                    break;
                }
            }
            break;
        default: break;
        }
    }
    // Cycles: a depth-first walk with a colour per node.
    std::map<u32, int> colour; // 0 new, 1 on the path, 2 done
    std::function<bool(u32)> visit = [&](u32 id) {
        colour[id] = 1;
        if (const CueNode* n = cue.Find(id)) {
            for (u32 c : n->children) {
                if (cue.Find(c) == nullptr) continue;
                if (colour[c] == 1) return true;
                if (colour[c] == 0 && visit(c)) return true;
            }
        }
        colour[id] = 2;
        return false;
    };
    for (const CueNode& n : cue.nodes) {
        if (colour[n.id] == 0 && visit(n.id)) {
            add("CU004", "The cue's nodes form a loop through node " + std::to_string(n.id) + "; use a Loop node to repeat.", n.id);
            break;
        }
    }
    // Nodes the output never reaches.
    std::set<u32> reached;
    std::vector<u32> stack{cue.root};
    while (!stack.empty()) {
        const u32 id = stack.back();
        stack.pop_back();
        const CueNode* n = cue.Find(id);
        if (n == nullptr || !reached.insert(id).second) continue;
        stack.insert(stack.end(), n->children.begin(), n->children.end());
    }
    for (const CueNode& n : cue.nodes) {
        if (reached.count(n.id) == 0) add("CU011", label(n) + " isn't connected to the output.", n.id, false);
    }
    return out;
}

// --- JSON ---------------------------------------------------------------------------------------

nlohmann::json CueToJson(const SoundCue& cue) {
    using nlohmann::json;
    json att = {{"model", kModelNames[static_cast<usize>(cue.attenuation.model)]},
                {"min_distance", cue.attenuation.min_distance},
                {"max_distance", cue.attenuation.max_distance},
                {"rolloff", cue.attenuation.rolloff},
                {"lowpass_at_max_hz", cue.attenuation.lowpass_at_max_hz}};
    if (!cue.attenuation.curve.empty()) {
        json curve = json::array();
        for (const auto& [x, y] : cue.attenuation.curve) curve.push_back({x, y});
        att["curve"] = curve;
    }
    json output = {{"volume_db", cue.volume_db},
                   {"pitch", cue.pitch},
                   {"bus", cue.bus},
                   {"priority", cue.priority},
                   {"virtual", kVirtualNames[static_cast<usize>(cue.virtual_mode)]},
                   {"spatial", cue.spatial},
                   {"spatial_blend", cue.spatial_blend},
                   {"attenuation", att},
                   {"doppler", cue.doppler},
                   {"occlusion", cue.occlusion}};
    json nodes = json::array();
    for (const CueNode& n : cue.nodes) {
        json j = {{"id", n.id}, {"type", CueNodeTypeName(n.type)}, {"pos", {n.x, n.y}}};
        if (!n.children.empty()) j["children"] = n.children;
        switch (n.type) {
        case CueNodeType::Wave:
            j["sound"] = n.sound;
            if (n.looping) j["looping"] = true;
            break;
        case CueNodeType::Random:
            if (!n.weights.empty()) j["weights"] = n.weights;
            if (n.no_repeat) j["no_repeat"] = true;
            break;
        case CueNodeType::Modulator:
            j["volume_db"] = {n.volume_min_db, n.volume_max_db};
            j["pitch"] = {n.pitch_min, n.pitch_max};
            break;
        case CueNodeType::Loop: j["count"] = n.count; break;
        case CueNodeType::Mix:
            if (!n.input_db.empty()) j["input_db"] = n.input_db;
            break;
        case CueNodeType::Delay: j["delay"] = {n.delay_min, n.delay_max}; break;
        default: break;
        }
        nodes.push_back(std::move(j));
    }
    return {{"version", 1}, {"name", cue.name}, {"root", cue.root}, {"output", output}, {"nodes", nodes}};
}

bool CueFromJson(const nlohmann::json& j, SoundCue& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    try {
        if (!j.is_object()) return fail("a sound cue is a JSON object");
        if (j.value("version", 1) > 1) return fail("this sound cue was saved by a newer version (" + std::to_string(j.value("version", 1)) + ")");
        SoundCue cue;
        cue.name = j.value("name", std::string());
        cue.root = j.value("root", 0u);
        if (j.contains("output")) {
            const auto& o = j.at("output");
            cue.volume_db = o.value("volume_db", 0.0f);
            cue.pitch = o.value("pitch", 1.0f);
            cue.bus = o.value("bus", std::string("SFX"));
            cue.priority = static_cast<u8>(std::clamp(o.value("priority", 128), 0, 255));
            const int vm = IndexOf(kVirtualNames, o.value("virtual", std::string("Continue")));
            if (vm < 0) return fail("unknown virtual mode '" + o.value("virtual", std::string()) + "'");
            cue.virtual_mode = static_cast<VirtualMode>(vm);
            cue.spatial = o.value("spatial", false);
            cue.spatial_blend = o.value("spatial_blend", 1.0f);
            cue.doppler = o.value("doppler", 1.0f);
            cue.occlusion = o.value("occlusion", false);
            if (o.contains("attenuation")) {
                const auto& a = o.at("attenuation");
                const int m = IndexOf(kModelNames, a.value("model", std::string("Inverse")));
                if (m < 0) return fail("unknown attenuation model '" + a.value("model", std::string()) + "'");
                cue.attenuation.model = static_cast<AttenuationModel>(m);
                cue.attenuation.min_distance = a.value("min_distance", 1.0f);
                cue.attenuation.max_distance = a.value("max_distance", 50.0f);
                cue.attenuation.rolloff = a.value("rolloff", 1.0f);
                cue.attenuation.lowpass_at_max_hz = a.value("lowpass_at_max_hz", 0.0f);
                if (a.contains("curve")) {
                    for (const auto& p : a.at("curve")) cue.attenuation.curve.emplace_back(p.at(0).get<f32>(), p.at(1).get<f32>());
                }
            }
        }
        if (j.contains("nodes")) {
            if (!j.at("nodes").is_array()) return fail("'nodes' must be a list");
            for (const auto& nj : j.at("nodes")) {
                CueNode n;
                if (!nj.contains("id")) return fail("a node has no id");
                n.id = nj.at("id").get<u32>();
                const std::string type = nj.value("type", std::string());
                if (!ParseCueNodeType(type, n.type)) return fail("node " + std::to_string(n.id) + " has an unknown type '" + type + "'");
                if (nj.contains("pos")) n.x = nj.at("pos").at(0).get<f32>(), n.y = nj.at("pos").at(1).get<f32>();
                if (nj.contains("children")) n.children = nj.at("children").get<std::vector<u32>>();
                n.sound = nj.value("sound", std::string());
                n.looping = nj.value("looping", false);
                if (nj.contains("weights")) n.weights = nj.at("weights").get<std::vector<f32>>();
                n.no_repeat = nj.value("no_repeat", false);
                if (nj.contains("volume_db")) n.volume_min_db = nj.at("volume_db").at(0).get<f32>(), n.volume_max_db = nj.at("volume_db").at(1).get<f32>();
                if (nj.contains("pitch")) n.pitch_min = nj.at("pitch").at(0).get<f32>(), n.pitch_max = nj.at("pitch").at(1).get<f32>();
                n.count = nj.value("count", 0u);
                if (nj.contains("input_db")) n.input_db = nj.at("input_db").get<std::vector<f32>>();
                if (nj.contains("delay")) n.delay_min = nj.at("delay").at(0).get<f32>(), n.delay_max = nj.at("delay").at(1).get<f32>();
                cue.nodes.push_back(std::move(n));
            }
        }
        out = std::move(cue);
        return true;
    } catch (const nlohmann::json::exception& e) {
        return fail(std::string("malformed sound cue: ") + e.what());
    }
}

std::string SaveCue(const SoundCue& cue) { return CueToJson(cue).dump(2); }

bool LoadCue(const std::string& text, SoundCue& out, std::string* error) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (error != nullptr) *error = "the sound cue isn't valid JSON";
        return false;
    }
    return CueFromJson(j, out, error);
}

// --- Evaluation -------------------------------------------------------------------------------

f32 CueState::Uniform(f32 min, f32 max) {
    // xorshift64*
    rng_ ^= rng_ >> 12;
    rng_ ^= rng_ << 25;
    rng_ ^= rng_ >> 27;
    const u64 r = rng_ * 0x2545F4914F6CDD1Dull;
    const f32 u = static_cast<f32>(static_cast<f64>(r >> 11) / static_cast<f64>(1ull << 53));
    return max <= min ? min : min + (max - min) * u;
}

namespace {

struct Evaluator {
    const SoundCue& cue;
    CueState& state;
    const SoundDuration& duration;
    CuePlan& plan;

    // Plays node `id` from `offset`; returns how long it lasts.
    f64 Eval(u32 id, f64 offset, f32 volume_db, f32 pitch, int depth) {
        const CueNode* n = cue.Find(id);
        if (n == nullptr || depth > kMaxDepth || plan.items.size() >= kMaxItems) return 0.0;
        pitch = std::max(pitch, 1e-3f);
        switch (n->type) {
        case CueNodeType::Wave: return Wave(*n, offset, volume_db, pitch, n->looping);
        case CueNodeType::Random: {
            if (n->children.empty()) return 0.0;
            const auto last = state.last_pick.find(n->id);
            std::vector<f32> w(n->children.size());
            f32 total = 0.0f;
            for (usize i = 0; i < w.size(); ++i) {
                w[i] = std::max(i < n->weights.size() ? n->weights[i] : 1.0f, 0.0f);
                if (n->no_repeat && w.size() > 1 && last != state.last_pick.end() && last->second == i) w[i] = 0.0f;
                total += w[i];
            }
            if (total <= 0.0f) return 0.0;
            f32 r = state.Uniform(0.0f, total);
            usize pick = 0;
            while (pick + 1 < w.size() && (r >= w[pick] || w[pick] == 0.0f)) r -= w[pick++];
            while (w[pick] == 0.0f) --pick; // rounding ran past the last pick-able child
            state.last_pick[n->id] = static_cast<u32>(pick);
            return Eval(n->children[pick], offset, volume_db, pitch, depth + 1);
        }
        case CueNodeType::Sequence: {
            if (n->children.empty()) return 0.0;
            u32& next = state.next_in_sequence[n->id];
            const u32 pick = next % static_cast<u32>(n->children.size());
            next = pick + 1;
            return Eval(n->children[pick], offset, volume_db, pitch, depth + 1);
        }
        case CueNodeType::Modulator: {
            if (n->children.empty()) return 0.0;
            const f32 v = state.Uniform(n->volume_min_db, n->volume_max_db);
            const f32 p = state.Uniform(n->pitch_min, n->pitch_max);
            return Eval(n->children[0], offset, volume_db + v, pitch * std::max(p, 1e-3f), depth + 1);
        }
        case CueNodeType::Concatenator: {
            f64 t = offset;
            for (u32 c : n->children) {
                const f64 d = Eval(c, t, volume_db, pitch, depth + 1);
                if (std::isinf(d)) return kForever;
                t += d;
            }
            return t - offset;
        }
        case CueNodeType::Loop: {
            if (n->children.empty()) return 0.0;
            const u32 child = n->children[0];
            if (n->count == 0) {
                // A plain Wave loops seamlessly inside one voice.
                if (const CueNode* c = cue.Find(child); c != nullptr && c->type == CueNodeType::Wave) return Wave(*c, offset, volume_db, pitch, true);
                const f64 d = Eval(child, offset, volume_db, pitch, depth + 1);
                if (std::isinf(d) || d <= 1e-6) return d; // endless already, or nothing to repeat
                plan.tails.push_back({child, offset + d, volume_db, pitch});
                return kForever;
            }
            f64 t = offset;
            for (u32 k = 0; k < std::min(n->count, kMaxLoopCount); ++k) {
                const f64 d = Eval(child, t, volume_db, pitch, depth + 1);
                if (std::isinf(d)) return kForever;
                if (d <= 1e-6) break;
                t += d;
            }
            return t - offset;
        }
        case CueNodeType::Mix: {
            f64 longest = 0.0;
            for (usize i = 0; i < n->children.size(); ++i) {
                const f32 in = i < n->input_db.size() ? n->input_db[i] : 0.0f;
                longest = std::max(longest, Eval(n->children[i], offset, volume_db + in, pitch, depth + 1));
            }
            return longest;
        }
        case CueNodeType::Delay: {
            if (n->children.empty()) return 0.0;
            const f64 wait = std::max(0.0f, state.Uniform(n->delay_min, n->delay_max));
            return wait + Eval(n->children[0], offset + wait, volume_db, pitch, depth + 1);
        }
        }
        return 0.0;
    }

    f64 Wave(const CueNode& n, f64 offset, f32 volume_db, f32 pitch, bool loop) {
        const f64 length = duration ? duration(n.sound) : -1.0;
        if (n.sound.empty() || length <= 0.0) return 0.0; // unknown sounds play nothing
        plan.items.push_back({n.sound, offset, volume_db, pitch, loop, n.id});
        return loop ? kForever : length / pitch;
    }
};

} // namespace

CuePlan EvaluateCueNode(const SoundCue& cue, u32 node, CueState& state, const SoundDuration& duration, f64 offset, f32 volume_db, f32 pitch) {
    CuePlan plan;
    Evaluator e{cue, state, duration, plan};
    const f64 end = e.Eval(node, offset, volume_db, pitch, 0);
    plan.duration = std::isinf(end) ? kForever : end;
    return plan;
}

CuePlan EvaluateCue(const SoundCue& cue, CueState& state, const SoundDuration& duration) {
    return EvaluateCueNode(cue, cue.root, state, duration, 0.0, 0.0f, 1.0f);
}

} // namespace aether::audio
