# Woodland demo

A 48-second film of the engine's newer systems at work in the **Woodland**, the world the procedural editor bakes
(`ProceduralEditor_OptInBakeOfAFullSizeWorld`: oak, birch and dead trees over a bay). Everything that moves is driven by
a script through the engine's own kits; nothing is animated by hand.

| Time | What happens | Kit |
|------|--------------|-----|
| 0–12 s | Golden evening, a fly-in over the bay | Time of Day, Weather (clear) |
| 12–22 s | Clouds roll in, rain begins; a lantern swings on a rope from a gallows | Weather, Ropes, Force Fields (wind) |
| 24–34 s | The storm builds; a jeep drives down the slope on its own suspension | Weather, Vehicles |
| 35 s | The pile of crates is blown up and breaks into pieces | Force Fields, Destruction |
| 36–48 s | Lightning, rain and a pull back over the bay | Weather |

The crates, lantern, jeep body, oaks, grass, wildflowers and a ruined arch are generated with [Meshy](https://docs.meshy.ai) (`tools/meshy_assets.py`);
without them `build_demo.py` uses simple placeholder models and the film still runs.

## Layout

```
Content/Scripts/WeatherDriver.luau   maps Weather / TimeOfDay onto the sun, sky, fog, wind and rain emitters
Content/Scripts/DemoDirector.luau    the film's clock: weather cues, the car's route, the blast
Content/Fx/RainLight.avfx, RainHeavy.avfx
tools/build_demo.py                  bake -> clearing -> cook -> film -> encode
tools/meshy_assets.py                text-to-3D for the crate, lantern and jeep body
```

## Running it

You need a build tree (`aether_cook`, `aether_player`, `aether_tests`), `ffmpeg`, Pillow and, on Linux, Mesa's lavapipe
and `xvfb`.

```
python samples/woodland_demo/tools/meshy_assets.py --out /tmp/woodland/meshy        # optional; costs Meshy credits
python samples/woodland_demo/tools/build_demo.py --work /tmp/woodland --meshy-dir /tmp/woodland/meshy --size 1280x720
```

`--size 480x270` makes a quick look. The bake is about 300 MB and the render is slow on a software renderer (seconds per
frame), so neither the bake, the Meshy models nor the video are committed.

The film is deterministic: the player advances game time by one film frame per frame, so physics and weather follow the
film's clock. That is why `CAR_TRACK` in `build_demo.py` (the jeep's route, used to aim the camera) repeats from run to run.
After changing the car's script or the site, rerun once and update it from the `[demo] car at` lines in `player.log`.

## Meshy key

`meshy_assets.py` sends no key itself. Give it one with `MESHY_API_KEY`, or through a proxy that injects it. Never paste a
key into a chat or commit it.
