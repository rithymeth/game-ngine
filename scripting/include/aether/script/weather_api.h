#pragma once

#include "aether/script/luau_host.h"

// Luau's `Weather` table: the game's weather (the weather kit). Present only when the kit is built. The weather belongs to the game, so it keeps going through a
// scene change. The kit only keeps the numbers; what the scene does with them (a fog density, a sun scaled down, a rain emitter, a lightning light) is the game's.
//
//   Weather.Set (kind [, seconds])   -> boolean: blends to a kind ("clear", "partly_cloudy", "overcast", "rain", "storm", "fog", "snow", any case) over `seconds`
//                                       (default 30, 0 is at once, up to 3600). false (and nothing changes) for a name that is not one or a time out of range
//   Weather.Current () / Target ()   -> the kind it is at / is going to, as a string (the same once it has arrived)
//   Weather.Blend ()                 -> number 0 to 1: how far through the blend
//   Weather.Clouds () / Rain () / Snow () / Fog () / LightScale ()  -> numbers 0 to 1 (multiply a light or a sky with LightScale)
//   Weather.WindSpeed ()             -> m/s, the steady wind with the gust of this moment on top
//   Weather.WindDirection ()         -> degrees about +Y the wind blows towards (0 is towards +Z, 90 towards +X); Weather.SetWindDirection (degrees) -> boolean
//   Weather.WindVector ()            -> vector: WindSpeed along WindDirection, for a flag, a leaf or a cloth
//   Weather.Lightning ()             -> number 0 to 1: the flash of a strike, 1 at the strike and gone within a second; Weather.Strikes () -> how many so far
//   Weather.SecondsSinceStrike ()    -> seconds since the last strike (a very large number before the first); Weather.ThunderDelay () -> seconds from that flash to its thunder
//   Weather.Wetness ()               -> number 0 to 1: how wet the ground is (rain wets it, snow a little, it dries over minutes)
//   Weather.SetAuto (on [, seed])    -> lets it change by itself (on) or holds it (off); a seed makes the changes, gusts and lightning repeatable
//   Weather.SetCold (cold)           -> in the cold rain and storms fall as snow
//   Weather.SetMeanDuration (seconds) -> the average time before it changes by itself (1 to 1000000); returns false if out of range
//
// A call with a wrong-typed argument raises a script error; without an active weather the getters return 0 / "" and Set returns false.

namespace aether::script {

void InstallWeatherApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
