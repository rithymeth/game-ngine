#include "aether/animation/anim_graph.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

namespace aether::anim {

using nlohmann::json;

namespace {

constexpr int kFormatVersion = 1;
constexpr int kMaxConduitHops = 8;

const char* kKindNames[] = {"Clip", "BlendSpace", "Blend", "BlendByBool", "BlendByInt", "Layered", "Additive", "StateMachine"};
const char* kTypeNames[] = {"bool", "int", "float", "trigger"};
const char* kOpNames[] = {"==", "!=", "<", "<=", ">", ">=", "true", "false", "triggered"};

// How many pose inputs a kind takes: exactly `min` unless `more` (then at least).
void InputCount(AnimNodeKind k, usize& min, bool& more) {
    more = false;
    switch (k) {
    case AnimNodeKind::Clip:
    case AnimNodeKind::BlendSpace:
    case AnimNodeKind::StateMachine: min = 0; break;
    case AnimNodeKind::BlendByInt: min = 2, more = true; break;
    default: min = 2; break;
    }
}

} // namespace

const char* VarTypeName(VarType t) { return kTypeNames[static_cast<int>(t)]; }
const char* AnimNodeKindName(AnimNodeKind k) { return kKindNames[static_cast<int>(k)]; }

i32 StateMachine::FindState(const std::string& n) const {
    for (usize i = 0; i < states.size(); ++i) {
        if (states[i].name == n) return static_cast<i32>(i);
    }
    return -1;
}

const AnimNode* AnimGraph::Find(u32 id) const {
    for (const AnimNode& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const StateMachine* AnimGraph::FindMachine(const std::string& n) const {
    for (const StateMachine& m : machines) {
        if (m.name == n) return &m;
    }
    return nullptr;
}

const AnimVariable* AnimGraph::FindVariable(const std::string& n) const {
    for (const AnimVariable& v : variables) {
        if (v.name == n) return &v;
    }
    return nullptr;
}

// --- Validation -------------------------------------------------------------------------------

std::vector<AnimDiagnostic> ValidateAnimGraph(const AnimGraph& g, const AnimAssets* assets) {
    std::vector<AnimDiagnostic> out;
    auto add = [&](const char* code, u32 node, const std::string& machine, const std::string& message, bool error = true) {
        out.push_back({code, error, node, machine, message});
    };
    std::set<std::string> names;
    for (const AnimVariable& v : g.variables) {
        if (!names.insert(v.name).second) add("AG011", 0, {}, "Two variables are named '" + v.name + "'.");
    }
    if (g.Find(g.output) == nullptr) add("AG001", 0, {}, "The graph has no output node.");

    // A variable used by a node or condition, of one of these types.
    auto check_var = [&](const std::string& name, std::initializer_list<VarType> types, u32 node, const std::string& machine,
                         const std::string& use) {
        const AnimVariable* v = g.FindVariable(name);
        if (v == nullptr) {
            add("AG004", node, machine, use + " uses '" + name + "', which isn't a variable.");
            return;
        }
        if (std::find(types.begin(), types.end(), v->type) == types.end()) {
            add("AG005", node, machine, use + " can't use the " + VarTypeName(v->type) + " variable '" + name + "'.");
        }
    };
    std::set<u32> ids;
    for (const AnimNode& n : g.nodes) {
        const std::string what = std::string(AnimNodeKindName(n.kind)) + " node " + std::to_string(n.id);
        if (!ids.insert(n.id).second) add("AG002", n.id, {}, "Two nodes share the id " + std::to_string(n.id) + ".");
        usize min = 0;
        bool more = false;
        InputCount(n.kind, min, more);
        if (more ? n.inputs.size() < min : n.inputs.size() != min) {
            add("AG002", n.id, {}, what + " takes " + (more ? "at least " : "") + std::to_string(min) + " inputs.");
        }
        for (u32 in : n.inputs) {
            if (g.Find(in) == nullptr) add("AG002", n.id, {}, what + " has an input that doesn't exist.");
        }
        switch (n.kind) {
        case AnimNodeKind::Clip:
            if (n.asset.empty() || (assets != nullptr && assets->clip && assets->clip(n.asset) == nullptr)) {
                add("AG010", n.id, {}, what + " plays '" + n.asset + "', which isn't a clip.");
            }
            break;
        case AnimNodeKind::BlendSpace: {
            const BlendSpace* bs = assets != nullptr && assets->blend_space ? assets->blend_space(n.asset) : nullptr;
            if (n.asset.empty() || (assets != nullptr && assets->blend_space && bs == nullptr)) {
                add("AG010", n.id, {}, what + " plays '" + n.asset + "', which isn't a blend space.");
            }
            check_var(n.variable, {VarType::Float, VarType::Int}, n.id, {}, what);
            if (!n.variable_y.empty() || (bs != nullptr && bs->dimensions == 2)) {
                check_var(n.variable_y, {VarType::Float, VarType::Int}, n.id, {}, what);
            }
            break;
        }
        case AnimNodeKind::Blend: check_var(n.variable, {VarType::Float}, n.id, {}, what); break;
        case AnimNodeKind::BlendByBool: check_var(n.variable, {VarType::Bool}, n.id, {}, what); break;
        case AnimNodeKind::BlendByInt: check_var(n.variable, {VarType::Int}, n.id, {}, what); break;
        case AnimNodeKind::Layered:
            if (n.bone.empty()) add("AG002", n.id, {}, what + " needs the bone its layer starts at.");
            break;
        case AnimNodeKind::Additive:
            if (!n.variable.empty()) check_var(n.variable, {VarType::Float}, n.id, {}, what);
            break;
        case AnimNodeKind::StateMachine:
            if (g.FindMachine(n.machine) == nullptr) add("AG006", n.id, n.machine, what + " runs '" + n.machine + "', which isn't a state machine.");
            break;
        }
    }

    // Loops through inputs (AG003).
    {
        std::map<u32, int> state;
        std::function<bool(u32)> visit = [&](u32 id) {
            if (state[id] == 1) return true;
            if (state[id] == 2) return false;
            state[id] = 1;
            const AnimNode* n = g.Find(id);
            if (n != nullptr) {
                for (u32 in : n->inputs) {
                    if (visit(in)) return true;
                }
            }
            state[id] = 2;
            return false;
        };
        for (const AnimNode& n : g.nodes) {
            if (visit(n.id)) {
                add("AG003", n.id, {}, "The pose nodes loop back on themselves here.");
                break;
            }
        }
    }
    // Machines inside themselves (AG008): machine -> the machines its states' subtrees run.
    {
        auto machines_under = [&](u32 root) {
            std::set<std::string> found, seen_nodes;
            std::function<void(u32)> walk = [&](u32 id) {
                if (!seen_nodes.insert(std::to_string(id)).second) return;
                const AnimNode* n = g.Find(id);
                if (n == nullptr) return;
                if (n->kind == AnimNodeKind::StateMachine) found.insert(n->machine);
                for (u32 in : n->inputs) walk(in);
            };
            walk(root);
            return found;
        };
        std::map<std::string, int> state;
        std::function<bool(const std::string&)> visit = [&](const std::string& name) {
            if (state[name] == 1) return true;
            if (state[name] == 2) return false;
            state[name] = 1;
            if (const StateMachine* m = g.FindMachine(name)) {
                for (const AnimState& s : m->states) {
                    if (s.conduit) continue;
                    for (const std::string& inner : machines_under(s.pose)) {
                        if (visit(inner)) return true;
                    }
                }
            }
            state[name] = 2;
            return false;
        };
        for (const StateMachine& m : g.machines) {
            if (visit(m.name)) {
                add("AG008", 0, m.name, "The state machine '" + m.name + "' runs inside itself.");
                break;
            }
        }
    }

    for (const StateMachine& m : g.machines) {
        std::set<std::string> states;
        if (m.states.empty()) {
            add("AG006", 0, m.name, "The state machine '" + m.name + "' has no states.");
            continue;
        }
        for (const AnimState& s : m.states) {
            if (!states.insert(s.name).second) add("AG011", 0, m.name, "Two states in '" + m.name + "' are named '" + s.name + "'.");
            if (!s.conduit && g.Find(s.pose) == nullptr) add("AG006", 0, m.name, "The state '" + s.name + "' plays a node that doesn't exist.");
        }
        if (m.entry >= m.states.size()) {
            add("AG006", 0, m.name, "The entry state of '" + m.name + "' doesn't exist.");
        } else if (m.states[m.entry].conduit) {
            add("AG007", 0, m.name, "A conduit can't be the entry state.");
        }
        std::vector<bool> has_exit(m.states.size(), false), reached(m.states.size(), false);
        for (const Transition& t : m.transitions) {
            const bool bad_from = t.from != kAnyState && (t.from < 0 || static_cast<usize>(t.from) >= m.states.size());
            if (bad_from || t.to >= m.states.size()) {
                add("AG006", 0, m.name, "A transition in '" + m.name + "' refers to a state that doesn't exist.");
                continue;
            }
            if (t.from != kAnyState) has_exit[static_cast<usize>(t.from)] = true;
            for (const Condition& c : t.conditions) {
                const AnimVariable* v = g.FindVariable(c.variable);
                const std::string use = "A transition to '" + m.states[t.to].name + "'";
                if (v == nullptr) {
                    add("AG004", 0, m.name, use + " uses '" + c.variable + "', which isn't a variable.");
                    continue;
                }
                const bool compare = c.op != CompareOp::IsTrue && c.op != CompareOp::IsFalse && c.op != CompareOp::Triggered;
                const bool equality = c.op == CompareOp::Equal || c.op == CompareOp::NotEqual;
                bool ok = true;
                switch (v->type) {
                case VarType::Trigger: ok = c.op == CompareOp::Triggered; break;
                case VarType::Bool: ok = c.op == CompareOp::IsTrue || c.op == CompareOp::IsFalse || equality; break;
                default: ok = compare; break;
                }
                if (!ok) add("AG005", 0, m.name, use + " compares the " + VarTypeName(v->type) + " '" + c.variable + "' with '" +
                                                     kOpNames[static_cast<int>(c.op)] + "'.");
            }
        }
        for (usize i = 0; i < m.states.size(); ++i) {
            if (m.states[i].conduit && !has_exit[i]) add("AG007", 0, m.name, "The conduit '" + m.states[i].name + "' has no way out.");
        }
        // Reachability from the entry (AG009).
        if (m.entry < m.states.size()) {
            std::vector<u32> stack{m.entry};
            reached[m.entry] = true;
            for (const Transition& t : m.transitions) {
                if (t.from == kAnyState && t.to < m.states.size()) {
                    if (!reached[t.to]) stack.push_back(t.to);
                    reached[t.to] = true;
                }
            }
            while (!stack.empty()) {
                const u32 s = stack.back();
                stack.pop_back();
                for (const Transition& t : m.transitions) {
                    if (t.from == static_cast<i32>(s) && t.to < m.states.size() && !reached[t.to]) {
                        reached[t.to] = true;
                        stack.push_back(t.to);
                    }
                }
            }
            for (usize i = 0; i < m.states.size(); ++i) {
                if (!reached[i]) add("AG009", 0, m.name, "No transition reaches the state '" + m.states[i].name + "'.", false);
            }
        }
    }
    return out;
}

// --- Runtime ------------------------------------------------------------------------------------

struct AnimGraphInstance::NodeState {
    u64 frame = ~0ull;
    Pose pose;
    f32 time = 0.0f;
    std::unique_ptr<BlendSpacePlayer> player;
    std::vector<f32> weights; // BlendByBool/Int crossfades
    BoneMask mask;
    bool has_mask = false;
};

struct AnimGraphInstance::MachineState {
    i32 current = 0;
    f32 time_in_state = 0.0f;
    i32 from = -1; // the state blending out, or -1
    bool from_snapshot = false;
    Pose snapshot;
    f32 blend_elapsed = 0.0f, blend_duration = 0.0f;
};

AnimGraphInstance::AnimGraphInstance(const AnimGraph& graph, const Skeleton& skeleton, AnimAssets assets)
    : graph_(graph), skeleton_(skeleton), assets_(std::move(assets)) {
    for (const AnimVariable& v : graph_.variables) values_[v.name] = v.type == VarType::Trigger ? 0.0f : v.default_value;
}

AnimGraphInstance::~AnimGraphInstance() = default;

namespace {
bool SetValue(std::map<std::string, f32>& values, const AnimGraph& g, const std::string& name, f32 value, std::initializer_list<VarType> types) {
    const AnimVariable* v = g.FindVariable(name);
    if (v == nullptr || std::find(types.begin(), types.end(), v->type) == types.end()) return false;
    values[name] = value;
    return true;
}
} // namespace

bool AnimGraphInstance::SetBool(const std::string& n, bool v) { return SetValue(values_, graph_, n, v ? 1.0f : 0.0f, {VarType::Bool}); }
bool AnimGraphInstance::SetInt(const std::string& n, i32 v) { return SetValue(values_, graph_, n, static_cast<f32>(v), {VarType::Int}); }
bool AnimGraphInstance::SetFloat(const std::string& n, f32 v) { return SetValue(values_, graph_, n, v, {VarType::Float, VarType::Int}); }
bool AnimGraphInstance::SetTrigger(const std::string& n) { return SetValue(values_, graph_, n, 1.0f, {VarType::Trigger}); }

f32 AnimGraphInstance::Get(const std::string& n) const {
    auto it = values_.find(n);
    return it == values_.end() ? 0.0f : it->second;
}

void AnimGraphInstance::Update(f32 dt, Pose& out) {
    ++frame_;
    changes_.clear();
    out = Evaluate(graph_.output, dt);
    for (const AnimVariable& v : graph_.variables) {
        if (v.type == VarType::Trigger) values_[v.name] = 0.0f;
    }
}

void AnimGraphInstance::Reset(u32 id) {
    const AnimNode* n = graph_.Find(id);
    if (n == nullptr) return;
    if (auto it = nodes_.find(id); it != nodes_.end()) {
        it->second->time = 0.0f;
        it->second->player.reset();
        it->second->weights.clear();
    }
    if (n->kind == AnimNodeKind::StateMachine) machines_.erase(id);
    for (u32 in : n->inputs) Reset(in);
}

f32 AnimGraphInstance::RemainingTime(u32 id) const {
    const AnimNode* n = graph_.Find(id);
    if (n == nullptr) return std::numeric_limits<f32>::infinity();
    auto it = nodes_.find(id);
    const NodeState* ns = it == nodes_.end() ? nullptr : it->second.get();
    switch (n->kind) {
    case AnimNodeKind::Clip: {
        const AnimationClip* c = assets_.clip ? assets_.clip(n->asset) : nullptr;
        if (c == nullptr || n->rate <= 0.0f) return 0.0f;
        const f32 t = ns != nullptr ? ns->time : 0.0f;
        const f32 at = n->loop ? ClipTime(*c, t, true) : std::min(t, c->duration);
        return (c->duration - at) / n->rate;
    }
    case AnimNodeKind::BlendSpace:
        return ns != nullptr && ns->player ? (1.0f - ns->player->Phase()) * ns->player->CycleDuration() : 0.0f;
    case AnimNodeKind::StateMachine: {
        auto m = machines_.find(id);
        const StateMachine* sm = graph_.FindMachine(n->machine);
        if (m == machines_.end() || sm == nullptr) return std::numeric_limits<f32>::infinity();
        return RemainingTime(sm->states[static_cast<usize>(m->second->current)].pose);
    }
    default: return n->inputs.empty() ? std::numeric_limits<f32>::infinity() : RemainingTime(n->inputs[0]);
    }
}

bool AnimGraphInstance::Passes(const Transition& t, const StateMachine& m, const MachineState& ms) const {
    for (const Condition& c : t.conditions) {
        const f32 v = Get(c.variable);
        bool ok = false;
        switch (c.op) {
        case CompareOp::Equal: ok = v == c.value; break;
        case CompareOp::NotEqual: ok = v != c.value; break;
        case CompareOp::Less: ok = v < c.value; break;
        case CompareOp::LessEqual: ok = v <= c.value; break;
        case CompareOp::Greater: ok = v > c.value; break;
        case CompareOp::GreaterEqual: ok = v >= c.value; break;
        case CompareOp::IsTrue:
        case CompareOp::Triggered: ok = v != 0.0f; break;
        case CompareOp::IsFalse: ok = v == 0.0f; break;
        }
        if (!ok) return false;
    }
    if (t.when_finished) {
        const AnimState& s = m.states[static_cast<usize>(ms.current)];
        if (s.conduit || RemainingTime(s.pose) > std::max(t.blend_time, 1e-4f)) return false;
    }
    return true;
}

bool AnimGraphInstance::FindTransition(const StateMachine& m, MachineState& ms, i32& target, f32& blend, std::vector<std::string>& used) {
    std::vector<const Transition*> candidates;
    for (const Transition& t : m.transitions) {
        if (t.to >= m.states.size()) continue;
        if (t.from == ms.current || (t.from == kAnyState && static_cast<i32>(t.to) != ms.current)) candidates.push_back(&t);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Transition* a, const Transition* b) { return a->priority > b->priority; });
    auto triggers_of = [&](const Transition& t) {
        for (const Condition& c : t.conditions) {
            if (c.op == CompareOp::Triggered) used.push_back(c.variable);
        }
    };
    for (const Transition* t : candidates) {
        if (!Passes(*t, m, ms)) continue;
        used.clear();
        triggers_of(*t);
        i32 to = static_cast<i32>(t->to);
        // Conduits: pass straight through to an exit that holds now.
        bool through = true;
        for (int hop = 0; hop < kMaxConduitHops && m.states[static_cast<usize>(to)].conduit; ++hop) {
            const Transition* exit = nullptr;
            std::vector<const Transition*> exits;
            for (const Transition& e : m.transitions) {
                if (e.from == to && e.to < m.states.size()) exits.push_back(&e);
            }
            std::stable_sort(exits.begin(), exits.end(), [](const Transition* a, const Transition* b) { return a->priority > b->priority; });
            MachineState probe = ms;
            probe.current = to;
            for (const Transition* e : exits) {
                if (!e->when_finished && Passes(*e, m, probe)) {
                    exit = e;
                    break;
                }
            }
            if (exit == nullptr) {
                through = false;
                break;
            }
            triggers_of(*exit);
            to = static_cast<i32>(exit->to);
        }
        if (!through || m.states[static_cast<usize>(to)].conduit || to == ms.current) continue;
        target = to;
        blend = t->blend_time;
        return true;
    }
    return false;
}

void AnimGraphInstance::EvaluateMachine(const AnimNode& node, f32 dt, Pose& out) {
    const StateMachine* m = graph_.FindMachine(node.machine);
    if (m == nullptr || m->states.empty() || m->entry >= m->states.size()) {
        out = RestPose(skeleton_);
        return;
    }
    auto& slot = machines_[node.id];
    if (!slot) {
        slot = std::make_unique<MachineState>();
        slot->current = static_cast<i32>(m->entry);
        Reset(m->states[m->entry].pose);
    }
    MachineState& ms = *slot;
    ms.time_in_state += dt;
    i32 target = -1;
    f32 blend = 0.0f;
    std::vector<std::string> used;
    if (FindTransition(*m, ms, target, blend, used)) {
        const bool blending = ms.blend_duration > 0.0f && ms.blend_elapsed < ms.blend_duration;
        if (blending) {
            ms.snapshot = out; // last frame's blended pose: blend on from where it was
            ms.from_snapshot = true;
        } else {
            ms.from_snapshot = false;
        }
        changes_.push_back({m->name, m->states[static_cast<usize>(ms.current)].name, m->states[static_cast<usize>(target)].name});
        ms.from = ms.current;
        ms.current = target;
        ms.time_in_state = 0.0f;
        ms.blend_elapsed = 0.0f;
        ms.blend_duration = blend;
        Reset(m->states[static_cast<usize>(target)].pose);
        for (const std::string& trigger : used) values_[trigger] = 0.0f;
    }
    const Pose target_pose = Evaluate(m->states[static_cast<usize>(ms.current)].pose, dt);
    if (ms.blend_duration > 0.0f && ms.blend_elapsed < ms.blend_duration && ms.from >= 0) {
        ms.blend_elapsed += dt;
        const f32 alpha = std::clamp(ms.blend_elapsed / ms.blend_duration, 0.0f, 1.0f);
        const Pose source = ms.from_snapshot ? ms.snapshot : Evaluate(m->states[static_cast<usize>(ms.from)].pose, dt);
        if (source.local.size() == target_pose.local.size()) BlendPoses(source, target_pose, alpha, out);
        else out = target_pose;
        if (alpha >= 1.0f) ms.from = -1;
    } else {
        ms.from = -1;
        out = target_pose;
    }
}

const Pose& AnimGraphInstance::Evaluate(u32 id, f32 dt) {
    auto& slot = nodes_[id];
    if (!slot) {
        slot = std::make_unique<NodeState>();
        slot->pose = RestPose(skeleton_);
    }
    NodeState& ns = *slot;
    if (ns.frame == frame_) return ns.pose; // shared input, or a loop (validation reports those)
    ns.frame = frame_;
    const AnimNode* n = graph_.Find(id);
    if (n == nullptr) {
        ns.pose = RestPose(skeleton_);
        return ns.pose;
    }
    auto input = [&](usize i) -> Pose { return i < n->inputs.size() ? Evaluate(n->inputs[i], dt) : RestPose(skeleton_); };
    switch (n->kind) {
    case AnimNodeKind::Clip: {
        const AnimationClip* c = assets_.clip ? assets_.clip(n->asset) : nullptr;
        if (c == nullptr) {
            ns.pose = RestPose(skeleton_);
            break;
        }
        ns.time += dt * n->rate;
        if (!n->loop) ns.time = std::clamp(ns.time, 0.0f, c->duration);
        SampleClip(*c, skeleton_, ns.time, n->loop, ns.pose);
        break;
    }
    case AnimNodeKind::BlendSpace: {
        const BlendSpace* bs = assets_.blend_space ? assets_.blend_space(n->asset) : nullptr;
        if (bs == nullptr) {
            ns.pose = RestPose(skeleton_);
            break;
        }
        if (!ns.player) {
            std::vector<const AnimationClip*> clips;
            for (const BlendSample& s : bs->samples) clips.push_back(assets_.clip ? assets_.clip(s.clip) : nullptr);
            ns.player = std::make_unique<BlendSpacePlayer>(*bs, std::move(clips), skeleton_);
        }
        ns.player->SetParameters(Get(n->variable), n->variable_y.empty() ? 0.0f : Get(n->variable_y));
        ns.player->Update(dt * n->rate, ns.pose);
        break;
    }
    case AnimNodeKind::Blend: {
        const f32 alpha = std::clamp(Get(n->variable), 0.0f, 1.0f);
        if (alpha <= 0.0f) ns.pose = input(0);
        else if (alpha >= 1.0f) ns.pose = input(1);
        else BlendPoses(input(0), input(1), alpha, ns.pose);
        break;
    }
    case AnimNodeKind::BlendByBool:
    case AnimNodeKind::BlendByInt: {
        const usize count = n->inputs.size();
        if (count == 0) {
            ns.pose = RestPose(skeleton_);
            break;
        }
        const f32 v = Get(n->variable);
        const usize active = n->kind == AnimNodeKind::BlendByBool ? (v != 0.0f ? 1 : 0)
                                                                   : static_cast<usize>(std::clamp(static_cast<i64>(std::lround(v)), i64{0}, static_cast<i64>(count - 1)));
        if (ns.weights.size() != count) {
            ns.weights.assign(count, 0.0f);
            ns.weights[std::min(active, count - 1)] = 1.0f; // start settled
        }
        const f32 step = n->blend_time > 0.0f ? dt / n->blend_time : 1.0f;
        for (usize i = 0; i < count; ++i) {
            const f32 goal = i == active ? 1.0f : 0.0f;
            ns.weights[i] = goal > ns.weights[i] ? std::min(goal, ns.weights[i] + step) : std::max(goal, ns.weights[i] - step);
        }
        std::vector<Pose> poses;
        std::vector<f32> weights;
        for (usize i = 0; i < count; ++i) {
            if (ns.weights[i] <= 0.0f) continue;
            poses.push_back(input(i));
            weights.push_back(ns.weights[i]);
        }
        std::vector<const Pose*> ptrs;
        for (const Pose& p : poses) ptrs.push_back(&p);
        if (ptrs.empty()) ns.pose = input(active);
        else BlendWeighted(ptrs, weights, ns.pose);
        break;
    }
    case AnimNodeKind::Layered: {
        if (!ns.has_mask) {
            ns.mask = MakeBoneMask(skeleton_, n->bone, n->depth);
            ns.has_mask = true;
        }
        const Pose base = input(0), layer = input(1);
        BlendPoses(base, layer, n->weight, ns.pose, &ns.mask);
        break;
    }
    case AnimNodeKind::Additive: {
        Pose base = input(0);
        const Pose delta = MakeAdditive(input(1), RestPose(skeleton_));
        ApplyAdditive(base, delta, n->variable.empty() ? n->weight : Get(n->variable));
        ns.pose = std::move(base);
        break;
    }
    case AnimNodeKind::StateMachine: {
        Pose out = ns.pose; // last frame's, for a snapshot
        EvaluateMachine(*n, dt, out);
        ns.pose = std::move(out); // std::map nodes don't move, so `ns` is still valid
        break;
    }
    }
    return ns.pose;
}

std::string AnimGraphInstance::CurrentState(u32 machine_node) const {
    const AnimNode* n = graph_.Find(machine_node);
    auto it = machines_.find(machine_node);
    if (n == nullptr || n->kind != AnimNodeKind::StateMachine) return {};
    const StateMachine* m = graph_.FindMachine(n->machine);
    if (m == nullptr || m->states.empty()) return {};
    const i32 current = it == machines_.end() ? static_cast<i32>(m->entry) : it->second->current;
    return static_cast<usize>(current) < m->states.size() ? m->states[static_cast<usize>(current)].name : std::string();
}

bool AnimGraphInstance::InTransition(u32 machine_node) const {
    auto it = machines_.find(machine_node);
    return it != machines_.end() && it->second->from >= 0;
}

// --- Files --------------------------------------------------------------------------------------

json AnimGraphToJson(const AnimGraph& g) {
    json vars = json::array(), nodes = json::array(), machines = json::array();
    for (const AnimVariable& v : g.variables) vars.push_back({{"name", v.name}, {"type", VarTypeName(v.type)}, {"default", v.default_value}});
    for (const AnimNode& n : g.nodes) {
        json j = {{"id", n.id}, {"kind", AnimNodeKindName(n.kind)}};
        if (!n.asset.empty()) j["asset"] = n.asset;
        if (!n.machine.empty()) j["machine"] = n.machine;
        if (!n.variable.empty()) j["variable"] = n.variable;
        if (!n.variable_y.empty()) j["variable_y"] = n.variable_y;
        if (!n.inputs.empty()) j["inputs"] = n.inputs;
        if (!n.loop) j["loop"] = false;
        if (n.rate != 1.0f) j["rate"] = n.rate;
        if (n.blend_time != 0.2f) j["blend_time"] = n.blend_time;
        if (n.weight != 1.0f) j["weight"] = n.weight;
        if (!n.bone.empty()) j["bone"] = n.bone;
        if (n.depth != 0) j["depth"] = n.depth;
        nodes.push_back(std::move(j));
    }
    for (const StateMachine& m : g.machines) {
        json states = json::array(), transitions = json::array();
        for (const AnimState& s : m.states) {
            json j = {{"name", s.name}};
            if (s.conduit) j["conduit"] = true;
            else j["pose"] = s.pose;
            states.push_back(std::move(j));
        }
        auto state_name = [&](i32 i) { return i == kAnyState ? std::string("*") : m.states[static_cast<usize>(i)].name; };
        for (const Transition& t : m.transitions) {
            if ((t.from != kAnyState && static_cast<usize>(t.from) >= m.states.size()) || t.to >= m.states.size()) continue;
            json conditions = json::array();
            for (const Condition& c : t.conditions) {
                json cj = {{"variable", c.variable}, {"op", kOpNames[static_cast<int>(c.op)]}};
                if (c.op != CompareOp::IsTrue && c.op != CompareOp::IsFalse && c.op != CompareOp::Triggered) cj["value"] = c.value;
                conditions.push_back(std::move(cj));
            }
            json j = {{"from", state_name(t.from)}, {"to", m.states[t.to].name}, {"blend_time", t.blend_time}, {"conditions", conditions}};
            if (t.when_finished) j["when_finished"] = true;
            if (t.priority != 0) j["priority"] = t.priority;
            transitions.push_back(std::move(j));
        }
        machines.push_back({{"name", m.name},
                            {"entry", m.entry < m.states.size() ? m.states[m.entry].name : std::string()},
                            {"states", states},
                            {"transitions", transitions}});
    }
    return {{"$type", "AnimGraph"}, {"$version", kFormatVersion}, {"variables", vars}, {"output", g.output}, {"nodes", nodes}, {"machines", machines}};
}

bool AnimGraphFromJson(const json& j, AnimGraph& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "AnimGraph") return fail("not an animation graph file");
    if (j.value("$version", 0) > kFormatVersion) return fail("saved by a newer version of the engine");
    AnimGraph g;
    for (const json& v : j.value("variables", json::array())) {
        AnimVariable var;
        var.name = v.value("name", "");
        const std::string type = v.value("type", "float");
        const auto t = std::find(std::begin(kTypeNames), std::end(kTypeNames), type);
        if (var.name.empty() || t == std::end(kTypeNames)) return fail("a variable needs a name and a known type");
        var.type = static_cast<VarType>(t - std::begin(kTypeNames));
        var.default_value = v.value("default", 0.0f);
        g.variables.push_back(std::move(var));
    }
    if (!j.value("output", json(0)).is_number_unsigned()) return fail("the output must be a node id");
    g.output = j.value("output", 0u);
    for (const json& n : j.value("nodes", json::array())) {
        if (!n.is_object() || !n.value("id", json()).is_number_unsigned()) return fail("a node needs an id");
        AnimNode node;
        node.id = n["id"].get<u32>();
        const std::string kind = n.value("kind", "");
        const auto k = std::find(std::begin(kKindNames), std::end(kKindNames), kind);
        if (k == std::end(kKindNames)) return fail("unknown node kind '" + kind + "'");
        node.kind = static_cast<AnimNodeKind>(k - std::begin(kKindNames));
        node.asset = n.value("asset", "");
        node.machine = n.value("machine", "");
        node.variable = n.value("variable", "");
        node.variable_y = n.value("variable_y", "");
        const json inputs = n.value("inputs", json::array());
        if (!inputs.is_array()) return fail("a node's inputs must be a list of ids");
        for (const json& in : inputs) {
            if (!in.is_number_unsigned()) return fail("a node's inputs must be a list of ids");
            node.inputs.push_back(in.get<u32>());
        }
        node.loop = n.value("loop", true);
        node.rate = n.value("rate", 1.0f);
        node.blend_time = n.value("blend_time", 0.2f);
        node.weight = n.value("weight", 1.0f);
        node.bone = n.value("bone", "");
        node.depth = n.value("depth", 0u);
        g.nodes.push_back(std::move(node));
    }
    for (const json& mj : j.value("machines", json::array())) {
        StateMachine m;
        m.name = mj.value("name", "");
        if (m.name.empty()) return fail("a state machine needs a name");
        for (const json& s : mj.value("states", json::array())) {
            AnimState st;
            st.name = s.value("name", "");
            st.conduit = s.value("conduit", false);
            st.pose = s.value("pose", 0u);
            if (st.name.empty()) return fail("a state needs a name");
            m.states.push_back(std::move(st));
        }
        const i32 entry = m.FindState(mj.value("entry", ""));
        m.entry = entry < 0 ? static_cast<u32>(m.states.size()) : static_cast<u32>(entry); // a bad entry is reported by validation
        for (const json& t : mj.value("transitions", json::array())) {
            Transition tr;
            const std::string from = t.value("from", "*");
            tr.from = from == "*" ? kAnyState : m.FindState(from);
            const i32 to = m.FindState(t.value("to", ""));
            if ((from != "*" && tr.from < 0) || to < 0) return fail("a transition in '" + m.name + "' names a state that doesn't exist");
            tr.to = static_cast<u32>(to);
            tr.blend_time = t.value("blend_time", 0.2f);
            tr.when_finished = t.value("when_finished", false);
            tr.priority = t.value("priority", 0);
            for (const json& c : t.value("conditions", json::array())) {
                Condition cond;
                cond.variable = c.value("variable", "");
                const std::string op = c.value("op", "true");
                const auto o = std::find(std::begin(kOpNames), std::end(kOpNames), op);
                if (o == std::end(kOpNames)) return fail("unknown comparison '" + op + "'");
                cond.op = static_cast<CompareOp>(o - std::begin(kOpNames));
                cond.value = c.value("value", 0.0f);
                tr.conditions.push_back(std::move(cond));
            }
            m.transitions.push_back(std::move(tr));
        }
        g.machines.push_back(std::move(m));
    }
    out = std::move(g);
    return true;
}

} // namespace aether::anim
