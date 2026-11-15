#pragma once

#include "aether/script/luau_host.h"

// Luau's `Destruction` table: the destruction kit's calls, on the active DestructionSystem. Present only when the kit is built (it needs physics). The
// Destructible component itself needs no script: put one on an entity with a box collider, and a hard enough hit breaks it.
//
//   Destruction.Damage (entity, amount [, point])  -> true if that damage used up the entity's health, so it breaks at the next frame's update. false for an
//                                                      entity that is not an enabled Destructible, that has no health, or for an amount that is not positive
//   Destruction.Shatter (entity [, point])         -> true if the entity will break at the next update, whatever its health or speed; false if it is not an
//                                                      enabled Destructible. `point` is where the pieces burst from (default: the entity's position)
//   Destruction.Debris ()                          -> how many pieces are lying about
//
// A call with a missing or wrong-typed argument raises a script error; without an active system (no physics) the calls return false (Debris: 0).

namespace aether::script {

void InstallDestructionApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
