#include "test_framework.h"

#include "aether/ai/goap.h"
#include "aether/ai/state_tree.h"
#include "aether/ai/utility.h"

#include <cmath>

// Utility AI, state trees and GOAP (Phase 40 step 4): deterministic choices, bounded work, safe loading.

using namespace aether;
using namespace aether::ai;
using nlohmann::json;

namespace {

bool Near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

Consideration Linear(const char* input, f32 min = 0.0f, f32 max = 1.0f) {
    Consideration c;
    c.name = input;
    c.input = input;
    c.min = min;
    c.max = max;
    return c;
}

} // namespace

AETHER_TEST(Utility_CurvesStayInRangeAndHaveTheRightShape) {
    ResponseCurve linear;
    AETHER_CHECK(Near(linear.Evaluate(0.25f), 0.25f) && Near(linear.Evaluate(-5.0f), 0.0f) && Near(linear.Evaluate(9.0f), 1.0f));
    ResponseCurve quad;
    quad.kind = CurveKind::Quadratic;
    AETHER_CHECK(Near(quad.Evaluate(0.5f), 0.25f));
    ResponseCurve inverse;
    inverse.kind = CurveKind::Inverse;
    inverse.exponent = 1.0f;
    AETHER_CHECK(Near(inverse.Evaluate(0.25f), 0.75f));
    ResponseCurve logistic;
    logistic.kind = CurveKind::Logistic;
    AETHER_CHECK(logistic.Evaluate(0.0f) < 0.05f && logistic.Evaluate(1.0f) > 0.95f && Near(logistic.Evaluate(0.5f), 0.5f));
    ResponseCurve step;
    step.kind = CurveKind::Step;
    AETHER_CHECK(step.Evaluate(0.49f) == 0.0f && step.Evaluate(0.5f) == 1.0f);
    ResponseCurve nan_curve;
    AETHER_CHECK(nan_curve.Evaluate(std::nanf("")) == 0.0f); // NaN in, 0 out
}

AETHER_TEST(Utility_PicksTheBestActionKeepsItWithInertiaAndIgnoresDisabledOnes) {
    Blackboard bb;
    UtilityBrain brain;
    UtilityAction flee{"flee", 1.0f, {Linear("danger")}};
    UtilityAction heal{"heal", 1.0f, {Linear("hurt")}};
    UtilityAction idle{"idle", 0.2f, {}};
    UtilityAction off{"off", 0.0f, {}};
    brain.Add(flee);
    brain.Add(heal);
    brain.Add(idle);
    brain.Add(off);

    UtilityBrain::Choice c = brain.Choose(bb); // nothing known: only the constant action scores
    AETHER_CHECK(c.valid && brain.Actions()[c.index].name == "idle" && Near(c.score, 0.2f));

    AETHER_CHECK(bb.Set("danger", 0.9f) && bb.Set("hurt", 0.3f));
    c = brain.Choose(bb);
    AETHER_CHECK(brain.Actions()[c.index].name == "flee" && brain.Current() == 0);

    // A near tie keeps the current action; a clear lead takes over.
    AETHER_CHECK(bb.Set("hurt", 0.92f)); // 0.92 vs 0.9 + 0.05 inertia
    AETHER_CHECK(brain.Actions()[brain.Choose(bb).index].name == "flee");
    AETHER_CHECK(bb.Set("hurt", 0.99f));
    AETHER_CHECK(brain.Actions()[brain.Choose(bb).index].name == "heal");

    // Evaluate does not remember; Reset forgets.
    AETHER_CHECK(brain.Current() == 1);
    brain.Reset();
    AETHER_CHECK(brain.Current() == -1);

    Blackboard none;
    UtilityBrain empty;
    AETHER_CHECK(!empty.Evaluate(none).valid);
    UtilityBrain zero;
    zero.Add(off);
    AETHER_CHECK(!zero.Evaluate(none).valid);
}

