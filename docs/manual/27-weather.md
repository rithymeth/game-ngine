# Weather

The weather kit is **the sky's mood as numbers**: how cloudy, how hard it rains or snows, how thick the fog, how hard the wind blows (with gusts), when lightning
strikes and how wet the ground is. It blends smoothly from one kind of weather to the next, by hand or on its own from a seed. Like the time-of-day clock, the
weather belongs to the game, not the scene: it keeps going through a scene change. The kit is an optional part of the build (`AETHER_KIT_WEATHER`, on by default).

The kit **only keeps the numbers**. What a scene does with them is the game's: a script scales a fog density with `Weather.Fog()`, dims the sun with
`Weather.LightScale()`, drives a rain emitter with `Weather.Rain()`, flashes a light with `Weather.Lightning()`, leans a flag with `Weather.WindVector()`.

## Kinds

| Kind | Clouds | Rain | Snow | Fog | Wind (m/s) | Sun let through | Lightning (per min) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `clear` | 0.05 | 0 | 0 | 0 | 2 | 1.0 | 0 |
| `partly_cloudy` | 0.45 | 0 | 0 | 0.05 | 4 | 0.85 | 0 |
| `overcast` | 0.85 | 0 | 0 | 0.15 | 5 | 0.55 | 0 |
| `rain` | 0.9 | 0.6 | 0 | 0.3 | 8 | 0.4 | 0 |
| `storm` | 1.0 | 1.0 | 0 | 0.45 | 18 | 0.25 | 6 |
| `fog` | 0.7 | 0 | 0 | 1.0 | 1 | 0.5 | 0 |
| `snow` | 0.9 | 0 | 0.7 | 0.35 | 4 | 0.5 | 0 |

## Changing it

`Weather.Set("storm", 20)` blends to a kind over 20 seconds (default 30; 0 is at once; up to 3600) with a smooth step, so it eases in and out. A blend that is
interrupted carries on from where the numbers are, never jumping. `Weather.SetAuto(true, seed)` lets it change by itself: after 0.5 to 1.5 times the mean duration
(`Weather.SetMeanDuration`, 300 s by default) it picks what usually comes next (clear tends to become cloudy, rain to a storm or clearing; nothing follows itself).
`Weather.SetCold(true)` makes rain and storms fall as snow, and snow needs the cold.

The weather moves in fixed steps of 25 ms whatever the frame rate, so **the same seed gives the same weather at 30 or 144 fps**.

## Wind, lightning and wet ground

- **Wind.** `WindSpeed()` is the steady wind of the current weather with a gust on top (never below 0); `WindDirection()` is the way it blows towards in degrees about
  +Y (0 towards +Z, 90 towards +X, set with `SetWindDirection`); `WindVector()` is the two together.
- **Lightning.** Strikes arrive at random while the weather's rate is above 0 (the storm: six a minute). `Lightning()` is the flash, 1 at the strike and gone within a
  second; `Strikes()` counts them; `SecondsSinceStrike()` and `ThunderDelay()` (0.9 to 8.7 s, from a strike 0.3 to 3 km away) let a script play the thunder late.
- **Wet ground.** `Wetness()` rises in rain (steady full rain soaks the ground in 30 s) and a little in snow, and falls by itself over about 300 s when it is dry.
  Use it to darken and gloss surfaces.

```lua
Weather.SetAuto(true, 1234)                      -- the weather changes by itself, the same way every run
if Weather.Rain() > 0.3 then Audio.PlaySound2D("Audio/rain_loop.acue") end
local flash = Weather.Lightning()                -- 0 to 1: add it to a light's intensity for the flash
```

## In the player

The player owns the weather beside the clock and advances it in a frame stage, `Player.Weather`, in the Update phase after `Player.Update` and before the scripts, so a
script sees this frame's weather.
