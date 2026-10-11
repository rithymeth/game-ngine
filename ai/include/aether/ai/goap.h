#pragma once

#include "aether/core/base.h"

#include <span>
#include <string>
#include <vector>

namespace aether::ai {

// Goal-oriented action planning (Phase 40 step 4, docs/design/UPGRADE_PLAN.md): the world is up to 64 true or
// false facts, an action needs some facts true and some false and then sets and clears others, and the planner
// finds the cheapest sequence of actions from the start facts to a goal. A uniform-cost search (so the plan is
// the cheapest one, not just any one) with a node budget, and fully deterministic: equal costs go to the state
// reached first.

using FactSet = u64;

class GoapDomain {
public:
    static constexpr usize kMaxFacts = 64;
    // The bit of a named fact, adding it the first time. 0 (and no fact added) past 64 facts.
    FactSet Fact(const std::string& name);
    FactSet Facts(std::initializer_list<const char*> names);
    usize Count() const { return names_.size(); }
    const std::string& Name(usize index) const { return names_[index]; }

private:
    std::vector<std::string> names_;
};

struct GoapAction {
    std::string name;
    f32 cost = 1.0f;          // must be above 0
    FactSet require_true = 0;
    FactSet require_false = 0;
    FactSet set = 0;          // facts made true
    FactSet clear = 0;        // facts made false (applied before `set`)
    bool CanRun(FactSet state) const { return (state & require_true) == require_true && (state & require_false) == 0; }
    FactSet Apply(FactSet state) const { return (state & ~clear) | set; }
};

struct GoapGoal {
    FactSet want_true = 0;
    FactSet want_false = 0;
    bool Met(FactSet state) const { return (state & want_true) == want_true && (state & want_false) == 0; }
};

struct GoapPlan {
    bool found = false;
    bool budget_exhausted = false;  // the search stopped at max_nodes before it could say
    std::vector<usize> steps;       // indices into the action list, in order
    f32 cost = 0.0f;
    usize expanded = 0;             // states expanded, for tuning the budget
};

// An action with a cost of 0 or less is skipped. A goal met at the start is a found plan of no steps.
GoapPlan PlanGoap(FactSet start, const GoapGoal& goal, std::span<const GoapAction> actions, usize max_nodes = 4096);

} // namespace aether::ai
