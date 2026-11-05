#pragma once

#include "aether/nav/debug_draw.h"
#include "aether/script/ai_api.h"

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
inline constexpr u32 kTarget = 0xFF3030FF;      // red
} // namespace ai_debug_colors

// Appends, per shown agent: path corners (walked dim, ahead bright), a line
// from the agent to its next corner, crosses at the next corner and the goal,
// and a line from the agent to its perception target.
void BuildAiDebugOverlay(const World& world, const std::vector<script::AiDebugEntry>& entries,
                         const AiDebugOverlayOptions& options, nav::NavDebugDraw& out);

} // namespace aether::editor