AETHER_TEST(Utility_InputsMapOntoTheirRangeAndCompensationKeepsManyConsiderationsFair) {
    Blackboard bb;
    AETHER_CHECK(bb.Set("health", 50)); // an int works as a number
    Consideration c = Linear("health", 0.0f, 100.0f);
    AETHER_CHECK(Near(c.Score(bb), 0.5f));
    c.missing = 0.7f;
    Consideration absent = Linear("nope");
    absent.missing = 0.7f;
    AETHER_CHECK(Near(absent.Score(bb), 0.7f));
    Consideration flat = Linear("health", 5.0f, 5.0f); // a zero-width range scores 0, not NaN
    AETHER_CHECK(flat.Score(bb) == 0.0f);

    AETHER_CHECK(bb.Set("a", 0.8f) && bb.Set("b", 0.8f) && bb.Set("c", 0.8f));
    UtilityAction one{"one", 1.0f, {Linear("a")}};
    UtilityAction three{"three", 1.0f, {Linear("a"), Linear("b"), Linear("c")}};
    const f32 raw = 0.8f * 0.8f * 0.8f;
    AETHER_CHECK(Near(one.Score(bb), 0.8f));
    AETHER_CHECK(three.Score(bb) > raw && three.Score(bb) < 0.8f);
    AETHER_CHECK(bb.Set("b", 0.0f));
    AETHER_CHECK(three.Score(bb) == 0.0f); // a zero consideration vetoes the action
}

AETHER_TEST(Utility_JsonRoundTripsAndRefusesBadInput) {
    UtilityBrain brain;
    brain.inertia = 0.1f;
    Consideration c = Linear("hunger", 0.0f, 10.0f);
    c.curve.kind = CurveKind::Logistic;
    c.curve.slope = 0.6f;
    brain.Add({"eat", 2.0f, {c}});
    const json saved = brain.ToJson();
    UtilityBrain loaded;
    std::string error;
    AETHER_CHECK(UtilityBrain::FromJson(saved, loaded, &error));
    AETHER_CHECK(loaded.ToJson() == saved && Near(loaded.inertia, 0.1f));

    for (const char* text : {R"({"$type":"UtilityBrain","$version":9,"actions":[]})", R"({"$type":"UtilityBrain","actions":[{"name":""}]})",
                             R"({"$type":"UtilityBrain","actions":[{"name":"a","considerations":[{"input":"x","curve":{"kind":"wobbly"}}]}]})",
                             R"({"$type":"UtilityBrain","actions":[{"name":"a","considerations":[{"input":""}]}]})", R"({"$type":"UtilityBrain","actions":5})",
                             R"({"$type":5})", R"({"$type":"UtilityBrain","actions":[{"name":5}]})", "[]"}) {
        AETHER_CHECK(!UtilityBrain::FromJson(json::parse(text), loaded, &error));
    }
}

namespace {

StateTree Patrol() {
    StateTree tree;
    tree.Add({"Root", "", {}, {}, {}});
    tree.Add({"Patrol", "Root", {{{{"seen", CompareOp::Equal, true}}, "Chase"}}, {{"mode", std::string("patrol")}}, {}});
    // Chase has children: a transition to it goes on down to its first child, Approach.
    tree.Add({"Chase", "Root", {{{{"seen", CompareOp::Equal, false}}, "Patrol"}}, {{"mode", std::string("chase")}}, {}});
    tree.Add({"Approach", "Chase", {{{{"distance", CompareOp::Less, 2.0f}}, "Attack"}}, {}, {}});
    tree.Add({"Attack", "Chase", {{{{"distance", CompareOp::GreaterEqual, 2.0f}}, "Approach"}}, {{"attacking", true}}, {{"attacking", false}}});
    return tree;
}

} // namespace

