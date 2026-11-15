#pragma once

#include "aether/script/luau_host.h"

// Luau's `TimeOfDay` table: the game's world clock (the time-of-day kit). Present only when the kit is built. The clock belongs to the game, so it
// keeps running through a scene change.
//
//   TimeOfDay.GetHour ()              -> number, 0 up to 24 (7.5 is 07:30)
//   TimeOfDay.SetHour (hour)          -> boolean: false (and nothing changes) if hour is not a finite number; it wraps into 0..24
//   TimeOfDay.GetDay ()               -> number, the day count (0 on the first day)
//   TimeOfDay.SetDay (day)            -> boolean: false if day is negative or not a whole number
//   TimeOfDay.GetTotalHours ()        -> number: day x 24 + hour, to measure how long ago something happened
//   TimeOfDay.GetPhase ()             -> "night", "dawn", "day" or "dusk" (dawn 5-8, day 8-18, dusk 18-21, night 21-5)
//   TimeOfDay.IsNight ()              -> boolean
//   TimeOfDay.Sunlight ()             -> number: 0 at midnight, 1 at noon, smooth between; scale a light or a sky with it
//   TimeOfDay.GetTimeText ()          -> string such as "07:05", for the HUD
//   TimeOfDay.GetDayLength () / SetDayLength (seconds) -> real seconds per game day (1 to 1000000); Set returns false if out of range
//   TimeOfDay.GetScale () / SetScale (scale)           -> speed multiplier (0 pauses, up to 1000); Set returns false if out of range
//
// A call with a wrong-typed argument raises a script error; without an active clock the getters return 0 / "" / false.

namespace aether::script {

void InstallTimeOfDayApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
