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

Kael can take salvage (E or X: the nearest thing tagged Salvage within 3 m goes into his pack; `Interactor.luau`). The pack is kept in SaveGames and written to the `autosave` slot at every change, so it survives a door and the next run. A HUD (`Content/UI/Hud.aui`) shows the pack and a `[E] Take` prompt for what is in reach. MULE-3 (`Mule3.luau`) starts broken: Interact beside it spends 3 components from the pack, then it follows Kael, and Interact beside it toggles follow / hold. The UI font (`Content/fonts/`) has to be in the package for any UI text to draw.

Not yet: most gameplay (scanner, prompts, MULE-3, crafting, power, missions, saves: the first sprint of DESIGN.md section 22), most interiors, audio, HUD. One interior exists (the Assembly Hall, reached by the door on the Iron Wastes hall; its exit puts Kael back outside the hall); the living dome and the reactor hall are next.

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
