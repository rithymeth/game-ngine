#include "aether/ai/state_tree.h"

#include "aether/core/json_util.h"

#include <algorithm>
#include <set>

namespace aether::ai {

namespace {
constexpr int kStateTreeFormatVersion = 1;
constexpr const char* kOpNames[] = {"==", "!=", "<", "<=", ">", ">="};

bool IsNumber(const BlackboardValue& v) { return std::holds_alternative<i32>(v) || std::holds_alternative<f32>(v); }
f64 NumberOf(const BlackboardValue& v) { return std::holds_alternative<i32>(v) ? static_cast<f64>(std::get<i32>(v)) : static_cast<f64>(std::get<f32>(v)); }
} // namespace

const char* CompareOpName(CompareOp op) { return kOpNames[static_cast<usize>(op)]; }

bool CompareOpFromName(const std::string& name, CompareOp& out) {
    for (usize i = 0; i < std::size(kOpNames); ++i) {
        if (name == kOpNames[i]) {
            out = static_cast<CompareOp>(i);
            return true;
        }
    }
    return false;
}

bool StateCondition::Holds(const Blackboard& blackboard) const {
    const BlackboardValue* actual = blackboard.Get(key);
    if (actual == nullptr) return op == CompareOp::NotEqual;
    if (IsNumber(*actual) && IsNumber(value)) {
        const f64 a = NumberOf(*actual), b = NumberOf(value);
        switch (op) {
        case CompareOp::Equal: return a == b;
        case CompareOp::NotEqual: return a != b;
        case CompareOp::Less: return a < b;
        case CompareOp::LessEqual: return a <= b;
        case CompareOp::Greater: return a > b;
        case CompareOp::GreaterEqual: return a >= b;
        }
    }
    if (op == CompareOp::Equal) return ValuesEqual(*actual, value);
    if (op == CompareOp::NotEqual) return !ValuesEqual(*actual, value);
    return false;
}

usize StateTree::Add(TreeState state) {
    states_.push_back(std::move(state));
    return states_.size() - 1;
}

const TreeState* StateTree::Find(const std::string& name) const {
    for (const TreeState& s : states_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

bool StateTree::Validate(std::string* error) const {
    const auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (states_.empty()) return fail("a state tree needs at least one state");
    std::set<std::string> names;
    for (const TreeState& s : states_) {
        if (s.name.empty()) return fail("a state needs a name");
        if (!names.insert(s.name).second) return fail("two states are named '" + s.name + "'");
    }
    bool has_top = false;
    for (const TreeState& s : states_) {
        if (s.parent.empty()) has_top = true;
        else if (names.count(s.parent) == 0) return fail("state '" + s.name + "' has the missing parent '" + s.parent + "'");
        // A parent chain longer than the number of states is a cycle.
        std::string at = s.parent;
        for (usize hops = 0; !at.empty(); ++hops) {
            if (hops > states_.size()) return fail("state '" + s.name + "' is its own ancestor");
            at = Find(at)->parent;
        }
        for (const StateTransition& t : s.transitions) {
            if (names.count(t.target) == 0) return fail("state '" + s.name + "' goes to the missing state '" + t.target + "'");
        }
    }
    if (!has_top) return fail("a state tree needs a top-level state");
    return true;
}

std::vector<std::string> StateTree::PathTo(const std::string& state) const {
    std::vector<std::string> path;
    for (const TreeState* s = Find(state); s != nullptr && path.size() <= states_.size(); s = s->parent.empty() ? nullptr : Find(s->parent)) {
        path.push_back(s->name);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::string StateTree::DescendToLeaf(std::string state) const {
    for (usize hops = 0; hops <= states_.size(); ++hops) {
        const auto child = std::find_if(states_.begin(), states_.end(), [&](const TreeState& s) { return s.parent == state; });
        if (child == states_.end()) return state;
        state = child->name;
    }
    return state;
}

void StateTree::Apply(const std::vector<StateEffect>& effects, Blackboard& blackboard) const {
    for (const StateEffect& e : effects) (void)blackboard.Set(e.key, e.value);  // an undeclared key or wrong type is skipped
}

bool StateTree::Start(Blackboard& blackboard) {
    if (!Validate()) return false;
    if (Running()) Stop(blackboard);
    const auto top = std::find_if(states_.begin(), states_.end(), [](const TreeState& s) { return s.parent.empty(); });
    path_ = PathTo(DescendToLeaf(top->name));
    for (const std::string& name : path_) Apply(Find(name)->on_enter, blackboard);
    return true;
}

void StateTree::Stop(Blackboard& blackboard) {
    for (auto it = path_.rbegin(); it != path_.rend(); ++it) Apply(Find(*it)->on_exit, blackboard);
    path_.clear();
}

const std::string& StateTree::CurrentLeaf() const {
    static const std::string none;
    return path_.empty() ? none : path_.back();
}

bool StateTree::FireOne(Blackboard& blackboard) {
    for (auto level = path_.rbegin(); level != path_.rend(); ++level) {
        for (const StateTransition& t : Find(*level)->transitions) {
            const bool ready = std::all_of(t.conditions.begin(), t.conditions.end(), [&](const StateCondition& c) { return c.Holds(blackboard); });
            if (!ready) continue;
            const std::vector<std::string> next = PathTo(DescendToLeaf(t.target));
            // Keep the states that are in both paths, except that a transition to a state already active (itself or
            // an ancestor) re-enters it.
            usize shared = 0;
            const usize reentered_from = static_cast<usize>(std::distance(path_.begin(), std::find(path_.begin(), path_.end(), t.target)));
            while (shared < path_.size() && shared < next.size() && path_[shared] == next[shared]) ++shared;
            shared = std::min(shared, reentered_from);
            for (usize i = path_.size(); i > shared; --i) Apply(Find(path_[i - 1])->on_exit, blackboard);
            path_ = next;
            for (usize i = shared; i < path_.size(); ++i) Apply(Find(path_[i])->on_enter, blackboard);
            return true;
        }
    }
    return false;
}

StateTree::TickResult StateTree::Tick(Blackboard& blackboard) {
    TickResult result;
    if (!Running()) return result;
    while (result.transitions < kMaxTransitionsPerTick && FireOne(blackboard)) ++result.transitions;
    if (result.transitions == kMaxTransitionsPerTick) {
        // Another one is ready: do not run it, only report it.
        const std::vector<std::string> saved = path_;
        Blackboard probe = blackboard;
        result.loop_limited = FireOne(probe);
        path_ = saved;
    }
    return result;
}

nlohmann::json StateTree::ToJson() const {
    nlohmann::json states = nlohmann::json::array();
    const auto effects = [](const std::vector<StateEffect>& list) {
        nlohmann::json out = nlohmann::json::array();
        for (const StateEffect& e : list) out.push_back({{"key", e.key}, {"value", ValueToJson(e.value)}});
        return out;
    };
    for (const TreeState& s : states_) {
        nlohmann::json transitions = nlohmann::json::array();
        for (const StateTransition& t : s.transitions) {
            nlohmann::json conditions = nlohmann::json::array();
            for (const StateCondition& c : t.conditions) conditions.push_back({{"key", c.key}, {"op", CompareOpName(c.op)}, {"value", ValueToJson(c.value)}});
            transitions.push_back({{"to", t.target}, {"when", conditions}});
        }
        states.push_back({{"name", s.name}, {"parent", s.parent}, {"transitions", transitions}, {"on_enter", effects(s.on_enter)}, {"on_exit", effects(s.on_exit)}});
    }
    return {{"$type", "StateTree"}, {"$version", kStateTreeFormatVersion}, {"states", states}};
}

namespace {
// A blackboard value from JSON text: a bool, a whole number, a float, a string or [x, y, z]. Null is unset.
bool ReadValue(const nlohmann::json& j, BlackboardValue& out) {
    if (j.is_null()) { out = {}; return true; }
    if (j.is_boolean()) { out = j.get<bool>(); return true; }
    if (j.is_number_integer()) {
        const auto v = j.get<std::int64_t>();
        if (v < INT32_MIN || v > INT32_MAX) return false;
        out = static_cast<i32>(v);
        return true;
    }
    if (j.is_number()) { out = j.get<f32>(); return true; }
    if (j.is_string()) { out = j.get<std::string>(); return true; }
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
        out = Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
        return true;
    }
    return false;
}
} // namespace

bool StateTree::FromJson(const nlohmann::json& json, StateTree& out, std::string* error) {
    return GuardJsonLoader(error, "state tree", [&] {
        const auto fail = [&](const std::string& why) {
            if (error != nullptr) *error = why;
            return false;
        };
        if (JsonString(json, "$type") != "StateTree") return fail("not a state tree");
        if (JsonVersion(json) > kStateTreeFormatVersion) return fail("saved by a newer version of the engine");
        if (!json.contains("states") || !json["states"].is_array()) return fail("a state tree needs a states list");
        const auto read_effects = [&](const nlohmann::json& list, std::vector<StateEffect>& dest, const std::string& state) {
            for (const nlohmann::json& e : list) {
                StateEffect effect;
                if (!e.is_object() || JsonString(e, "key").empty() || !e.contains("value") || !ReadValue(e["value"], effect.value)) {
                    return fail("state '" + state + "': an effect needs a key and a value");
                }
                effect.key = JsonString(e, "key");
                dest.push_back(std::move(effect));
            }
            return true;
        };
        StateTree tree;
        for (const nlohmann::json& s : json["states"]) {
            if (!s.is_object()) return fail("a state must be an object");
            TreeState state;
            state.name = JsonString(s, "name");
            state.parent = JsonString(s, "parent");
            for (const nlohmann::json& t : s.value("transitions", nlohmann::json::array())) {
                if (!t.is_object()) return fail("state '" + state.name + "': a transition must be an object");
                StateTransition transition;
                transition.target = JsonString(t, "to");
                for (const nlohmann::json& c : t.value("when", nlohmann::json::array())) {
                    StateCondition condition;
                    if (!c.is_object() || JsonString(c, "key").empty() || !CompareOpFromName(JsonString(c, "op", "=="), condition.op) ||
                        !c.contains("value") || !ReadValue(c["value"], condition.value)) {
                        return fail("state '" + state.name + "': a condition needs a key, an operator and a value");
                    }
                    condition.key = JsonString(c, "key");
                    transition.conditions.push_back(std::move(condition));
                }
                state.transitions.push_back(std::move(transition));
            }
            if (!read_effects(s.value("on_enter", nlohmann::json::array()), state.on_enter, state.name)) return false;
            if (!read_effects(s.value("on_exit", nlohmann::json::array()), state.on_exit, state.name)) return false;
            tree.Add(std::move(state));
        }
        if (!tree.Validate(error)) return false;
        out = std::move(tree);
        return true;
    });
}

} // namespace aether::ai
