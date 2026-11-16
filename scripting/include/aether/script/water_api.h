#pragma once

#include "aether/script/luau_host.h"

// Luau's `Water` table: the water kit's calls, on the active WaterSystem. Present only when the kit is built (it needs physics). The WaterSurface, Floater and
// Boat components need no script: put them on entities and the bodies float. A script only steers a boat and asks about the water.
//
//   Water.HeightAt (x, z)           -> the water's surface height there (world Y), waves included; a very large negative number where no water covers the point
//   Water.IsWater (x, z)            -> true if a water surface covers the point
//   Water.Current (x, z)            -> the current there, as a vector (zero where there is no water)
//   Water.Submersion (entity)       -> 0 to 1: how much of a floater is under the water as of the last step (0 for an entity that is not a floater)
//   Water.Drive (boat, throttle, steer)  -> true if the boat took the controls: throttle -1 (reverse) to 1, steer -1 (left) to 1 (right); they stay until changed.
//                                      false for an entity that is not an enabled Boat, or a number that is not finite
//   Water.Time ()                   -> the water's clock, s: it sets the waves' phase
//
// A call with a missing or wrong-typed argument raises a script error; without an active system (no physics) the calls return false or 0.

namespace aether::script {

void InstallWaterApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
