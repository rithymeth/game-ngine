#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/script/luau_host.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Luau's `AI` and `Nav` tables (M8.8 step 1, docs/design/M8_PRODUCTION_PLAN.md): the
// engine's perception and navigation for game scripts, so archetypes stop hard-coding
// line-of-sight boxes and room clamps. Positions are each entity's Transform (world space,
// as PerceptionWorld and NavWorld take them).
//
//   AI.CanSee (viewer, target, maxDistance?)   -> boolean: from the viewer's eyes (its
//       AIPerception's eye_height, else 1.6) to the target's AIStimuliSource target_height
//       (else 1.0), within maxDistance if given, through the active PerceptionWorld's
//       line of sight (no cone; with no world or no line-of-sight test, distance only)
//   AI.IsSightReady ()                         -> boolean: an active PerceptionWorld has a
//       line-of-sight test (so AI.CanSee is blocked by walls); scripts fall back without one
//   AI.GetKnown (entity)                       -> array of entities it knows (nil without AIPerception)
//   AI.GetTarget (entity)                      -> entity it is most aware of (nil if none)
//   Nav.IsReady ()                             -> boolean: an active NavWorld has a mesh
//   Nav.FindPath (from, to)                    -> array of vector corners and whether it is
//       complete (true) or partial (false); nil when there's no mesh or no path
//   Nav.MoveTowards (entity, goal, speed, dt)  -> boolean arrived, number remaining: moves the
//       entity's Transform along the path by speed * dt (dropping it onto the mesh's height
//       first, for free); false, nil (and no move) without a mesh, a path or a Transform
//   AI.SetDebugState (entity, state)           -> sets the text the AI debugger shows for the
//       entity (its decision: "idle", "tell", ...); nil or "" clears it
//
// A call with a missing or wrong-typed argument raises a script error.

namespace aether::script {

// What the AI debugger shows for one entity (M8.8: decisions and navigation
// state): the state its script last reported, what its AIPerception knows,
// and the path Nav.MoveTowards is walking it along.
struct AiDebugEntry {
    Entity entity;
    std::string state;           // from AI.SetDebugState (empty: none reported)
    bool has_perception = false; // it has an AIPerception
    Entity target;               // AIPerception::GetTarget (null if none)
    std::vector<Entity> known;   // AIPerception::known, in its order
    std::vector<Vec3> path;      // Nav.MoveTowards' cached corners (empty: no path)
    usize next_corner = 0;       // the corner being walked to (== path.size(): arrived)
    bool path_complete = false;  // the path reaches the goal (false: partial)
    Vec3 goal;                   // the goal of that path
};

// The data behind AI.SetDebugState and Nav.MoveTowards' path cache, one per
// host (InstallAiApi makes it). Read it between script ticks.
class AiDebugStore {
public:
    struct CachedPath {
        Entity entity;
        const void* mesh = nullptr;
        u64 revision = 0;
        Vec3 goal, left_at;
        usize next = 0; // the corner being walked to
        bool complete = false;
        std::vector<Vec3> points;
    };

    void SetState(Entity entity, std::string state);
    const std::string* FindState(Entity entity) const;
    // Nav.MoveTowards' entry for the entity's slot (it checks the entity itself).
    CachedPath& PathSlot(Entity entity) { return paths_[entity.index]; }
    const CachedPath* FindPath(Entity entity) const;
    // Every entity with a reported state, a cached path or an AIPerception (alive in `world`), by entity index.
    std::vector<AiDebugEntry> Collect(World& world) const;

    // Measurement only (profile tests): when off, Nav.MoveTowards re-paths on
    // every call instead of reusing the cached path. Process-wide, default on;
    // not reachable from scripts.
    static void SetPathCacheEnabled(bool enabled);
    static bool PathCacheEnabled();

private:
    struct State {
        Entity entity;
        std::string text;
    };
    std::unordered_map<u32, State> states_;
    std::unordered_map<u32, CachedPath> paths_;
};

// Installs the tables; the returned store is shared with them (keep it to read the debug data).
std::shared_ptr<AiDebugStore> InstallAiApi(LuauHost& host);

} // namespace aether::script
