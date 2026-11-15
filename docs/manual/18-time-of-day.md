# Time of day

The time-of-day kit is the game's world clock: an hour, a day count, a phase of the day and a sunlight level. It is an optional
part of the build (`AETHER_KIT_TIMEOFDAY`, on by default) and needs no other kit and no asset files. The clock belongs to the game, not to a
scene, so it keeps running when a script loads another scene.

## The clock

- A game day lasts **1200 real seconds** (20 minutes) by default, and the game starts at 8:00 on day 0.
- The clock moves with each frame's time. `SetScale` speeds it up (2 runs it twice as fast) or pauses it (0); `SetDayLength` changes how many real seconds a game day takes (1 to 1,000,000).
- The phase of the day: **dawn** 5:00 to 8:00, **day** 8:00 to 18:00, **dusk** 18:00 to 21:00, **night** 21:00 to 5:00.
- **Sunlight** is 0 at midnight, 1 at noon and smooth in between (0.5 at 6:00 and 18:00): scale a light or a sky with it.

What the time *does* to your game is up to your scripts: the kit keeps the time and answers questions about it, and does not move lights or spawn anything itself.

## From a script

```lua
local Sun = {}

function Sun:OnUpdate(dt)
    UI.SetText("ClockText", TimeOfDay.GetTimeText())          -- "07:05"
    local level = TimeOfDay.Sunlight()                         -- 0 to 1: drive a light with it
    if TimeOfDay.GetPhase() == "dusk" then
        -- lamps on
    end
end

return Sun
```

| Call | Result |
| --- | --- |
| `TimeOfDay.GetHour()` / `SetHour(hour)` | the hour, 0 up to 24 (7.5 is 07:30); `SetHour` wraps and returns `false` for a value that is not a finite number |
| `TimeOfDay.GetDay()` / `SetDay(day)` | the day count, 0 or more; `SetDay` returns `false` for a negative or fractional number |
| `TimeOfDay.GetTotalHours()` | day x 24 + hour, to measure how long ago something happened |
| `TimeOfDay.GetPhase()` / `IsNight()` | `"night"`, `"dawn"`, `"day"` or `"dusk"` / a boolean |
| `TimeOfDay.Sunlight()` | 0 to 1 |
| `TimeOfDay.GetTimeText()` | `"HH:MM"` for the HUD |
| `TimeOfDay.GetDayLength()` / `SetDayLength(seconds)` | real seconds per game day |
| `TimeOfDay.GetScale()` / `SetScale(scale)` | the speed multiplier (0 to 1000) |

A setter given a value it refuses changes nothing. The clock is not part of a save file by itself: a game that wants the time saved
writes `TimeOfDay.GetTotalHours()` into `SaveGames` and restores it with `SetDay` and `SetHour`.