AETHER_TEST(StateTree_RunsTransitionsOrderedEnterAndExitEffects) {
    Blackboard bb;
    StateTree tree = Patrol();
    std::string error;
    AETHER_CHECK(tree.Validate(&error));
    AETHER_CHECK(tree.Start(bb) && tree.CurrentLeaf() == "Patrol");
    AETHER_CHECK((tree.ActivePath() == std::vector<std::string>{"Root", "Patrol"}) && bb.GetAs<std::string>("mode") == "patrol");

    AETHER_CHECK(tree.Tick(bb).transitions == 0); // nothing seen yet

    AETHER_CHECK(bb.Set("seen", true) && bb.Set("distance", 10));
    AETHER_CHECK(tree.Tick(bb).transitions == 1 && tree.CurrentLeaf() == "Approach"); // Chase, then down to its first child
    AETHER_CHECK((tree.ActivePath() == std::vector<std::string>{"Root", "Chase", "Approach"}) && bb.GetAs<std::string>("mode") == "chase");

    // Approach -> Attack: only the leaf changes, Chase stays active above it.
    AETHER_CHECK(bb.Set("distance", 1.0f));
    AETHER_CHECK(tree.Tick(bb).transitions == 1 && tree.CurrentLeaf() == "Attack");
    AETHER_CHECK((tree.ActivePath() == std::vector<std::string>{"Root", "Chase", "Attack"}) && bb.GetAs<bool>("attacking"));

    // Losing the target: the leaf has no matching transition, its parent Chase does; the leaf exits first.
    AETHER_CHECK(bb.Set("seen", false));
    AETHER_CHECK(tree.Tick(bb).transitions == 1 && tree.CurrentLeaf() == "Patrol");
    AETHER_CHECK(!bb.GetAs<bool>("attacking", true) && bb.GetAs<std::string>("mode") == "patrol");

    tree.Stop(bb);
    AETHER_CHECK(!tree.Running() && tree.CurrentLeaf().empty() && tree.Tick(bb).transitions == 0);
}

AETHER_TEST(StateTree_StopsALoopOfAlwaysReadyTransitionsAndRejectsBadTrees) {
    Blackboard bb;
    StateTree loop;
    loop.Add({"A", "", {{{}, "B"}}, {}, {}});
    loop.Add({"B", "", {{{}, "A"}}, {}, {}});
    AETHER_CHECK(loop.Start(bb));
    const StateTree::TickResult result = loop.Tick(bb);
    AETHER_CHECK(result.transitions == StateTree::kMaxTransitionsPerTick && result.loop_limited);

    std::string error;
    StateTree bad;
    AETHER_CHECK(!bad.Validate(&error));
    bad.Add({"A", "", {}, {}, {}});
    bad.Add({"A", "", {}, {}, {}});
    AETHER_CHECK(!bad.Validate(&error) && error.find("two states") != std::string::npos);
    StateTree missing;
    missing.Add({"A", "Nowhere", {}, {}, {}});
    AETHER_CHECK(!missing.Validate(&error));
    StateTree cycle;
    cycle.Add({"Top", "", {}, {}, {}});
    cycle.Add({"A", "B", {}, {}, {}});
    cycle.Add({"B", "A", {}, {}, {}});
    AETHER_CHECK(!cycle.Validate(&error) && error.find("ancestor") != std::string::npos);
    StateTree target;
    target.Add({"A", "", {{{}, "Gone"}}, {}, {}});
    AETHER_CHECK(!target.Validate(&error) && !target.Start(bb));
}

AETHER_TEST(StateTree_ConditionsHandleUnsetKeysAndMixedNumbers) {
    Blackboard bb;
    StateCondition equal{"k", CompareOp::Equal, 3};
    StateCondition different{"k", CompareOp::NotEqual, 3};
    AETHER_CHECK(!equal.Holds(bb) && different.Holds(bb)); // unset: only != holds
    AETHER_CHECK(bb.Set("k", 3.0f));
    AETHER_CHECK(equal.Holds(bb) && !different.Holds(bb)); // a float against an int
    StateCondition ordered{"s", CompareOp::Less, 1};
    AETHER_CHECK(bb.Set("s", std::string("text")));
    AETHER_CHECK(!ordered.Holds(bb)); // ordering a string is never true
}

AETHER_TEST(StateTree_JsonRoundTripsAndRefusesBadInput) {
    const StateTree tree = Patrol();
    const json saved = tree.ToJson();
    StateTree loaded;
    std::string error;
    AETHER_CHECK(StateTree::FromJson(saved, loaded, &error));
    AETHER_CHECK(loaded.ToJson() == saved);
    Blackboard bb;
    AETHER_CHECK(loaded.Start(bb) && loaded.CurrentLeaf() == "Patrol");

    for (const char* text : {R"({"$type":"StateTree","$version":9,"states":[{"name":"A"}]})", R"({"$type":"StateTree","states":[]})", R"({"$type":"StateTree","states":5})",
                             R"({"$type":"StateTree","states":[{"name":"A","transitions":[{"to":"A","when":[{"key":"k","op":"~~","value":1}]}]}]})",
                             R"({"$type":"StateTree","states":[{"name":"A","transitions":[{"to":"Gone"}]}]})", R"({"$type":"StateTree","states":[{"name":"A","on_enter":[{"key":"k"}]}]})",
                             R"({"$type":"StateTree","states":[{"name":5}]})", R"({"$type":["StateTree"]})"}) {
        AETHER_CHECK(!StateTree::FromJson(json::parse(text), loaded, &error));
    }
}

