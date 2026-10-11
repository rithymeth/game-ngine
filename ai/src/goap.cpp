#include "aether/ai/goap.h"

#include <algorithm>
#include <queue>
#include <unordered_map>

namespace aether::ai {

FactSet GoapDomain::Fact(const std::string& name) {
    for (usize i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return FactSet{1} << i;
    }
    if (names_.size() >= kMaxFacts) return 0;
    names_.push_back(name);
    return FactSet{1} << (names_.size() - 1);
}

FactSet GoapDomain::Facts(std::initializer_list<const char*> names) {
    FactSet bits = 0;
    for (const char* n : names) bits |= Fact(n);
    return bits;
}

GoapPlan PlanGoap(FactSet start, const GoapGoal& goal, std::span<const GoapAction> actions, usize max_nodes) {
    GoapPlan plan;
    if (goal.Met(start)) {
        plan.found = true;
        return plan;
    }
    struct Node {
        FactSet state;
        f32 cost;
        i64 parent;  // index into `nodes`, -1 for the start
        i32 action;
    };
    std::vector<Node> nodes;
    nodes.push_back({start, 0.0f, -1, -1});
    // (cost, creation order): the lowest cost first, the earliest created among equals.
    using Entry = std::pair<f32, usize>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    open.push({0.0f, 0});
    std::unordered_map<FactSet, f32> best;
    best[start] = 0.0f;

    while (!open.empty()) {
        const auto [cost, index] = open.top();
        open.pop();
        const Node node = nodes[index];
        if (const auto it = best.find(node.state); it != best.end() && it->second < cost) continue;  // a cheaper way arrived
        if (goal.Met(node.state)) {
            plan.found = true;
            plan.cost = cost;
            for (i64 at = static_cast<i64>(index); nodes[static_cast<usize>(at)].parent >= 0; at = nodes[static_cast<usize>(at)].parent) {
                plan.steps.push_back(static_cast<usize>(nodes[static_cast<usize>(at)].action));
            }
            std::reverse(plan.steps.begin(), plan.steps.end());
            return plan;
        }
        if (plan.expanded >= max_nodes) {
            plan.budget_exhausted = true;
            return plan;
        }
        ++plan.expanded;
        for (usize a = 0; a < actions.size(); ++a) {
            const GoapAction& action = actions[a];
            if (!(action.cost > 0.0f) || !action.CanRun(node.state)) continue;
            const FactSet next = action.Apply(node.state);
            const f32 next_cost = cost + action.cost;
            const auto known = best.find(next);
            if (known != best.end() && known->second <= next_cost) continue;
            best[next] = next_cost;
            nodes.push_back({next, next_cost, static_cast<i64>(index), static_cast<i32>(a)});
            open.push({next_cost, nodes.size() - 1});
        }
    }
    return plan;  // searched everything: no plan
}

} // namespace aether::ai
