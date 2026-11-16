#pragma once

#include "aether/script/luau_host.h"

// Luau's `Ropes` table: the ropes kit's calls, on the active RopeSystem. Present only when the kit is built (it needs physics). The Rope component itself needs no
// script: put one on an entity and it hangs there.
//
//   Ropes.Cut (entity, fraction)    -> true if the rope was cut: fraction 0 cuts next to the pinned end, 1 next to the free end, and the part beyond falls away.
//                                      false for an entity that is not a built rope, a fraction that is not a number, or a cut already made at that place
//   Ropes.Length (entity)           -> how long the chain is now, m (0 if the entity is not a built rope): the rope's own length when it hangs unstretched
//   Ropes.Segments (entity)         -> how many segments it has (0 if not built)
//   Ropes.SegmentPosition (entity, index)  -> the world position of that segment's centre (index 0 is the pinned end); a zero vector for no such segment
//
// A call with a missing or wrong-typed argument raises a script error; without an active system (no physics) the calls return false or 0.

namespace aether::script {

void InstallRopesApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
