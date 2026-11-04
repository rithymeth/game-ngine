#pragma once

#include "aether/ai/blackboard.h"
#include "aether/core/base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether::ai {

// State trees (Phase 40 step 4, docs/design/UPGRADE_PLAN.md): a hierarchy of states. Exactly one leaf is active;
// its ancestors are active with it. Each tick the active leaf's transitions are tried, then each ancestor's,
// innermost first; the first whose conditions all hold fires. A transition exits the states it leaves (leaf
// first), then enters the states it reaches (outermost first), and a transition to a state with children goes on
// to that state's first child, and so on down to a leaf. Entering and exiting set blackboard values.

enum class CompareOp : u8 { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
const char* CompareOpName(CompareOp op);
bool CompareOpFromName(const std::string& name, CompareOp& out);

struct StateCondition {
    std::string key;
    CompareOp op = CompareOp::Equal;
    BlackboardValue value;
    // Numbers compare as numbers (an int against a float is fine); anything else supports only Equal and
    // NotEqual. An unset key holds only for NotEqual.
    bool Holds(const Blackboard& blackboard) const;
};

struct StateEffect {
    std::string key;
    BlackboardValue value;
};

struct StateTransition {
    std::vector<StateCondition> conditions;  // all must hold; none means always
    std::string target;
};

struct TreeState {
    std::string name;
    std::string parent;  // empty for a top-level state
    std::vector<StateTransition> transitions;
    std::vector<StateEffect> on_enter;
    std::vector<StateEffect> on_exit;
};

class StateTree {
public:
    static constexpr int kMaxTransitionsPerTick = 8;  // a loop of always-true transitions stops here

    usize Add(TreeState state);
    const std::vector<TreeState>& States() const { return states_; }
    const TreeState* Find(const std::string& name) const;

    // False, with a reason, for no states, a duplicate or empty name, a missing parent, a parent cycle, or a
    // transition to a state that does not exist.
    bool Validate(std::string* error = nullptr) const;

    // Enters the first top-level state and goes down to a leaf. False if the tree does not validate.
    bool Start(Blackboard& blackboard);
    bool Running() const { return !path_.empty(); }
    void Stop(Blackboard& blackboard);  // exits everything, leaf first

    struct TickResult {
        int transitions = 0;       // how many fired this tick
        bool loop_limited = false; // stopped at kMaxTransitionsPerTick with another still ready
    };
    TickResult Tick(Blackboard& blackboard);

    const std::string& CurrentLeaf() const;                // empty when not running
    const std::vector<std::string>& ActivePath() const { return path_; }  // outermost first

    nlohmann::json ToJson() const;
    // Format version 1. False, with a reason, for a wrong type, a newer version or a bad shape; the tree is
    // validated too.
    static bool FromJson(const nlohmann::json& json, StateTree& out, std::string* error = nullptr);

private:
    std::vector<std::string> PathTo(const std::string& state) const;     // outermost first
    std::string DescendToLeaf(std::string state) const;
    bool FireOne(Blackboard& blackboard);
    void Apply(const std::vector<StateEffect>& effects, Blackboard& blackboard) const;

    std::vector<TreeState> states_;
    std::vector<std::string> path_;
};

} // namespace aether::ai
