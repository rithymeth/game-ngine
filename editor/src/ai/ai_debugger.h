#pragma once

#include "aether/ai/goap.h"
#include "aether/ai/state_tree.h"
#include "aether/ai/utility.h"
#include "aether/nav/debug_draw.h"
#include "aether/script/ai_api.h"

#include <span>
#include <string>
#include <vector>

namespace aether::editor {

// The AI Debugger (M8.8: decisions and navigation state in the editor). Pure
// data shaping over an AiDebugStore::Collect snapshot, so the Qt dock stays a
// thin view and tests run it headless.
struct AiDebugRow {
    Entity entity;
    std::string name;   // EntityName, or "#<index>"
    std::string state;  // AI.SetDebugState ("-" when none reported)
    std::string target; // perception target's name ("-": none, "" without AIPerception)
    usize known = 0;    // AIPerception::known count
    f32 awareness = 0.0f; // Perception 2.0 awareness of the target (0..1)
    bool alerted = false; // IsAlerted(target)
    std::string path;   // "none", "corner 2/5", "arrived"; " (partial)" when incomplete
};

std::string AiDebugEntityLabel(const World& world, Entity entity);
std::vector<AiDebugRow> BuildAiDebugRows(const World& world, const std::vector<script::AiDebugEntry>& entries);

struct AiDebugOverlayOptions {
    bool all_agents = false; // false: only `selected`
    Entity selected;
    f32 lift = 0.1f;         // drawn this far above the corners
    f32 marker = 0.25f;      // half size of the corner / goal crosses
};

namespace ai_debug_colors {
inline constexpr u32 kPathWalked = 0x80808080;  // ABGR, as NavDebugLine
inline constexpr u32 kPathAhead = 0xFF00D0FF;   // amber
inline constexpr u32 kNextCorner = 0xFF00FFFF;  // yellow
inline constexpr u32 kGoal = 0xFF40FF40;        // green
inline constexpr u32 kGoalPartial = 0xFF4080FF; // orange: path does not reach it
inline constexpr u32 kTarget = 0xFF3030FF;      // red: alerted, or no awareness yet
inline constexpr u32 kTargetSuspicious = 0xFF00A5FF; // orange: aware of it, not yet alert
} // namespace ai_debug_colors

// Appends, per shown agent: path corners (walked dim, ahead bright), a line
// from the agent to its next corner, crosses at the next corner and the goal,
// and a line from the agent to its perception target.
void BuildAiDebugOverlay(const World& world, const std::vector<script::AiDebugEntry>& entries,
                         const AiDebugOverlayOptions& options, nav::NavDebugDraw& out);

// --- Phase 40.6: the Utility / State-tree / GOAP brains (Phase 40.4) as
// debugger rows, shaped here so the Qt dock stays a thin view and tests run
// them headless, the same way the rows and overlay above work.

// One utility action scored under a blackboard; `chosen` marks the action
// Evaluate() would pick now.
struct UtilityDebugRow {
    std::string action;
    f32 score = 0.0f;
    bool chosen = false;
    usize considerations = 0;
};
// Every action of `brain`, in the brain's order, scored against `blackboard`.
std::vector<UtilityDebugRow> BuildUtilityDebug(const ai::UtilityBrain& brain, const ai::Blackboard& blackboard);

// A state tree's active hierarchy and what its last tick did.
struct StateTreeDebug {
    std::vector<std::string> active_path; // outermost first; empty when not running
    std::string leaf = "-";               // CurrentLeaf
    i32 last_transitions = 0;
    bool loop_limited = false;
};
StateTreeDebug BuildStateTreeDebug(const ai::StateTree& tree, const ai::StateTree::TickResult& last_tick);

// One step of a GOAP plan, resolved to an action name.
struct GoapDebugRow {
    u32 step = 0;
    usize action = 0; // index into the action list
    std::string name;
};
std::vector<GoapDebugRow> BuildGoapDebug(const ai::GoapPlan& plan, std::span<const ai::GoapAction> actions);

} // namespace aether::editor
