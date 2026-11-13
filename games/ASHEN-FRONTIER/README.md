# Ashen Frontier

A grounded science-fiction salvage RPG: crash-land on Vespera-9, an abandoned colony world now inhabited by autonomous machine societies; salvage, build an outpost,
recruit machines, and decide whether the returning humans have any right to reclaim the planet. Premium offline single-player, third-person, Windows PC.

Made with the Aether engine, and used to find what the engine is missing (see ENGINE_AUDIT.md). The design is in DESIGN.md (the supplied game design document,
converted); this file is where the build stands.

## Where it stands

**First open world, greybox.** One connected 480 m world of the four regions the design asks for, with the outpost at the crossroads:

| Region | Look | Points of interest |
|---|---|---|
| Fallen Basin (south-west) | oxidised red ground, canyon gullies, layered mesas and spires, dust haze | Crash Site, MULE-3 Wreck, Homestead Ruins, Salvage Field, Distress Relay, Storm Field |
| Iron Wastes (south-east) | grey-brown ground with rust, terraced mesas | Smelter Row, Rail Yard, Gantry Works, Buried Assembly Hall, Warden Checkpoint, Slag Pits |
| Verdant Array (north-west) | soft green hills over rock | Greenhouse Rows, Living Dome, Maintenance Depot, Repaired Garden, Research Annex |
| Hollow Core (north-east) | dark plateau with one long ramp up | Core Approach, Reactor Tower, Data Spine, Core Gate, Failing Grid |

The ground is one terrain (`tools/terrain.py`): rolling and terraced, with ridges between the regions and a natural pass through each, an irregular rim of mountains,
and flat clearings at every point of interest. Models are low-poly placeholders made by `tools/make_models.py` (Kael, MULE-3, SPARK-2, SENTINEL-4, a drone, the crashed
freighter, props, machines, mesas, spires, an arch), to be replaced by real art without touching the scenes. Kael walks it with the third-person controller on the engine's
new `HeightfieldCollider`.

Kael can take salvage (E or X: the nearest thing tagged Salvage within 3 m goes into his pack; `Interactor.luau`). What he takes stays taken (`taken.<scene>.<name>` in SaveGames): it does not come back after a door or a restart, and F9's new game brings it all back. The pack is kept in SaveGames and written to the `autosave` slot at every change, so it survives a door and the next run. A HUD (`Content/UI/Hud.aui`) shows the pack and a `[E] Take` prompt for what is in reach. MULE-3 (`Mule3.luau`) starts broken: Interact beside it spends 3 components from the pack, then it follows Kael, and Interact beside it toggles follow / hold. The fab bench (`Workbench.luau`) crafts a Generator Core from 4 scrap and 2 components; the outpost generator (`PowerGenerator.luau`) takes it and lights up (`power.outpost`, saved). The first chapter is a chain on the HUD's objective line: craft the core, power the generator, repair MULE-3 (it then stays repaired and with Kael through doors and restarts), reach the Distress Relay (`Beacon.luau`). F9 starts a new game (clears the saved progress and reloads the world). The UI font (`Content/fonts/`) has to be in the package for any UI text to draw.

Sentinels (`Sentinel.luau`) guard the Iron Wastes checkpoint, the assembly hall and the reactor hall: they notice Kael within 14 m, walk up and strike; Kael hits back (F, left mouse or B on a pad: 25 damage within 2.6 m), a disabled sentinel drops 2 components and stays gone (health is kept in SaveGames: `player.hp`, `enemy.<name>.hp`); at no health Kael is returned to the start. F9 clears all of it.

Music (`Music.luau`): a calm loop, and a tense one while an active sentinel is within 16 m of Kael; the old loop is faded out with `Audio.Stop` when the mood changes (the three loops are synthesized placeholders from `tools/make_audio.py`).

Sound effects (`Content/Audio`, written by `tools/make_audio.py`: short synthesized tones, placeholders) play for pickups, crafting, power, repairs, the relay, doors, strikes and hits, through the scripts' `Audio` table; the smoke test fails if a script asks for a sound that is not there.

The scanner (Q, or Y on a pad) lists what is near by category as words ([SALVAGE], [MACHINE], [ROBOT], [FABRICATION], [POWER], [RELAY], [HAZARD]) with distance, compass direction and known status, on the HUD.

Chapter 2 (after the relay): collect three glowing seed samples out in the Verdant Array (E, counted on the HUD) and load them into the irrigation console inside the living dome (`Console.luau`), which restores the garden. The scanner lists samples and the console too.

The chapters show in the world: the outpost lights come on when the generator is powered and the dome's glow rises when its console is restored (`FlagLight.luau`), and the garden inside the dome grows from saplings to full trees (`Growth.luau`); both follow a saved flag, so they hold through doors and restarts.

Chapter 3 (Iron Wastes, after the dome): disable the two warden sentinels at the checkpoint (the objective line counts them, and the scanner shows their health), then start the assembly line at the terminal in the assembly hall (`Terminal.luau`: locked while any named sentinel is active); the hall's work lights come up when it runs.

Three interiors exist, one for each of the reference images: the Assembly Hall (the door on the Iron Wastes hall), the Living Dome (the door on the Verdant Array dome) and the Reactor Hall (the door on the Hollow Core's gate); each exit puts Kael back outside its door.

Not yet: ranged combat, music and ambience, the other regions' missions (the rest of DESIGN.md section 22).

## Run it

Build the engine first (the repository's README), then:

```
python games/ASHEN-FRONTIER/tools/smoke_test.py                       # layout checks, cook, and a walk in the real player
games/ASHEN-FRONTIER/tools/render.sh Scenes/OpenWorld.ascene out.png   # one frame (cooks first)
```

On Windows, cook and package with the engine's tools:

```
aether_package games\ASHEN-FRONTIER\ASHEN-FRONTIER.aproject --out Build
```

Or open `ASHEN-FRONTIER.aproject` in the editor (Live Viewport: Start).

## What is generated, and what to edit

Scenes, models, the terrain and its texture, and the map are **generated** and committed; change the generator and re-run it, not the output.

| To change | Edit | Then run |
|---|---|---|
| where things are (regions, points of interest, spawn) | `tools/world.py` | `python tools/make_scenes.py` |
| the shape of the ground, its colours | `tools/terrain.py` | `python tools/terrain.py`, then `make_scenes.py` |
| a model | `tools/make_models.py` | `python tools/make_models.py` |
| what is placed on the world | `tools/world_scene.py` | `python tools/make_scenes.py` |

`Content/Data/world.json` (regions, points of interest, passes, spawn) and `Content/Textures/WorldMap.png` (the top-down map, an orthographic render of the whole
world) are what a map screen and fast travel will read. Re-make the map with `tools/render.sh Scenes/WorldMapTopDown.ascene Content/Textures/WorldMap.png 40 1024x1024`.

Other scenes: `FallenBasin` (the first, smaller level), `ModelGallery`, `ModelGalleryLarge` and `ModelFreighter` (to look at the models), `OpenWorldOverview`.

## Needs

Python 3 with numpy to generate (the committed output needs nothing); the engine's built player and cooker, and `xvfb-run` with a software Vulkan driver to run the
smoke test on Linux.