AETHER_TEST(Goap_FindsTheCheapestPlanNotJustAPlan) {
    GoapDomain domain;
    const FactSet has_wood = domain.Fact("has_wood"), has_axe = domain.Fact("has_axe"), has_fire = domain.Fact("has_fire"), at_forest = domain.Fact("at_forest");
    std::vector<GoapAction> actions = {
        {"go_forest", 2.0f, 0, at_forest, at_forest, 0},
        {"buy_axe", 5.0f, 0, has_axe, has_axe, 0},
        {"make_axe", 1.0f, 0, has_axe, has_axe, 0},
        {"chop_wood", 1.0f, has_axe | at_forest, 0, has_wood, 0},
        {"gather_sticks", 6.0f, at_forest, has_wood, has_wood, 0},
        {"light_fire", 1.0f, has_wood, has_fire, has_fire, has_wood},
    };
    // Fire needs wood: go + make axe + chop (2+1+1 = 4) + light (1) = 5 beats go + gather (2+6) + light = 9.
    const GoapPlan plan = PlanGoap(0, {has_fire, 0}, actions);
    AETHER_CHECK(plan.found && Near(plan.cost, 5.0f) && plan.steps.size() == 4);
    std::vector<std::string> names;
    for (usize s : plan.steps) names.push_back(actions[s].name);
    AETHER_CHECK(names.back() == "light_fire" && std::count(names.begin(), names.end(), "chop_wood") == 1);
    AETHER_CHECK(!plan.budget_exhausted && plan.expanded > 0);

    // The same input gives the same plan.
    const GoapPlan again = PlanGoap(0, {has_fire, 0}, actions);
    AETHER_CHECK(again.steps == plan.steps);

    // Already met: a found plan of no steps. Impossible: not found, and the search ends.
    AETHER_CHECK(PlanGoap(has_fire, {has_fire, 0}, actions).found && PlanGoap(has_fire, {has_fire, 0}, actions).steps.empty());
    const GoapPlan impossible = PlanGoap(0, GoapGoal{domain.Fact("rich"), 0}, actions);
    AETHER_CHECK(!impossible.found && !impossible.budget_exhausted);
}

AETHER_TEST(Goap_RespectsTheNodeBudgetNegativeFactsAndBadCosts) {
    GoapDomain domain;
    std::vector<GoapAction> chain;
    // A 40-step chain: fact i -> fact i+1.
    std::vector<FactSet> bits;
    for (int i = 0; i < 41; ++i) bits.push_back(domain.Fact("f" + std::to_string(i)));
    for (int i = 0; i < 40; ++i) chain.push_back({"step" + std::to_string(i), 1.0f, bits[i], 0, bits[i + 1], bits[i]});
    const GoapPlan full = PlanGoap(bits[0], {bits[40], 0}, chain, 4096);
    AETHER_CHECK(full.found && full.steps.size() == 40 && Near(full.cost, 40.0f));
    const GoapPlan limited = PlanGoap(bits[0], {bits[40], 0}, chain, 5);
    AETHER_CHECK(!limited.found && limited.budget_exhausted && limited.expanded == 5);

    // want_false: the goal says a fact must be off.
    GoapDomain d2;
    const FactSet lit = d2.Fact("lit");
    std::vector<GoapAction> acts = {{"douse", 1.0f, lit, 0, 0, lit}};
    AETHER_CHECK(PlanGoap(lit, {0, lit}, acts).found && PlanGoap(lit, {0, lit}, acts).steps.size() == 1);

    // A cost of zero or less is skipped (it would loop for free).
    std::vector<GoapAction> free_actions = {{"free", 0.0f, 0, 0, lit, 0}};
    AETHER_CHECK(!PlanGoap(0, {lit, 0}, free_actions).found);

    // More than 64 facts: the extras are refused, not wrapped around.
    GoapDomain big;
    for (int i = 0; i < 64; ++i) AETHER_CHECK(big.Fact("x" + std::to_string(i)) != 0);
    AETHER_CHECK(big.Fact("one_too_many") == 0 && big.Count() == 64);
}
