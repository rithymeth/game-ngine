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
claude mcp add aether -- <build-dir>/mcp/aether_mcp_server [--scene scene.json] [--scene-root folder]
```

stdout carries the protocol only; engine logging below Error is switched off
and errors go to stderr.

## Tools

| Tool | What it does |
| --- | --- |
| `editor_state` | Entity count, undo/redo labels and attribution, unsaved changes, play state |
| `list_component_types` | Component names and their fields |
| `list_entities` | Paged results (default 200, max 1000) with `offset`, `after_entity_index`/`next_entity_index`, total matches, and `has_more`; includes GUIDs, names (null when unnamed), entity indices, component names, parent GUIDs, effective layer indices, local active state, and active/layer/parent-override presence; optional exact-name or case-insensitive ASCII name-substring/component/parent/parent_any/parent_none/root/layer/active/active-override/layer-override/parent-override/tag filters, `components_all`/`components_any`/`components_none` component groups, and `tags_all`/`tags_any`/`tags_none` tag groups (supplied filters combine with AND). The index cursor is not a scene snapshot. |
| `get_entity` | Every component of one entity, as JSON |
| `get_entities_many` | Get full reflected component values for up to 64 unique entities in selection order; validates all GUIDs before returning data |
| `get_entities_metadata_many` | Get names, active/layer overrides, layer indices, tags, and parent links for up to 64 selected entities |
| `get_components_many` | Read up to 64 entity/component pairs in request order; reports absent components explicitly and validates the whole request first |
| `compare_entity_components_many` | Compare up to 64 entities across up to 32 component types; reports per-entity presence plus common, mixed, and absent types |
| `summarize_entity_components_many` | Count each reflected component across up to 64 entities and list the types shared by all selected entities |
| `get_entities_world_transforms_many` | Get parent-composed world position, basis-length scale magnitudes, and column-major matrices for up to 64 unique entities |
| `get_entities_descendants_many` | List descendants for up to 64 roots in deterministic parent-first order with parent and relative-depth metadata; caps output at 64 entries |
| `get_entities_ancestors_many` | List parent chains for up to 64 entities, nearest parent first, with distance metadata; validates parent links and caps output at 64 entries |
| `validate_scene_hierarchy` | Audit scene-wide GUID and parent integrity, cycles, and excessive depth; returns up to 128 issues |
| `validate_scene_transforms` | Audit finite local and derived world transforms, cycles, and excessive depth; returns up to 128 issues |
| `validate_scene_components` | Audit component reflection and scene persistence support, reporting serialization errors and unsafe raw types |
| `validate_scene_tags` | Audit scene tags for empty names and duplicate exact, case-sensitive names |
| `validate_scene_names` | Audit entity names for empty values and leading or trailing ASCII whitespace; duplicate names remain allowed |
| `validate_scene_layers` | Audit project layer names and scene assignments for duplicates, empty names, unconfigured slots, and out-of-range indices |
| `rename_entities` | Rename up to 64 unique entities as one undoable action; creates `EntityName` when needed |
| `set_entity_tags_many` | Replace the complete tag list on up to 64 unique entities as one undoable action; creates `Tags` when needed |
| `modify_entity_tags_many` | Add/remove tags on up to 64 unique entities as one undoable action; preserves tag order and reports no-op edits |
| `set_entity_layers_many` | Set collision layer indices (0–31) on up to 64 unique entities as one undoable action; creates `Layer` when needed |
| `offset_entities_local_many` | Add one local-position offset to up to 64 unique entities in one undo action; preserves rotation and scale |
| `offset_entities_world_many` | Add one world-position offset to up to 64 unique entities in one undo action; resolves selected ancestor transforms |
| `offset_entities_world_positions_many` | Apply individual world-position offsets to up to 64 unique entities in one undo action; resolves selected ancestor transforms |
| `offset_entities_local_per_entity_many` | Apply individual local-position offsets to up to 64 unique entities in one undo action |
| `multiply_entities_local_scale_many` | Multiply local scale by one shared component-wise factor on up to 64 unique entities in one undo action |
| `multiply_entities_local_scale_per_entity_many` | Multiply local scale using individual component-wise factors for up to 64 unique entities in one undo action |
| `scale_entities_local_many` | Multiply local scale using per-entity component-wise factors for up to 64 unique entities in one undo action |
| `set_entities_local_position_many` | Set one shared absolute local position on up to 64 unique entities in one undo action; preserves rotation and scale |
| `set_entities_local_positions_many` | Set individual local positions on up to 64 unique entities in one undo action; preserves rotation and scale |
| `set_entities_world_positions_many` | Set individual world positions on up to 64 unique entities in one undo action; converts through parents and rejects selected ancestor pairs |
| `set_entities_world_position_many` | Set one shared world position on up to 64 unique entities in one undo action; resolves final parent transforms for selected hierarchies |
| `set_entities_world_rotation_many` | Set one shared normalized world quaternion on up to 64 unique entities in one undo action; resolves final parent orientations for selected hierarchies |
| `set_entities_world_rotations_many` | Set individual world quaternions on up to 64 unique entities in one undo action; resolves final parent orientations for selected hierarchies |
| `set_entities_world_scales_many` | Set per-entity world-scale magnitudes on up to 64 unique entities in one undo action; uses world-basis column lengths and preserves local mirror signs |
| `set_entities_world_scale_many` | Set one world-scale magnitude vector on up to 64 unique entities in one undo action; uses world-basis column lengths and preserves local mirror signs |
| `multiply_entities_world_scale_many` | Multiply world-scale magnitudes by one shared factor for up to 64 unique entities in one undo action; derives each target from original world scale |
| `multiply_entities_world_scale_per_entity_many` | Multiply world-scale magnitudes by per-entity factors for up to 64 unique entities in one undo action; derives each target from original world scale |
| `offset_entities_world_scale_many` | Add one shared delta to world-scale magnitudes for up to 64 unique entities in one undo action; derives each target from original world scale |
| `offset_entities_world_scale_per_entity_many` | Add per-entity deltas to world-scale magnitudes for up to 64 unique entities in one undo action; derives each target from original world scale |
| `rotate_entities_world_many` | Apply per-entity world quaternion deltas on up to 64 unique entities in one undo action; resolves selected ancestor orientations |
| `rotate_entities_world_shared_many` | Apply one world quaternion delta to up to 64 unique entities in one undo action; resolves selected ancestor orientations |
| `set_entities_local_rotation_many` | Set one shared absolute local quaternion on up to 64 unique entities in one undo action; preserves position and scale |
| `set_entities_local_scales_many` | Set individual local scales on up to 64 unique entities in one undo action; preserves position and rotation |
| `set_entities_local_rotations_many` | Set individual local quaternions on up to 64 unique entities in one undo action; preserves position and scale |
| `rotate_entities_local_many` | Apply per-entity local quaternion deltas to up to 64 unique entities in one undo action |
| `set_entities_active_many` | Set active states on up to 64 unique entities as one undoable action; missing `Active` means active, disabling adds `Active` |
| `create_entity` / `create_entities` | Create an entity or up to 64 entities with component values, current-scene parents, or zero-based `parent_index` links within the batch; the batch validates the full hierarchy before creation and is one undo action |
| `duplicate_entity` / `duplicate_entities` | Copy reflected component values for one entity or up to 64 unique entities as one undo action; `duplicate_entities` can preserve links within the selected group with `preserve_hierarchy: true`, duplicate selected entities and all descendants with `include_children: true`, and optionally remap internal EntityGuid references with `remap_internal_references: true` |
| `destroy_entity` / `destroy_entities` | Delete one entity or up to 64 unique entities as one undoable action; the batch can optionally include all descendants with `include_children: true`, deletes child-first, and restores original GUIDs on undo |
| `add_component` / `add_components_many` / `remove_component` / `remove_components_many` | Add one component or components across up to 64 unique entities as one undo action after validating all additions; remove one component or up to 64 selected entity/component pairs as one confirmed undo action |
| `reset_components_many` | Reset up to 64 unique entity/component pairs to reflected defaults in one undo action; validates the whole selection and keeps components attached |
| `copy_components_many` | Copy selected reflected components from one source entity to up to 64 target entities, with a 64-pair selection cap and one undo action |
| `set_component` / `set_components_many` | Partial component update; batch applies patches to up to 64 unique entities as one undo action after validating every patch |
| `reparent` / `reparent_many` | Parent or detach entities; `reparent_many` supports up to 64 unique per-entity moves or a selected entity list with one shared parent, in one undo action after validating the final hierarchy for cycles |
| `undo` / `redo` | Walk the editor history and return the source (`agent`) recorded with that action |
| `save_scene` / `load_scene` | `.json` (readable) or `.aesc` (binary); load can `replace` |. They stay inside the scene folder (the working folder, or `--scene-root`): relative paths resolve there, and absolute paths, `..` and symlinks that leave it are refused
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
| `project_open` / `project_info` / `project_set` | Open an `.aproject` (scans `Content/`, giving new files GUIDs); read or change startup scene, `always_cook`, plugins; setting changes are agent-attributed and undoable |
| `project_templates` | The project templates `project_create` can start from, with what each contains. |
| `project_create` | Makes a new project in `<parent_dir>/<name>/` (optionally from a template: a startup scene, controller script, input bindings and a Blueprint) and opens it. |
| `asset_list` / `asset_info` / `asset_info_many` / `asset_references` / `asset_references_many` | Browse and search content by folder/name/type; inspect one asset or up to 32 unique assets in one bounded response; view one or many bounded dependency/referencer graphs |
| `asset_add_files` | Copy source files (textures, models, audio) into `Content/<folder>` as one agent-attributed undoable action; undo restores overwritten sources and `.ameta` sidecars, and import targets only the submitted files |
| `asset_import` | (Re)import one asset, a selected `assets` array (up to 128) as one grouped undoable action, or everything pending; selected items return individual results and undo restores import metadata and sub-asset registrations (immutable derived-cache entries remain reusable) |
| `asset_set_import_settings` | Merge importer settings into the `.ameta` (e.g. a texture's `cook_format`) as one agent-attributed undoable action; undo/redo restores metadata and reruns the selected import |
| `asset_set_import_settings_many` | Apply settings merges and label replacements to up to 64 unique assets as one undoable action; reimport can be enabled or deferred for the whole batch |
| `asset_move` / `asset_create_folder` / `asset_delete` | Move/rename assets or folders as agent-attributed undoable actions (GUIDs and references survive; glTF URIs rewritten), make folders as undoable actions (undo only while empty), delete with source and metadata backed up under `Saved/` for undo (refused while referenced unless `force`) |
| `asset_move_many` | Move up to 64 independent assets or folders as one undoable action; overlapping paths are rejected before moving, with a result for each move |
| `asset_create_folders` | Create up to 64 folders under existing Content/ parents as one undoable action; all names and paths are checked before creation |
| `asset_delete_folders` | Delete up to 64 empty Content/ folders as one undoable action; refuses the Content/ root, non-empty folders, and paths outside the content tree |
| `asset_delete_many` | Delete up to 64 selected assets as one undoable action; mutual references within the selection are allowed, while external referencers require `force` and are returned explicitly |
| `content_read` / `content_read_many` / `content_write` / `content_write_many` | Read text files under `Content/`; `content_read_many` reads up to 64 unique paths with a 1 MB aggregate response cap; write/overwrite files up to 2 MB as agent-attributed undoable actions with source and GUID sidecars restored on undo/redo; `content_write_many` writes up to 64 files (128 MB total undo snapshot limit) as one undo action after validating the full batch; paths cannot leave `Content/`, `.ameta` is not writable |
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
| `kit_put` | Validate, then write normalized JSON under `Content/` (extension added) as one agent-attributed undoable action; undo/redo restores both the source and its GUID sidecar |
| `kit_validate_<type>` / `kit_put_<type>` | Type-specific forms generated from the registered kit types (for example, `kit_validate_effect` and `kit_put_effect`); omit the `type` argument, and the write endpoint still validates, normalizes and creates one agent-attributed undoable action |
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

### Blueprints (`bp_*`)

Blueprints (`.abp`) are the engine's visual scripting. They are a real asset type, so they go through the same
`kit_*` tools (`type: "blueprint"`: schema with a working sample, validate, put, get, list, check; a Blueprint
with a validation error is refused by `kit_put` and reported by `kit_check`). The Blueprint tools add what an
agent needs to write a graph and test it:

| Tool | What it does |
| --- | --- |
| `bp_node_types` | The node types you can use (id, title, category), searchable by id, title or category and filterable by category; give a Blueprint to include its own variables, functions and custom events |
| `bp_node_info` | A node type's pins: name, direction, type (exec, bool, int, float, string, Vec3, Quat, Entity, a struct, `Array<T>`) and default, plus its title, category and kind |
| `bp_validate` | The Blueprint module's own diagnostics (BP001...): code, severity, graph, node, pin, message |
| `bp_compile` | Compile to bytecode: diagnostics, each event's and function's instruction count, optionally the disassembly |
| `bp_run` | Run it headless on a test entity: BeginPlay, then your `events` (with arguments), then N ticks; returns what Print String printed, every variable's final value, runtime errors (BP201-BP203...), instructions run, game time and pending latent actions |

Give the Blueprint as `definition` (JSON) or `asset` (a path in the open project). `bp_run` has no physics
or world around the test entity, so traces find nothing, and a runaway loop stops at the instruction budget
(BP202) instead of hanging.

### Localization (`loc_*`)

The project's string tables (`.astrings`: CSV with a column per language), the strings its content uses, and the
loc module's own message format, plural rules and pseudo-localization.

| Tool | What it does |
| --- | --- |
| `loc_status` | The tables, key count and each language's completeness, fallback chain and direction, with the first missing keys |
| `loc_get` / `loc_set` / `loc_remove` | Read a key in every language; add or change translations (a new language becomes a column; language tags are normalized, so `pt_br` is `pt-BR`; an empty text removes one) or delete a key; the `.astrings` file is made on first use and scanned |
| `loc_validate` | Every translation must use the same placeholders as the source text, and every message must be well-formed (no unclosed brace); errors name the key and language |
| `loc_gather` | The strings the content uses (Text widgets' `text_key`, `LocText` values, Blueprint `Localize` nodes with literal keys) against the table: new keys and unused keys; changes nothing |
| `loc_sync` | Bring the table up to date with the content (new keys get their source text, no translation is touched, no key is removed); optionally write `.po` files per language and apply returned ones; reports only unless `write` is true |
| `loc_export_po` / `loc_import_po` | gettext `.po` files for translators and back (empty and fuzzy entries are skipped) |
| `loc_format` | Preview how a message formats and pluralizes in a language (`{name}`, `{count\|s}`, `{n\|one:coin;other:coins}`), with the problems found and each number's plural category; no project needed |
| `loc_language` | A tag's normalized form, fallback chain, direction and the plural category of sample numbers |
| `loc_pseudo` | Pseudo-localize text: accents, about a third longer, in `[ ]`, placeholders kept |

### Level sequences (`seq_*`)

Level sequences (`.asequence`) are the engine's cutscenes: tracks bound to entities that animate transforms and
component fields, fire events, play sounds and animations, spawn prefabs, cut between cameras, fade the screen
and play other sequences. They are a real asset type, so they go through the `kit_*` tools (`type: "sequence"`:
schema with a working sample, validate, put, get, list; a sequence with a diagnostic is refused by `kit_put`).

| Tool | What it does |
| --- | --- |
| `seq_validate` | The sequencer's own diagnostics (SQ001...), a summary (effective duration, tracks by type), and, against the project: a Property track's component and field must exist (reflection), Audio cues, Spawn prefabs and Subsequence sequences must be assets, and sequences must not play themselves in a loop; with `scene`, every track's bound entity must be in that scene |
| `seq_evaluate` | What each track is doing at given times, without playing: Transform position and rotation, Property channels, Visibility, Fade amount and colour, the camera cut in force, the spawns alive, the sub-sequences playing with their child time |
| `seq_events` | Everything it fires or changes, in time order: events, audio and animation keys, camera cuts, spawns and despawns, sub-sequence starts and ends, fade keys; optionally a time window |
| `sequence_component_set` / `_list` / `_remove` | Edit the `SequenceComponent` on editor entities (sequence, auto_play, loop, rate, destroy_when_finished), validated, one undoable step |

Evaluation is a pure function of time, as in the player. The `game_*sequence*` tools above play and control
sequences in the hosted game.

### 2D levels: tilesets and tilemaps (`tileset_*`, `tilemap_*`)

A tileset (`.atileset`) cuts a texture into equal tiles, numbered in reading order from 0 at the top left, with a
collision flag per tile and autotile rules. A tilemap (`.atilemap`) is layers of cells over a tileset, row 0 the
bottom row. Both are real asset types, so they also go through the `kit_*` tools (`type: "tileset"` and
`"tilemap"`).

| Tool | What it does |
| --- | --- |
| `tileset_create` / `tileset_info` / `tileset_set` | Make a tileset from a texture in the project (its size is read from the file) with a tile size, margin and spacing; read its grid, solid tiles and autotiles; change the solid tiles (replace, add, remove) and the autotile rules, with every tile number checked against the grid |
| `tilemap_create` | A map over a tileset: width x height cells with named layers, all empty |
| `tilemap_ascii` | A layer as text, top row first: `.` for empty, a symbol per distinct tile in order of first use, `~ = + *` for autotiles; returns the legend; with `solid`, a second grid of the cells that block |
| `tilemap_paint` | Edit with a list of operations that run on a copy and are saved only if all succeed: `set`, `fill`, `line`, `replace`, `ascii` (stamp text rows with a legend at a position), `clear`, `resize` (keeps what fits), `add_layer`; returns the cells changed and warnings for tiles the tileset lacks |
| `tilemap_info` | Size, layers, how often each tile and autotile is used, and problems (a tile the tileset does not have, a missing autotile, a layer of the wrong size, a tileset not in the project) |
| `tilemap_preview` | A PNG returned as an image, drawn from the tileset texture's real tile pictures with the autotiles resolved to the tile each cell shows; optionally the solid cells tinted red and a layer filter |

A cell is a tile number, `-1` for empty, or `"autotile:N"`.

### Luau scripts (`script_*`)

Scripts (`.luau`) are modules that return a table (a class) with lifecycle callbacks, run on entities through a
`ScriptComponent`. Built only where the engine has the Luau scripting module.

| Tool | What it does |
| --- | --- |
| `script_check` | Compile a script (text or a project asset): ok, or the error with its line; and what the script is: its exposed variables (name, kind, default, `--@range`, `--@tooltip`) and the lifecycle callbacks it defines |
| `script_complete` | Code completion for the engine API at a position (a byte `cursor`, or `line` and `column`): `world:` methods, component names after `:Get("`, a component's reflected fields, `self.` fields, callbacks after `function Class:`, keywords, globals, locals |
| `script_run` | Run Luau in the sandbox: what `print` wrote, the returned values, the error with its line; optionally call a global function afterwards (`call: {function, args}`) to test it on its own; a fresh world is bound; a runaway script stops at the instruction budget |
| `script_simulate` | Run a script class on an entity as the game does, headless: OnCreate, OnEnable, OnStart, then N ticks of OnFixedUpdate, OnUpdate, OnLateUpdate and timers (`Timer.After`, `Timer.Every`), then OnDisable and OnDestroy; with overridden `variables` (checked against their kinds), extra `components` and other `entities` for the script to find; returns prints, errors, the entity's final transform and every exposed variable's value |
| `script_check_project` | Every script in the project must compile, and every `ScriptComponent` in the scenes and prefabs must name a script that exists, overriding only variables it exposes with values of the right kind |

Scripts reach gameplay through the same APIs as in the game (`Attributes`, `Timer`, `Input`, ...); the sandbox has
math, string, table, bit32, utf8, buffer, coroutine, vector and `os.clock`, and no file or network access.

Enum values are lowercase words (`"instant"`, `"count"`); `kit_schema` lists them. Write definitions,
`kit_check` them, `asset_cook`, then `game_load` to play them.

### Animation (`anim_*`)

Animation graphs (`.aanim`), blend spaces (`.ablend`) and montages (`.amontage`) have no importer, so these tools take the data as `definition` (JSON) or a `file` under the open project's `Content/`.

| Tool | What it does |
| --- | --- |
| `anim_validate` | The animation module's own diagnostics for a `graph` (AG001..AG011), `blend_space` (BS001..BS006) or `montage` (MN001..MN004). Give `clips` / `blend_spaces` to also check the assets a graph names exist. |
| `anim_blend_weights` | The clip weights a blend space gives at `points` (or an even `grid`), plus the 2D triangulation. |
| `anim_graph_simulate` | Runs a graph headless over `steps` (set variables, fire triggers, play / stop montages, advance `dt`) and reports each state machine's state, the state changes and the events (notifies, montage sections). Clips are stand-ins of a given length: there is no skeleton, so poses are not produced. |

### UI layouts (`ui_*`)

UI layouts (`.aui`) have no importer, so these tools take the layout as `definition` (JSON) or a `file` under the open project's `Content/`.

| Tool | What it does |
| --- | --- |
| `ui_widget_types` | Every widget type, how many children it takes, and what it saves by default, so a layout can be written without guessing field names. |
| `ui_check` | Loads the layout (errors name the widget by path), lays it out at `width` x `height`, and reports duplicate names, bindings that name a missing widget or an unbindable property, widgets that run past the screen or have no size, and keys a button's `hold_key` or a virtual stick's `key_x` / `key_y` name that are not key names; with `touch_target` (say 44) also controls too small for a finger. Returns the widget tree with rectangles, the bindings and the images used. |
| `ui_preview` | Renders the layout to a PNG. Text shows as solid blocks (a fixed-width stand-in font) and images as flat grey rectangles, so it shows layout, anchoring, sizing, colour and clipping, not final art. |
| `ui_theme_check` | Loads a theme (`.atheme`) and lists its palette, control and text styles and images. With a layout it reports each widget `style_class` the theme has no style for. `ui_preview` takes `theme` / `theme_file` to see a layout styled by one. |
| `ui_animation_sample` | Checks the animations in a layout (each track's widget exists, keys in time order) and evaluates every track at the times you give, to show what moves when. |

### Saves and settings (`save_*`)

A game's saves live in a user folder it picks, so these take the folder as `directory`.

| Tool | What it does |
| --- | --- |
| `save_list` | The `.asav` slots in a folder: struct type, schema version, time, size, whether each reads, whether a `.bak` sits beside it, and whether there is a settings file. |
| `save_inspect` | What a save or settings file claims to be (kind, type, version, slot, time), whether its checksum matches, and the saved data as JSON. `backup` reads the `.bak`, which loading falls back to. |
| `save_delete` | Deletes a slot and its backup. |
| `save_settings` | Reads `settings.asettings` (defaults if missing). `set` merges top-level fields, validated as the game does (volumes clamped, window size bounds; each fix a warning) and written atomically with a backup; `reset` writes the defaults. |

### World partition (`world_*`)

The cooked game does not stream a partitioned world, so the simulation runs the streamer on a scratch world, not the hosted game.

| Tool | What it does |
| --- | --- |
| `world_partition` | Splits the editor scene into grid cells on the ground plane. Without `directory` it is a dry run (cells, entity counts, bounds, the largest cell, the persistent count) for choosing a cell size; with it, writes `world.aworld`, `persistent.aesc` and `cells/<x_z>.aesc`. |
| `world_partition_info` | Reads a partitioned world's index and checks every cell file and the persistent scene exist; lists missing files and files not in the index. |
| `world_stream_simulate` | Walks a streaming source along a path over the world and reports, per waypoint, the cells that loaded and unloaded, those now loaded, and the entity count. |

### Materials (`mat_*`)

Materials (`.amat`), functions (`.amf`) and instances (`.amati`) have no importer, so these tools take them as JSON (`definition`, `material`, `instances`) or a file under the open project's `Content/`. Graphs are checked and compiled on the CPU only: no shader is built and nothing is drawn.

| Tool | What it does |
| --- | --- |
| `mat_node_types` | The node library: each node's id, title, category and pins. Filter by `category` or `search`; give a material to include its parameter nodes. |
| `mat_validate` | The renderer's analysis (MT001..MT013), the inferred type of every connected pin, and a summary. `functions` supplies material functions for `Function.Call` nodes. |
| `mat_compile` | Compiles a graph to the HLSL include: defines, the parameter buffer layout, sampled textures, needed features, a cost estimate and the permutation key. The HLSL text is included unless `hlsl` is false. |
| `mat_instance_resolve` | Applies an instance chain (parent first) to a material: each parameter's final value and which level set it, with MI001 / MI002 diagnostics for stale or mismatched overrides. |

### Prefabs (`prefab_*`)

Prefabs are project assets, so these need `project_open`. They act on the editor scene's world directly: the entities they create or change are not on the undo stack.

| Tool | What it does |
| --- | --- |
| `prefab_info` | Looks inside a prefab: entities, nested prefabs, the variant base, what it flattens to (or why it can't), orphaned overrides and the prefabs built on it. `scenes` also checks every scene that uses it. |
| `prefab_make` | Captures an entity and its descendants from the editor scene as a new `.aprefab` under `Content/`. |
| `prefab_instantiate` | Places an instance in the editor scene, at `position` or where the prefab's root is; one undoable, agent-attributed action. |
| `prefab_overrides` | Lists an instance's overrides and deleted entities. |
| `prefab_revert` | Removes overrides (all, or one entity / component / field path) and re-resolves the instance; undoable and agent-attributed. |
| `prefab_apply` | Writes overrides into the prefab asset, saves it, and re-resolves every instance of it or of prefabs built on it in the editor scene; undoable and agent-attributed. |

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
| `log_recent` | The engine's last 512 log lines, including what scripts and Blueprints `print()` (categories `Script`, `Blueprint`); filter by level, category, text, time |
| `log_clear` | Forget the recent log lines |
| `game_spawn_entity` | Create an entity in the running game from component name -> fields (a Transform is always added), with optional tags |
| `game_add_component` | Add a component to a running entity (or merge values onto an existing one) |
| `game_remove_component` | Remove a component from a running entity |
| `game_ui` | The running game's UI: each layer's widget tree with type, name, rectangle, visibility, enabled, and the text, value or percent a widget shows; or one widget by name |
| `game_ui_click` | Click a widget of the game's UI by name (hover, press, release over its middle, a frame between each) |
| `game_lifecycle` | Send suspend, resume or memory-pressure, as a phone's OS would: the game stands still, audio stops and saves are written; resume skips the time that passed |
| `game_input` | Hold keys/axes (`{"W": 1}`), mouse delta for one frame, or clear; `mouse` and `touches` move pointers for the game's on-screen UI (taps, drags on a virtual stick) |
| `game_screenshot` | Render the current frame off screen (what the player draws: scene and HUD) and return it as a PNG image, also saved to `path`. Needs a graphics device (D3D12 or Vulkan); `width`, `height`, `backend`, `inline` |
| `game_attributes` / `game_set_attribute` | An entity's attributes in the running game; set a base, add a delta or define one with bounds, through the game's own attribute system (events fire, bounds apply) |
| `game_interactables` / `game_can_interact` / `game_interact` / `game_interaction_focus` / `game_set_interactable` | The interaction kit in the running game: list usable things (and why an interactor can't use each), check, use (range, tags, cooldown, effect and ability apply, OnInteract is queued for the target's script), find what would be prompted, enable/disable/reset |
| `game_audio_state` / `game_play_sound` / `game_set_bus` / `game_stop_sounds` / `game_audio_source` | The audio kit in the running game: voices, cues playing, listener, reverb, every bus and the cue problems found; play a cue in 2D or at a location; set a bus's volume (faded) or mute; stop everything; play/stop/fade/set volume or cue on an entity's AudioSource |
| `game_sequences` / `game_play_sequence` / `game_sequence_control` / `game_stop_sequences` | The running game's level sequences: every `SequenceComponent` with its time and state, the screen fade and problems; play a sequence by asset path; play, pause, stop, skip, set time, rate or loop on one; stop or skip them all |
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
- `build_tools.h/.cpp`, `subprocess.h/.cpp` -- the build/test/run tools and the child-process runner.
- `screenshot.h/.cpp` -- the off-screen renderer behind `game_screenshot`.
- `asset_tools.h/.cpp` -- the project, asset, import and cook tools.
- `kit_tools.h/.cpp`, `project_host.h` -- the gameplay-kit data tools, and the open-project state they share with the asset tools.
- `audio_tools.h/.cpp` -- cue validation and preview (audio components and live audio are in `editor_tools.cpp` and `game_tools.cpp`).
- `vfx_tools.h/.cpp` -- particle effect schema, validation, files, check and headless simulation (the `ParticleSystem` component tools are in `editor_tools.cpp`).
- `terrain_tools.h/.cpp` -- the terrain workbench.
- `nav_tools.h/.cpp` -- the navigation workbench (the nav component tools are in `editor_tools.cpp`); `image_util.h` -- PNG and base64 helpers for tools that return a picture.
- `blueprint_tools.h/.cpp` -- the Blueprint node, validate, compile and run tools.
- `loc_tools.h/.cpp` -- the localization tools.
- `sequence_tools.h/.cpp` -- the level sequence tools (the `SequenceComponent` tools are in `editor_tools.cpp`, the live ones in `game_tools.cpp`).
- `tilemap_tools.h/.cpp` -- the tileset and tilemap tools.
- `script_tools.h/.cpp` -- the Luau script tools (compiled out where the engine has no scripting module).
- `permissions.h/.cpp` -- the per-project permission file (see below).
- `main.cpp` -- the `aether_mcp_server` executable.
- Tests: `tests/test_mcp.cpp`.

Every module's components are registered at startup, so all of them can be listed, added and edited.
Gameplay AI (`Aether::AI`: behaviour trees, perception) is deliberately not linked: the editor/MCP side
must not depend on it (`docs/design/agent_gameplay_ai_boundary.md`).

The editor's `play` simulation (above) is deliberately small: physics only. For audio, scripting,
sequences and the gameplay kits, load the cooked game with `game_load`. The simulation itself is headless;
`game_screenshot` makes a hidden window and graphics device on first use, so it works only where one exists.

Systems that need project assets or a window (audio output, particles, sequences, UI, scripting) are not
in the headless simulation; they need a full `Game`.

## Permissions, confirmation and the session log

`<scene folder>/.aether/agent_permissions.json` says what an agent may do:

```json
{ "$type": "AgentPermissions", "$version": 1, "default": "allow",
  "tools": { "destroy_entity": "confirm", "save_scene": "deny" } }
```

Each tool gets `allow`, `confirm` or `deny`: the tool's own entry, else `default`, else the built-in rule (the
destructive tools -- `destroy_entity`, `destroy_entities`, `remove_component`, `remove_components_many`, `save_scene`, `load_scene` -- ask first and the rest
run). A missing file means the built-in rule; an unreadable one is reported on stderr and the built-in rule
applies.

A `confirm` tool does not run on the first call. It answers `{"status": "confirmation_required", "token": ...}`;
after the person approves, the client repeats the same call with `"confirm_token": "<token>"`. A token works once,
only for the same tool and arguments, and expires after five minutes.

Every call is appended to `.aether/agent_session.jsonl` as `{"time_ms", "tool", "args_hash", "outcome"}`
(outcome: `ok`, `error`, `denied`, `confirmation_required` or `confirmation_invalid`). Arguments are hashed, not
stored. The log stops growing at 8 MiB.
