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
| Layout | Move/tab/show docks and persist window state | Qt automation pending | Functional, UI test pending |
| Command palette | Invoke the currently migrated commands and docks | Qt automation pending | Functional, UI test pending |
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
| Scripting | Blueprint editor, Luau code editor |
| Rendering | Material editor, particle/VFX editor |
| Animation | Animation graph, blend space, skeletal clip viewer, montage/event track authoring |
| AI | Behavior tree, navigation |
| Audio | Sound Cue, mixer, waveform tools |
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
