#pragma once

#include "aether/ai/blackboard.h"
#include "aether/core/base.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether::ai {

// Utility AI (Phase 40 step 4, docs/design/UPGRADE_PLAN.md): each action is scored from considerations, each a
// blackboard number run through a response curve; the highest score wins. Scoring is a pure function of the
// blackboard, so a choice is reproducible. The brain remembers its last choice and gives it a small bonus
// (`inertia`) so two close scores do not make it flip back and forth every tick.

enum class CurveKind : u8 { Linear, Quadratic, Logistic, Inverse, Step };
const char* CurveKindName(CurveKind kind);
bool CurveKindFromName(const std::string& name, CurveKind& out);

struct ResponseCurve {
    CurveKind kind = CurveKind::Linear;
    f32 slope = 1.0f;    // Linear: y = slope * x + shift; Logistic: steepness; Step: the threshold's sharpness is fixed
    f32 exponent = 2.0f; // Quadratic and Inverse
    f32 shift = 0.0f;    // added to the result (Linear) or moves the midpoint (Logistic, Step: the threshold)
    // x in [0, 1] (clamped), result in [0, 1] (clamped).
    f32 Evaluate(f32 x) const;
};

struct Consideration {
    std::string name;
    std::string input;  // a blackboard number (int or float) key; an unset or missing key scores as `missing`
    f32 min = 0.0f;     // the input range mapped onto [0, 1]
    f32 max = 1.0f;
    f32 missing = 0.0f;
    ResponseCurve curve;
    f32 Score(const Blackboard& blackboard) const;
};

struct UtilityAction {
    std::string name;
    f32 weight = 1.0f;  // multiplies the score; 0 disables the action
    std::vector<Consideration> considerations;
    // The product of the consideration scores with the usual compensation (so many considerations do not drag
    // the score down just by being many), times `weight`. No considerations scores `weight`.
    f32 Score(const Blackboard& blackboard) const;
};

class UtilityBrain {
public:
    f32 inertia = 0.05f;  // added to the score of the action chosen last time

    usize Add(UtilityAction action);
    const std::vector<UtilityAction>& Actions() const { return actions_; }

    struct Choice {
        bool valid = false;  // false when there are no actions or every score is 0
        usize index = 0;
        f32 score = 0.0f;
    };
    // The best action now, without remembering it.
    Choice Evaluate(const Blackboard& blackboard) const;
    // Evaluate, and remember the choice for the next tick's inertia. Ties go to the earlier action.
    Choice Choose(const Blackboard& blackboard);
    // The last chosen action, or -1.
    i32 Current() const { return current_; }
    void Reset() { current_ = -1; }

    nlohmann::json ToJson() const;
    // Format version 1. False, with a reason, for a wrong type, a newer version, a bad curve name or a bad shape.
    static bool FromJson(const nlohmann::json& json, UtilityBrain& out, std::string* error = nullptr);

private:
    std::vector<UtilityAction> actions_;
    i32 current_ = -1;
};

} // namespace aether::ai
