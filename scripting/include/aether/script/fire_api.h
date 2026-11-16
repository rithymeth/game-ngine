#pragma once

#include "aether/script/luau_host.h"

// Luau's `Fire` table: the fire kit's calls, on the scene's FireSystem. Present only when the kit is built. The Flammable component needs no script: put it on an
// entity and it catches from a burning neighbour; a script lights things, puts them out and wets them.
//
//   Fire.Ignite (entity)             -> true if it was set alight (false: not a Flammable, already burning, burnt out, or too wet)
//   Fire.Extinguish (entity)         -> true if it was burning and is out now
//   Fire.IgniteNear (x, y, z, radius) -> how many Flammables within the radius caught: a fire bomb, a lightning strike
//   Fire.IsBurning (entity)          -> true while it burns
//   Fire.IsBurnt (entity)            -> true once its fuel is gone
//   Fire.Heat (entity)               -> the heat it has taken so far (it catches at its ignition_heat)
//   Fire.FuelLeft (entity)           -> seconds it can still burn
//   Fire.Burning ()                  -> how many things burn now
//   Fire.SetWetness (entity, w)      -> true if set: 0 dry to 1 soaked; at its douse_wetness a burning one goes out
//   Fire.WetAll (w)                  -> how many Flammables it set: the rain, from Weather.Rain or Weather.Wetness
//
// A call with a missing or wrong-typed argument raises a script error; without a FireSystem (no scene yet) the calls return false or 0.

namespace aether::script {

void InstallFireApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
