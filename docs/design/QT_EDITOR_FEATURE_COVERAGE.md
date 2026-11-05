# Qt editor feature coverage

This is the acceptance ledger for replacing the ImGui editor with Qt 6. A
feature may appear in the Qt UI only when it changes real editor or project
state, reports failures, and has an automated test for its non-visual logic.
Disabled mock controls and sample-only panels do not count as migrated.

## Current production slice

| Area | Functional behavior | Automated coverage | Status |
|---|---|---|---|
| Project | Create/open project; edit and persist identity, startup scene, physics, window, quality, cook, plugin, and layer settings | Existing project/editor-project tests and Qt self-test | Functional |
| Scene document | New/open/save JSON and binary scenes, dirty state; preserve registered movement, physics, audio, attribute, and sequence components; reject unknown components before editor save | `SceneDocument_SelectionEditsUndoAndRoundTrips`, `SceneDocument_RejectsUnknownComponentsWithoutReplacingCurrentScene`, and Qt runtime-component load/resave self-test | Functional |
| Map hierarchy | Filter/select; readable unique names for legacy scenes; create empty entities and cameras; place project model assets; create nested child entities; move children to the world root; duplicate, delete, and rename entities | `SceneDocument_MapEntitiesDuplicateWithComponentsAndUndo`, `SceneDocument_LegacyScenesGetReadableUniqueEditorNames`, and Qt self-test | Functional |
| Inspector | Explicit empty/active selection state; entity name and Transform position/rotation/scale; reset actions; Camera projection, lens, clip planes, and priority; Cine Camera focal length, sensor, aperture, and focus distance | Existing Qt self-test covers selection states, position/rotation conversion and reset, perspective/orthographic control states, camera creation, lens edits, and undo/redo; scale interaction coverage is pending | Functional for Entity, Transform, Camera, and Cine Camera |
| Undo/redo | Create, delete, rename, and transform commands | Command tests, scene-document test, Qt self-test | Functional |
| Play session | Play, pause, resume, stop, snapshot restore, frozen history | Play-session tests, scene-document test, Qt self-test | Functional |
| Perspective world view | Depth-tested grid; imported glTF/GLB mesh instances, base-color materials, and textures; fallback editor proxies; hierarchy-aware transforms and visible-bounds picking; model-bounds focus and scene fit; plane drag, X/Y/Z move handles, and WASD/QE selected-object movement in Move mode; uniform scale drag and per-axis Inspector scale; local-Y rotation ring; orbit, pan, dolly, and camera fly controls outside Move mode | Existing framing self-test and AETHER-01 runtime/visual smoke coverage; transform interaction automation pending | Core view functional; skeletal deformation, PBR lighting, and full 3-axis rotation/scale gizmos remain |
| Content Browser | Project-root navigation with ancestor breadcrumbs, filtering, create folder; select glTF/GLB and place it into the scene from a clear action or double-click | Qt self-test verifies breadcrumb/parent navigation and model placement with a project-relative asset; filter/folder widget coverage pending | Functional core; remaining UI automation pending |
| Output | Live engine log sink | Logger behavior covered elsewhere; Qt automation pending | Functional, UI test pending |
| Luau source editor | Docked source editing with syntax highlighting, atomic save, and compile/class validation; open from the Content Browser or Script Editor dock | Existing Luau validation API; Qt self-test coverage pending | Functional, UI test pending |
| Blueprint graph editor | Open project `.abp` assets; switch graphs; search/place nodes; move nodes; connect compatible pins; add/move/edit/recolor/remove graph comment boxes; toggle F9 breakpoints and stop/continue/step the Qt Play runtime; inspect stopped call frames and pin values; highlight the active node and nodes visited in the latest trace batch during Play; edit selected-node comments and unlinked scalar/vector/quaternion defaults; create/rename/remove variables and function/macro graphs; edit variable type/default/flags/tooltip and function/macro signatures; compile diagnostics; save and undo/redo | Existing Blueprint VM debugger tests; dedicated Qt interaction coverage pending | Initial Qt authoring slice; breakpoint automation remains |
| Behavior Tree editor | Open/save `.abt`; inspect validated hierarchy; add, duplicate, remove, reorder, and configure nodes; author typed blackboard keys with rename/reference updates, initial values, and descriptions; add, configure, remove, and reorder decorators/services; report validation issues; undo/redo; inspect live Play-mode branches/status and runtime blackboard values; start, stop, and restart a tree; invoke Luau tasks and services on the entity Script Component with validated task results | Behavior Tree document tests; dedicated Qt interaction coverage pending | Dedicated Qt interaction coverage remains |
| Material graph editor | Open and save `.amat`; create a material; search/place nodes; move nodes; connect typed pins with graph validation; create/remove/rename parameters and edit parameter defaults/types; configure constants, texture-coordinate channels, component masks, noise octaves, custom HLSL input signatures/output/body, and unconnected input defaults; set shading/blend/face modes; inspect diagnostics and generated HLSL; undo/redo | Dedicated Qt interaction coverage pending | Functional authoring core; live preview remains |
| Particle/VFX editor | Open/save `.avfx`; create, duplicate, rename, and remove emitters; author and reorder stage modules; edit serialized module and emitter properties; add, drag, and remove curve keys with interpolation controls; add, drag, edit, and remove gradient color and opacity keys; create/retype/rename/remove typed system parameters with editable defaults; bind parameters to emitter module fields; author event sub-emitters and inheritance/limit settings; deterministic CPU particle preview with orbit, timeline, and playback speed; inspect validation diagnostics and GPU shader eligibility; inspect/export generated HLSL; compile-check generated compute entry points with D3DCompiler | Dedicated Qt interaction coverage pending | Functional authoring core; shared RHI compute resources and actual GPU simulation/render preview remain |
| Sound Cue and Mixer | Open/save `.acue`; create, remove, select, and drag Wave, Random, Sequence, Modulator, Concatenator, Loop, Mix, and Delay nodes on a graph canvas; render child connections and cue output; persist node positions; configure node settings; connect/disconnect child inputs with cycle validation; choose cue output; discover project WAV/Ogg/FLAC assets; preview and stop cues; inspect mono/stereo waveforms with zoom, scroll, position marker, metadata, and direct asset preview; inspect bus routing and stereo peak meters; adjust volume/mute; add child buses; add, configure, bypass, remove, and reorder low-pass/high-pass/peak filters, compressors, and reverbs | Existing cue document validation and graph-edit logic; dedicated Qt interaction coverage pending | Functional audio authoring and mixer core |
| Animation graph editor | Open/save `.aanim`; create and move pose nodes; connect pose inputs with cycle validation; configure clip asset/loop/rate and blend variables; create, rename, remove, type, and set defaults for variables; select output pose; inspect graph diagnostics; undo/redo | Existing animation graph/document tests; dedicated Qt interaction coverage pending | Initial Qt pose-graph slice; state machine authoring and asset picker remain |
| Skeletal clip editor | Preview glTF skeletal deformation; browse rig nodes; play, pause, loop, scrub, and edit animation keys; inspect selected-bone translation curves; add, select, rename, retime, resize, drag, and remove notify events; persist events in `.aetheranim`; undo/redo clip and event edits | Existing animation and glTF editor coverage; dedicated Qt interaction coverage pending | Montage composition and event sequencing remain |
| Montage editor | Open/save `.amontage`; assign clip and slot; configure blend-in/out; add, rename, retime, remove, and chain sections; inspect section at preview time; validate montage; undo/redo | Montage serialization/runtime tests; dedicated Qt interaction coverage pending | Multi-track event sequencing remains |
| Blend Space editor | Open/save `.ablend`; configure 1D/2D axes and ranges; add, move, configure, and remove clip samples; snap points to a grid; preview live blend weights; inspect diagnostics; undo/redo | Existing blend-space/document tests; dedicated Qt interaction coverage pending | Clip asset paths are entered manually; skeletal pose preview and project asset picker remain |
| Layout | Move/tab/show docks and persist window state | Qt automation pending | Functional, UI test pending |
| Command palette | Invoke the currently migrated commands and docks | Qt automation pending | Functional, UI test pending |
| Mesh (Inspector) | For an entity with a model: primitives and vertices, and a LOD table (triangles, error, meshlets) from the same `GenerateLods`/`BuildMeshlets` the cooker runs; cached per file and write time; says so when the file is missing, unreadable or too large; skinned primitives are listed as not included | `MeshSummary_*` unit tests (counts match the cooker's functions; skinned, empty and oversized cases) and the Qt self-test (placing the test cube shows four LOD rows, 12 triangles at LOD0) | Functional |
| Transform animation | New/open/save `.asequence`; edit name/FPS/duration; bind Transform tracks by entity GUID; set/update/clear position keys; scrub, loop, preview, pause, and restore the scene | Existing 10 SequenceEditor tests and Qt self-test save/reload/evaluation flow | Functional |

The Qt executable supports `--self-test`. CTest registers it as
`qt_editor_self_test`; it invokes the real window actions and validates map
duplication, parent/child hierarchy display, Inspector selection, Transform reset,
Camera editing, selected-model placement, transform-track authoring/scrubbing/restoration,
sequence save/reload, project persistence, and the Play/Pause/Resume/Stop
lifecycle without opening file dialogs.

## Not exposed until migrated

These existing editor tools still use the ImGui presentation layer and are not
shown as working Qt tabs or menu commands:

| Category | Tools still to migrate |
|---|---|
| Rendering | GPU particle preview integration |
| Animation | Multi-track event sequencing |
| UI | UI Designer |
| World | Terrain, foliage, spline, world partition |
| 2D | Tilemap editor |
| Cinematics | Property, event, visibility, spawn, camera cut, audio, fade, and subsequence Qt editors |
| Data | Save Inspector, localization |
| Debug | Gameplay Debugger, console, profiler, crash reports |
| Networking | Network Play, Network Profiler |
| Project | Build and Package, Plugins |

Each tool moves to Qt as a complete vertical slice: document/service adapter,
native Qt surface, save/dirty/error behavior, keyboard and empty states,
automated logic tests, Qt action self-test, and a launch/render smoke test.

The Navigation dock bakes the active scene's static glTF model geometry with
the scene's NavObstacle, NavModifierVolume, and NavLinkProxy components. It
exposes agent bake settings, Bake and Rebuild All controls, status/errors, and
live tile, polygon, dirty-tile, revision, volume, and link statistics. It
recreates its NavWorld when the active scene changes. The viewport can show
area-colored navigation polygons, edges, tile bounds, links, volumes, NavAgent
paths and goals, sight cones, hearing ranges, and perceived or remembered
actors. Play mode advances NavCrowd and PerceptionWorld against the baked mesh.
