#include "aether/ai/utility.h"

#include "aether/core/json_util.h"

#include <algorithm>
#include <cmath>

namespace aether::ai {

namespace {
constexpr int kUtilityFormatVersion = 1;
constexpr const char* kCurveNames[] = {"linear", "quadratic", "logistic", "inverse", "step"};
f32 Clamp01(f32 v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }
} // namespace

const char* CurveKindName(CurveKind kind) { return kCurveNames[static_cast<usize>(kind)]; }

bool CurveKindFromName(const std::string& name, CurveKind& out) {
    for (usize i = 0; i < std::size(kCurveNames); ++i) {
        if (name == kCurveNames[i]) {
            out = static_cast<CurveKind>(i);
            return true;
        }
    }
    return false;
}

f32 ResponseCurve::Evaluate(f32 input) const {
    const f32 x = Clamp01(input);
    switch (kind) {
    case CurveKind::Linear: return Clamp01(slope * x + shift);
    case CurveKind::Quadratic: return Clamp01(std::pow(x, std::max(exponent, 0.0f)) + shift);
    case CurveKind::Inverse: return Clamp01(1.0f - std::pow(x, std::max(exponent, 0.0f)) + shift);
    case CurveKind::Logistic: {
        // Centred on 0.5 + shift; slope is the steepness. Rescaled so 0 maps near 0 and 1 near 1.
        const f32 k = std::max(slope, 0.0f) * 10.0f;
        return Clamp01(1.0f / (1.0f + std::exp(-k * (x - (0.5f + shift)))));
    }
    case CurveKind::Step: return x >= 0.5f + shift ? 1.0f : 0.0f;
    }
    return 0.0f;
}

f32 Consideration::Score(const Blackboard& blackboard) const {
    f64 value = 0.0;
    if (!blackboard.GetNumber(input, value)) return Clamp01(missing);
    const f32 span = max - min;
    const f32 normalized = span != 0.0f ? static_cast<f32>((value - min) / span) : 0.0f;
    return curve.Evaluate(normalized);
}

f32 UtilityAction::Score(const Blackboard& blackboard) const {
    if (weight <= 0.0f) return 0.0f;
    if (considerations.empty()) return weight;
    f32 product = 1.0f;
    for (const Consideration& c : considerations) {
        product *= c.Score(blackboard);
        if (product == 0.0f) return 0.0f;
    }
    // Compensation: a product of many numbers below 1 shrinks just because there are many of them.
    const f32 n = static_cast<f32>(considerations.size());
    const f32 modification = 1.0f - 1.0f / n;
    const f32 make_up = (1.0f - product) * modification;
    return (product + make_up * product) * weight;
}

usize UtilityBrain::Add(UtilityAction action) {
    actions_.push_back(std::move(action));
    return actions_.size() - 1;
}

UtilityBrain::Choice UtilityBrain::Evaluate(const Blackboard& blackboard) const {
    Choice best;
    f32 best_adjusted = 0.0f;
    for (usize i = 0; i < actions_.size(); ++i) {
        const f32 score = actions_[i].Score(blackboard);
        if (score <= 0.0f) continue;
        const f32 adjusted = score + (static_cast<i32>(i) == current_ ? inertia : 0.0f);
        if (!best.valid || adjusted > best_adjusted) {
            best = {true, i, score};
            best_adjusted = adjusted;
        }
    }
    return best;
}

UtilityBrain::Choice UtilityBrain::Choose(const Blackboard& blackboard) {
    const Choice choice = Evaluate(blackboard);
    current_ = choice.valid ? static_cast<i32>(choice.index) : -1;
    return choice;
}

nlohmann::json UtilityBrain::ToJson() const {
    nlohmann::json actions = nlohmann::json::array();
    for (const UtilityAction& a : actions_) {
        nlohmann::json considerations = nlohmann::json::array();
        for (const Consideration& c : a.considerations) {
            considerations.push_back({{"name", c.name}, {"input", c.input}, {"min", c.min}, {"max", c.max}, {"missing", c.missing},
                                      {"curve", {{"kind", CurveKindName(c.curve.kind)}, {"slope", c.curve.slope}, {"exponent", c.curve.exponent}, {"shift", c.curve.shift}}}});
        }
        actions.push_back({{"name", a.name}, {"weight", a.weight}, {"considerations", considerations}});
    }
    return {{"$type", "UtilityBrain"}, {"$version", kUtilityFormatVersion}, {"inertia", inertia}, {"actions", actions}};
}

bool UtilityBrain::FromJson(const nlohmann::json& json, UtilityBrain& out, std::string* error) {
    return GuardJsonLoader(error, "utility brain", [&] {
        const auto fail = [&](const std::string& why) {
            if (error != nullptr) *error = why;
            return false;
        };
        if (JsonString(json, "$type") != "UtilityBrain") return fail("not a utility brain");
        if (JsonVersion(json) > kUtilityFormatVersion) return fail("saved by a newer version of the engine");
        UtilityBrain brain;
        brain.inertia = json.value("inertia", 0.05f);
        if (!json.contains("actions") || !json["actions"].is_array()) return fail("a utility brain needs an actions list");
        for (const nlohmann::json& a : json["actions"]) {
            if (!a.is_object()) return fail("an action must be an object");
            UtilityAction action;
            action.name = a.value("name", std::string());
            if (action.name.empty()) return fail("an action needs a name");
            action.weight = a.value("weight", 1.0f);
            for (const nlohmann::json& c : a.value("considerations", nlohmann::json::array())) {
                if (!c.is_object()) return fail("action '" + action.name + "': a consideration must be an object");
                Consideration cons;
                cons.name = c.value("name", std::string());
                cons.input = c.value("input", std::string());
                if (cons.input.empty()) return fail("action '" + action.name + "': a consideration needs an input key");
                cons.min = c.value("min", 0.0f);
                cons.max = c.value("max", 1.0f);
                cons.missing = c.value("missing", 0.0f);
                if (const auto curve = c.find("curve"); curve != c.end()) {
                    if (!curve->is_object()) return fail("action '" + action.name + "': a curve must be an object");
                    if (!CurveKindFromName(curve->value("kind", std::string("linear")), cons.curve.kind)) {
                        return fail("action '" + action.name + "': unknown curve '" + JsonString(*curve, "kind") + "'");
                    }
                    cons.curve.slope = curve->value("slope", 1.0f);
                    cons.curve.exponent = curve->value("exponent", 2.0f);
                    cons.curve.shift = curve->value("shift", 0.0f);
                }
                action.considerations.push_back(std::move(cons));
            }
            brain.actions_.push_back(std::move(action));
        }
        out = std::move(brain);
        return true;
    });
}

} // namespace aether::ai
