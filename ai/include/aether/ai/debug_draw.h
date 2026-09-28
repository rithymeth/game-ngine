#pragma once

#include "aether/ai/perception.h"
#include "aether/nav/crowd.h"
#include "aether/nav/debug_draw.h"

namespace aether::ai {

// AI debug drawing (Phase 20 step 6): each agent's way ahead (its corners
// from the crowd) and goal, and each AIPerception's sight cone, hearing
// range and what it knows (a line to what it sees, a dimmer one to where
// it last sensed the rest).
struct AIDebugOptions {
    bool agent_paths = true;
    bool sight = true;
    bool hearing = false;
    bool known = true;
};

namespace ai_colors {
inline constexpr u32 kCorners = 0xFF40FF40;
inline constexpr u32 kGoal = 0xFF40FFFF;
inline constexpr u32 kSight = 0xA040C0FF;
inline constexpr u32 kHearing = 0x60FFC040;
inline constexpr u32 kSeen = 0xFF3030FF;
inline constexpr u32 kRemembered = 0xA000C0FF;
} // namespace ai_colors

void DrawAIDebug(World& world, const nav::NavCrowd* crowd, const AIDebugOptions& options, nav::NavDebugDraw& out);

} // namespace aether::ai
