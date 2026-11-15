#pragma once

#include "aether/script/luau_host.h"

// Luau's `Forces` table: the force-fields kit's explosion, on the active ForceFieldSystem. Present only when the kit is built (it needs physics). The
// fields themselves (the ForceField component) need no script: put one in the scene, and read or write its fields like any component's
// (entity:Get("ForceField")), to switch it on, change its strength or turn a wind round.
//
//   Forces.Explode (position, radius, strength [, upward])  -> number of bodies pushed: every dynamic body whose collider touches the sphere gets a change of
//                                                              velocity of `strength` m/s away from `position` (plus `upward`, 0 to 1, of that straight up as
//                                                              a lift), fading linearly to nothing at `radius`; the same for a heavy and a light body.
//                                                              0 for a radius that is not positive or a strength that is not a number
//
// A call with a missing or wrong-typed argument raises a script error; without an active system (no physics) it returns 0.

namespace aether::script {

void InstallForcesApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
