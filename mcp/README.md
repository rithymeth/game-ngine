# Aether MCP server

A [Model Context Protocol](https://modelcontextprotocol.io) server that lets an
AI agent (Claude Code, Claude Desktop, any MCP client) drive the Aether
editor's scene: inspect, create, edit and delete entities and components,
reparent, undo/redo, save and load scenes, and use Play-in-Editor.

Every change goes through the editor's undoable command stack
(`editor/src/core/commands.h`), so an agent's edits can be undone with the same
history a human uses. It is headless -- no window, graphics backend or ImGui --
and talks JSON-RPC over stdio.

## Use it

```
cmake --build <build-dir> --target aether_mcp_server
claude mcp add aether -- <build-dir>/mcp/aether_mcp_server [--scene scene.json]
```

stdout carries the protocol only; engine logging below Error is switched off
and errors go to stderr.

## Tools

| Tool | What it does |
| --- | --- |
| `editor_state` | Entity count, undo/redo labels, unsaved changes, play state |
| `list_component_types` | Component names and their fields |
| `list_entities` | GUIDs and component names; optional component filter |
| `get_entity` | Every component of one entity, as JSON |
| `create_entity` | New entity with optional components and parent (all-or-nothing) |
| `destroy_entity` | Delete an entity |
| `add_component` / `remove_component` | Add (with optional initial values) or remove a component |
| `set_component` | Partial update: only the fields named change |
| `reparent` | Parent or detach an entity; rejects cycles |
| `undo` / `redo` | Walk the editor history |
| `save_scene` / `load_scene` | `.json` (readable) or `.aesc` (binary); load can `replace` |
| `play` / `pause` / `stop` | Play-in-Editor; stop restores the scene exactly. `play` starts the simulation systems |
| `list_systems` | The simulation systems in execution order, with enabled state (play mode) |
| `set_system_enabled` | Turn one system on or off |
| `step_simulation` | Run N frames (1-10000) of `dt` seconds through the fixed-timestep frame loop |
| `set_platformer_input` | Move/jump input for 2D platformer controllers |

### Assets, import and cook

These use the engine's own asset database, importers and cooker, in process, on a project opened with
`project_open`. The cooked archive can go straight into `game_load`.

| Tool | What it does |
| --- | --- |
| `project_open` / `project_info` / `project_set` | Open an `.aproject` (scans `Content/`, giving new files GUIDs); read or change startup scene, `always_cook`, plugins |
| `asset_list` / `asset_info` / `asset_references` | Browse and search content by folder/name/type; one asset's settings, dependencies and referencers; the reference graph |
| `asset_add_files` | Copy source files (textures, models, audio) into `Content/<folder>`, scan and import them |
| `asset_import` | (Re)import one asset, or everything pending; served from the derived-data cache when unchanged |
| `asset_set_import_settings` | Merge importer settings into the `.ameta` (e.g. a texture's `cook_format`) and reimport |
| `asset_move` / `asset_create_folder` / `asset_delete` | Move/rename (GUIDs and references survive; glTF URIs rewritten), make folders, delete (refused while referenced unless `force`) |
| `content_read` / `content_write` | Read and write text files under `Content/` (scenes, prefabs, materials, JSON assets); paths cannot leave `Content/`, `.ameta` is not writable |
| `asset_scan` | Rescan after changing files outside the tools |
| `asset_cook` | Cook into an `.apak` (configuration, extra roots, compression, strict, encryption, patch); returns the archive path, sizes, warnings and the cooked assets |

### Gameplay kit data (`kit_*`)

The definitions a game is built from: effects (`.aeffect`), abilities (`.aability`), items (`.aitem`),
quests (`.aquest`) and input actions and mappings (`.aaction`, `.amapping`). They use the kits' own
parsers and validators, so what they accept is what the game loads, and they work on the project opened
with `project_open`.

| Tool | What it does |
| --- | --- |
| `kit_types` | The types, their extensions, and how many the project has |
| `kit_schema` | One type's shape: a sample with every field set, the allowed words and cross-reference rules, the blank definition, and an example from the project |
| `kit_validate` | Check a definition without writing it: ok, or the kit's named error (`effect.bad_duration: ...`) |
| `kit_put` | Validate, then write normalized JSON under `Content/` (extension added) and scan it; nothing is written if it is invalid |
| `kit_list` / `kit_get` | The project's definitions with their names and validity; read one |
| `kit_check` | Every definition must parse and validate, names must be unique, and references must resolve: an ability's cost and cooldown effects, an item's use and equip effects, a quest's reward effects, reward items and prerequisites |

**Attributes.** There is no attribute-definition file: an entity's stats (Health, Mana, ...) are the
`AttributeSet` component's data, saved in scenes and prefabs. So the attribute tools work on that:
`attribute_list`, `attribute_define` (a list of `{name, base, min?, max?}`, adds the component if needed, one
undoable step, bounds applied as the game does) and `attribute_remove` on editor entities;
`attribute_project` lists what the project's scenes and prefabs define; `game_attributes` /
`game_set_attribute` act on the running game. `kit_check` warns (it cannot know about scripts) when an effect
modifies an attribute no scene or prefab defines. A hand-written scene should save `current` beside `base`:
the game only computes `current` when an attribute changes, so without it the value starts at 0
(`attribute_project` warns about this; `attribute_define` always saves it).

**Interaction.** There is no interaction asset either: what can be used is the `Interactable` component
(prompt, range, required/blocked tags, an effect and ability applied to the user, one-shot, cooldown), saved
in scenes and prefabs. `interactable_set` (validated, one undoable step, adds the component if needed),
`interactable_list` and `interactable_remove` edit editor entities; `interaction_project` lists the project's;
`kit_check` verifies that each one's effect and ability exist. The `game_interact*` tools above drive the
running game. Reasons use the names scripts hear (`out_of_range`, `missing_tag`, `blocked_tag`, `cooldown`,
`used`, `disabled`, `action_failed`).

**Audio.** Sound cues (`.acue`) are a real asset type, so they go through the same `kit_*` tools
(`type: "sound_cue"`: schema with a sample, validate, put, get, list, check) plus two cue-specific ones:
`cue_validate` returns every diagnostic with its code (CU001...), severity and node, and checks each Wave's sound
against the open project's Sound assets; `cue_preview` evaluates the cue graph (Random picks, Sequence order,
Modulator ranges, Loops, Delays) and lists which sounds it would play, when, how loud and at what pitch, over
several plays with a repeatable `seed`, with no audio output. Sound lengths come from the project's `.wav`
files (other formats are assumed to last a second, and listed). `audio_set` / `audio_list` / `audio_remove`
edit the `AudioSource`, `AudioListener` and `ReverbZone` components on editor entities (validated, one undoable
step), and `kit_check` also verifies each cue (with its sounds) and that every scene's `AudioSource` names a
cue that exists. The `game_audio_*` tools above drive the running game.

**VFX.** Particle effects (`.avfx`) are an emitter of modules (Spawn, Initialize, Update, Render) plus declared
parameters. `vfx_schema` returns a sample effect and the default fields of every module; `vfx_validate` gives
the module's own diagnostics (FX001...); `vfx_put` / `vfx_get` / `vfx_list` write, read and list effects under
`Content/` (refusing an effect with error diagnostics); `vfx_check` validates them all and verifies that every
scene's `ParticleSystem` component names an `.avfx` that exists; `particle_set` / `particle_list` /
`particle_remove` edit the `ParticleSystem` component on editor entities (validated, one undoable step).
`vfx_simulate` runs an effect headless, with no GPU: particle counts over time per emitter, the peak, how many
were born, when a one-shot finishes, the bounds of what is alive and a sample of particles (position, size,
colour, age). The same effect, seed and steps always give the same result, and `parameters` sets the effect's
declared parameters first (a bound `Intensity` really changes the spawn rate).

Two limits of the engine, not of these tools: `.avfx` is not an asset-database type (no importer, so no GUID and
it is never cooked), and the player does not run `ParticleSystem` components, so there are no `game_*` tools
for particles; an effect plays only in the editor's VFX tools or in anything that hosts a `vfx::ParticleWorld`.

### Terrain workbench (`terrain_*`)

The terrain module is a standalone library: the engine has no terrain component, no terrain asset type and no
terrain in the player. So these tools keep **one terrain in the server's memory** (a workbench), separate from any
scene or game, to author and analyse a heightmap terrain with the module's own brushes and math.

| Tool | What it does |
| --- | --- |
| `terrain_create` | A new terrain: width x depth samples, cell size, flat or procedural (the engine's fixed-pattern fractal noise, heights in -scale..scale), vertical scale, chunk size, 1-4 paint layers (default Grass, Rock, Dirt) |
| `terrain_info` / `terrain_sample` | Size, height range, chunk layout, layers; height, normal, slope and layer weights at world points |
| `terrain_sculpt` | raise / lower / smooth / flatten / paint along a list of strokes with the editor's brush (radius, strength, falloff, amount); one undoable step per call |
| `terrain_undo` / `terrain_redo` | Up to 20 steps |
| `terrain_preview` | A top-down PNG returned as an image (and optionally saved): shaded, layers, slope or height; foliage shows as dark dots |
| `terrain_chunks` | The render plan for a camera: the chunk grid, each chunk's LOD by distance, vertex and triangle counts per LOD, total triangles |
| `terrain_scatter_foliage` / `terrain_clear_foliage` | Scatter foliage by spacing, density, height and slope limits and optional noise; counts, bounds, per-type breakdown |
| `terrain_export` / `terrain_import` | Save and load as `<path>.r16` (16-bit heights), `<path>.splat` (RGBA8 weights) and `<path>.terrain.json` |

The export files are this server's own format for keeping the workbench; nothing else in the engine reads them.
Foliage and undo history are not saved. World heights are the stored heights times `vertical_scale`.

### Navigation workbench (`nav_*`)

A navigation workbench in the server's memory: describe the ground, bake a mesh with Recast, query it with
Detour, look at it, and run a crowd over it. The player does not run navigation (no `NavWorld` or `NavCrowd` in
the cooked game), so this is separate from the hosted game. Geometry from `ModelRenderer` entities is not
available (it needs the model assets): describe the ground with shapes and triangles, the terrain workbench, or
the scene's navigation components.

| Tool | What it does |
| --- | --- |
| `nav_geometry_add` | Planes, boxes, triangles, volumes (relabel the ground: area 0 blocks it, others are water, mud, road) and off-mesh links; all or nothing |
| `nav_geometry_add_terrain` / `nav_geometry_add_scene` | The terrain workbench's heightmap as ground; the editor scene's `NavObstacle`, `NavModifierVolume` and `NavLinkProxy` as volumes and links |
| `nav_geometry_clear` / `nav_info` | Empty the geometry; geometry counts, the last bake's settings, tiles, polygons, links |
| `nav_bake` | Recast bake for an agent size (height, radius, max climb, max slope, cell size, tile size); stats and time |
| `nav_path` | Up to 50 shortest paths with `area_costs` and `excluded_areas`: status (complete, partial, none), length, corners, off-mesh link starts |
| `nav_query` | nearest point on the mesh, raycast along the mesh, random points (repeatable seed, optionally near a centre), reachability |
| `nav_preview` | Top-down PNG returned as an image: polygons by area, links, optional routes and points |
| `nav_simulate` | A crowd of `NavAgent`s walks to their goals on the engine's own Detour crowd: arrival times, distance walked against the shortest path, outcomes, the closest any two agents came, sampled trajectories |
| `nav_export` / `nav_import` | The mesh as a `.anav` file (the engine's format) |
| `nav_component_set` / `nav_component_list` / `nav_component_remove` | Edit `NavAgent`, `NavObstacle`, `NavModifierVolume` and `NavLinkProxy` on editor entities, validated, one undoable step |

Detour joins an off-mesh link only inside a tile or between neighbouring tiles, so bake with `tile_size: 0` for
long links.

Enum values are lowercase words (`"instant"`, `"count"`); `kit_schema` lists them. Write definitions,
`kit_check` them, `asset_cook`, then `game_load` to play them.

### Running a cooked game (`game_*`)

The editor tools edit the authored scene. The `game_*` tools instead host a full cooked
`Game` (the `player/` runtime) headless, so **every system runs**: 3D and 2D physics, Luau scripts,
Blueprints, audio, sequences, the gameplay kits (abilities, effects, inventory, interaction, quests),
saves and localization. The game has its own world, separate from the editor's.

| Tool | What it does |
| --- | --- |
| `game_load` | Mount `.apak` archives (or a folder of them / a loose cook), load the startup scene or `scene`, start modules, begin play |
| `game_step` | Run N frames (1-100000) of `dt` seconds (default: the project's fixed step) |
| `game_state` | Frames, time, entity/physics/script counts, script and Blueprint errors, load warnings |
| `game_input` | Hold keys/axes (`{"W": 1}`), mouse delta for one frame, or clear |
| `game_screenshot` | Render the current frame off screen (what the player draws: scene and HUD) and return it as a PNG image, also saved to `path`. Needs a graphics device (D3D12 or Vulkan); `width`, `height`, `backend`, `inline` |
| `game_attributes` / `game_set_attribute` | An entity's attributes in the running game; set a base, add a delta or define one with bounds, through the game's own attribute system (events fire, bounds apply) |
| `game_interactables` / `game_can_interact` / `game_interact` / `game_interaction_focus` / `game_set_interactable` | The interaction kit in the running game: list usable things (and why an interactor can't use each), check, use (range, tags, cooldown, effect and ability apply, OnInteract is queued for the target's script), find what would be prompted, enable/disable/reset |
| `game_audio_state` / `game_play_sound` / `game_set_bus` / `game_stop_sounds` / `game_audio_source` | The audio kit in the running game: voices, cues playing, listener, reverb, every bus and the cue problems found; play a cue in 2D or at a location; set a bus's volume (faded) or mute; stop everything; play/stop/fade/set volume or cue on an entity's AudioSource |
| `game_set_paused` | Freeze or resume the simulation |
| `game_load_scene` | Switch to another cooked scene |
| `game_systems` | The game's systems in execution order by phase (filled after the first step) |
| `game_list_entities` / `game_get_entity` | Inspect the running world (filter by component or tag) |
| `game_set_component` / `game_destroy_entity` | Change the live world (not undoable; gone on reload) |
| `game_unload` | Stop and unload |

The plugin modules Audio, Navigation, Networking and Physics are linked into the server so manifests
that name them start; AI is not (see below).

### Building, testing and running the engine

These tools act on one CMake build directory: `--build-dir <dir>`, else `$AETHER_BUILD_DIR`, else the
nearest `CMakeCache.txt` above the server executable (the server is built inside it). On Windows an
MSVC build gets the Visual Studio environment automatically (from the `vcvars64.bat` beside the
compiler the cache names). Calls block until the process ends or times out; a timeout kills the whole
process tree.

| Tool | What it does |
| --- | --- |
| `build_info` | Build dir, source dir, generator, build type, compiler and the `AETHER_BUILD_*` / `AETHER_KIT_*` options |
| `configure` | Run CMake, optionally setting cache variables |
| `build` | `cmake --build`, all or one `target`; returns success, compiler errors, warning count, output tail |
| `build_targets` | Target names, optionally filtered |
| `run_tests` | Run `aether_tests` (optional `filter`, optional `build_first`); pass/fail counts and each failure with its output |
| `run_program` | Run any program inside the build dir (`aether_player --headless --frames N`, the functional runner, the cooker) with args, environment and timeout; no shell |

Entities are addressed by GUID (from `list_entities`), which survives
undo/redo and save/load. Component values use the engine's reflection JSON
(`Vec3` is `[x, y, z]`, entity references are GUID strings).
Bad input is reported as a tool error (`isError`), never a crash, and a failed
`create_entity` leaves the scene untouched.

## Layout

- `mcp_server.h/.cpp` -- the protocol: initialize, ping, `tools/list`,
  `tools/call`; transport-independent `HandleMessage`, plus the stdio loop.
- `editor_tools.h/.cpp` -- the tools above, over an `EditorSession` (world,
  GUID index, command stack, play session).
- `modules.cpp` -- registers every module's components (animation, audio, Blueprint, gameplay/GAS,
  navigation, renderer, sequencer, 2D, streaming, UI, VFX, and physics and the genre kits when built).
- `simulation.h/.cpp` -- the systems that run in play mode (3D physics and characters, 2D physics with
  platformers and camera follow, previous-transform recording) on the engine's scheduler.
- `game_tools.h/.cpp` -- the `game_*` tools over a hosted `player::Game`.
- `build_tools.h/.cpp`, `process.h/.cpp` -- the build/test/run tools and the child-process runner.
- `screenshot.h/.cpp` -- the off-screen renderer behind `game_screenshot`.
- `asset_tools.h/.cpp` -- the project, asset, import and cook tools.
- `kit_tools.h/.cpp`, `project_host.h` -- the gameplay-kit data tools, and the open-project state they share with the asset tools.
- `audio_tools.h/.cpp` -- cue validation and preview (audio components and live audio are in `editor_tools.cpp` and `game_tools.cpp`).
- `vfx_tools.h/.cpp` -- particle effect schema, validation, files, check and headless simulation (the `ParticleSystem` component tools are in `editor_tools.cpp`).
- `terrain_tools.h/.cpp` -- the terrain workbench.
- `nav_tools.h/.cpp` -- the navigation workbench (the nav component tools are in `editor_tools.cpp`); `image_util.h` -- PNG and base64 helpers for tools that return a picture.
- `main.cpp` -- the `aether_mcp_server` executable.
- Tests: `tests/test_mcp.cpp`.

Every module's components are registered at startup, so all of them can be listed, added and edited.
Gameplay AI (`Aether::AI`: behaviour trees, perception) is deliberately not linked: the editor/MCP side
must not depend on it (`docs/design/agent_gameplay_ai_boundary.md`).

The editor's `play` simulation (above) is deliberately small: physics only. For audio, scripting,
sequences and the gameplay kits, load the cooked game with `game_load`. The simulation itself is headless;
`game_screenshot` makes a hidden window and graphics device on first use, so it works only where one exists.
