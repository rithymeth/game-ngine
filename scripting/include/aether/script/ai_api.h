#pragma once

#include "aether/script/luau_host.h"

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
//
// A call with a missing or wrong-typed argument raises a script error.

namespace aether::script {

void InstallAiApi(LuauHost& host);

} // namespace aether::script
