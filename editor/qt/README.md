# Qt editor host spike

This is the first Qt 6 Widgets host for the editor migration. It currently
provides project creation/opening, a schema-backed Project Settings dialog, a project-backed
Content Browser with ancestor breadcrumbs, filtering, folder creation, and a
contextual model-placement action, movable Hierarchy,
Inspector, Output, Animation Sequencer, Animation Clip Editor, Blueprint Graph Editor, Behavior Tree Editor, Material Graph Editor, VFX Particle Editor, and Luau Script docks, and a native QWidget viewport backed by the Aether
D3D12 RHI. The Hierarchy and Inspector now edit a shared `SceneDocument`:
selection, hierarchy filtering, entity rename/create/delete/duplicate, nested
child creation and unparenting, standard and cinematic camera creation, project model placement,
Transform, Camera, and Cine Camera property editing, dirty state, undo
and redo, and JSON/binary scene save/load use the engine ECS and editor command
stack. The Inspector exposes local Transform rotation as pitch/yaw/roll in
degrees while scene data keeps its quaternion representation. Rotation
automation is still pending. New projects start with an in-memory sample world that can be saved as
the startup scene.

The Qt host registers runtime scene schemas before loading a project, including
movement, physics, audio, gameplay attributes, and sequence components. It
starts the enabled project modules before reading the startup scene and refuses
to open a project if a required module is unavailable. Scene documents reject
unknown component schemas, so the editor cannot save a partial scene after
silently dropping plugin data. The self-test load/resaves the registered
runtime components. Legacy scenes without `EntityName` receive readable,
unique editor labels from their tags or model names instead of generic indexes.

The viewport now draws a depth-tested perspective 3D representation of the active
`SceneDocument`, including a ground grid, imported glTF/GLB mesh instances,
base-color materials and textures, selected-entity axis handles,
hierarchy-aware world transforms, and click-to-select. Missing or invalid models
retain shaded editor proxies so they remain editable. Right-drag orbits the
editor camera, middle-drag pans, and the wheel dollies. Focus and Fit All frame
the selected model or scene from imported mesh bounds, while off-map staged
models stay selectable without stretching the map view. It does not yet provide skeletal deformation, PBR
lighting, or X/Z rotation and scale tools. Move drags entities
across the ground plane, optionally snapping to whole world units. Rotate Y
shows a local-axis ring and applies snapped or continuous rotation through the
scene command stack. Play, Pause, Resume, and Stop use the
shared Play-in-Editor snapshot service; play-time edits are restored on Stop.
Unmigrated tools are omitted from the Qt UI instead of appearing as mock
controls.

The Animation Sequencer is backed by the same `SequenceDocument` and
`SequencePlayer` used by the engine's established sequencer tests. It can
create, open, and save `.asequence` assets; edit sequence name, frame rate,
and duration; bind transform tracks to selected scene entities; set, update,
and clear position keys; and create multiple unbound Event tracks with named
keys and optional payloads. Scrubbing and playback fire crossed events to the
status bar in time order. Sequence edits support undo/redo, and preview restore
returns every previewed transform to its pre-preview value. Double-clicking a
sequence in the Content Browser opens it. Selecting a glTF/GLB asset enables
the explicit Place Model action, and double-clicking the asset remains a
shortcut; both create a real `ModelRenderer` entity in the map.

The Inspector's Mesh section lists what a placed model cooks to: its primitives and vertices and a table of LODs with triangle, error and meshlet counts, computed with the cooker's own code and cached per file and write time. It says so when the file is missing, unreadable or too large.
The Luau Script dock provides syntax-highlighted source editing, atomic saves,
and validation through the engine's script class inspection path. Open a `.luau`
asset by double-clicking it in the Content Browser or using Edit Source in the
Script component inspector. The editor prompts before discarding unsaved source
when opening another script, switching projects, or closing the host. Dedicated
Qt interaction automation for the script dock is still pending.

The Blueprint Graph Editor opens `.abp` assets from the Content Browser and
provides graph selection, a searchable node palette, node placement and
movement, compatible pin connections, a selection inspector for node comments
and unlinked scalar/vector/quaternion defaults, compilation diagnostics, save,
and undo/redo. Comment boxes can be added around selected nodes, moved with
their contents, and edited or recolored from the inspector. F9 toggles
breakpoints; Qt Play mode runs Blueprint components and opens Continue, Step
Into, Step Over, and Step Out controls when execution stops, with call frames
and pin values in the stop dialog. Its Members tab creates, renames, and removes variables,
functions, and macros; edits function purity and function/macro parameter names
and types; and edits variable type, default, instance-editable and expose-on-spawn
flags, and tooltip. Project changes and editor close prompt
before discarding unsaved Blueprint edits. While Play is running, the graph highlights
the active node and nodes visited in the latest trace batch.
Dedicated Qt interaction
automation for this Blueprint slice is still pending.

The Behavior Tree dock opens and saves `.abt` assets, shows the validated
hierarchy, adds children to composites, duplicates/removes/reorders nodes,
edits node fields, and reports validation issues. Its Blackboard tab creates,
renames, types, initializes, describes, and removes keys while showing key
usage. Decorators and services can be added, edited, reordered, and removed
per node. Document undo/redo covers these changes. During Play mode its Live
Debugger tab lists BehaviorTreeComponent entities, highlights active nodes,
shows node results and runtime blackboard values, and can start, stop, or
restart a tree. Play mode loads `.abt` files referenced by each component and
routes Behavior Tree Blueprint tasks to matching custom events and Luau tasks/services to methods on the entity Script Component. Luau tasks receive `(dt, first)` and return `"running"`, `"success"`, or `"failure"`; Luau services receive `(dt)`. Missing methods, script errors, and invalid task results are reported once per method during the Play session.

The Navigation dock bakes the active scene's glTF model geometry, reads the
scene's obstacle, modifier-volume, and link components, and exposes the core
agent bake settings, full bake, rebuild-all, status, and live mesh statistics.
The dock controls viewport overlays for walkable polygons, polygon edges, tile bounds, off-mesh links, and obstacle/area volumes. Play mode advances NavAgent crowd movement and AI perception against the baked navigation mesh; the same viewport overlay shows their paths, goals, senses, and known targets.

The Material Graph Editor opens `.amat` files from the Content Browser and can
create and save materials, search and place nodes, move nodes, connect typed
pins with validation, edit parameter names/types/defaults, configure shading
and blend modes, and inspect diagnostics and generated HLSL. Selecting a graph
node exposes configuration for constants, texture-coordinate channels,
component masks, noise octaves, custom HLSL output/body, and unconnected input
defaults. Custom HLSL input signatures can be added, typed, and removed from the
same node details panel. Live preview remains open.

The VFX dock opens `.avfx` assets and supports emitter creation, duplication,
renaming and removal; stage module creation, enable/disable, removal and
reordering; JSON property editing; deterministic CPU preview playback, seeking,
orbit and zoom; validation diagnostics; per-emitter GPU shader eligibility;
generated HLSL inspection/export; and a D3D compute compile check for the
generated reset, emit, and update entry points. The live preview remains
CPU-backed until the shared RHI supports compute pipelines and storage-buffer
bindings. Curve properties have an interactive graph editor with interpolation
and add/drag/remove key controls. Gradient properties have color and opacity
tracks with add/drag/edit/remove controls. Changes update the CPU preview and
are grouped into undoable document edits. Parameter/binding/sub-emitter panels
are available alongside module properties. The parameter panel creates,
renames, retypes, edits defaults, and removes system parameters; the binding
panel attaches declared parameters to emitter module fields; the sub-emitter
panel authors event, target, count, inheritance, probability, and per-frame cap
settings. These edits use the particle-system document history and appear in
validation diagnostics.

The Animation Graph dock opens `.aanim` assets and supports pose-node placement
and movement, validated pose connections, typed variable authoring, clip and
blend-node settings, output selection, diagnostics, save, and undo/redo. State
machine authoring and project-backed clip/blend-space pickers remain to be
migrated.

The Blend Space dock opens `.ablend` files and supports 1D/2D axis setup,
sample placement and editing, snapped dragging, live blend weights, validation,
save, and undo/redo. Clip paths are entered manually; project asset picking and
direct clip browsing remain to be migrated.

The Animation Workbench previews glTF skeletal deformation, provides rig
selection and clip playback/key editing, and shows the selected bone's
translation curves. Its notify track supports event creation, selection,
renaming, retiming, duration windows, dragging, deletion, and undo/redo; notify
data is stored in `.aetheranim` sidecars. The Montage Editor opens and saves
`.amontage` assets, assigns a clip and slot, configures blend times, authors
named section boundaries and next-section links, previews the active section at
a chosen time, validates montage data, and supports undo/redo. Multi-track
event sequencing remains to be migrated.

The Sound Cue dock opens `.acue` assets from the Content Browser and supports
creating, removing, selecting, and dragging cue nodes on a graph canvas.
Connections and the selected cue output are drawn on the canvas; node positions
are stored in the cue document. A node list remains available as an alternate
selection view. The details panel edits Wave/Random/Modulator/Loop/Delay
settings and adds or removes child inputs. Wave nodes browse audio assets in
the active project's Content directory, and Preview plays the cue through the
engine mixer and default audio backend. Connections pass through the cue
document's cycle validation. Its Mixer panel shows the Master/Music/SFX/UI/Voice
bus levels, mute states, parent routing, and live stereo peak meters. Developers
can add child buses and adjust levels or mute state while previewing; output is
paused briefly while a new bus is registered, then restarted. The effect chain
can add low-pass, high-pass, and peak filters, compressors, and reverbs, with
bypass and their core parameters editable per bus. Effects can be removed and
moved earlier or later in their bus chain while audio is running; mixer commands
apply those edits between render blocks. Wave nodes also show decoded mono/stereo
waveforms for WAV, Ogg, and FLAC assets, with zoom, horizontal scroll, a marked
sample position, duration metadata, a 128 MB preview limit, and direct asset
preview.

Project Settings edits and validates the project name, startup scene, fixed
timestep, gravity, packaged window settings, default quality, always-cook
paths, plugins, and collision layers before atomically replacing the in-memory
settings after a successful save.

Configure on Windows with Qt 6.8 or later available to CMake:

```powershell
cmake -S . -B build-qt `
  -DAETHER_BUILD_QT_EDITOR=ON `
  -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-qt --target aether_editor_qt --config RelWithDebInfo
```

The current host is Windows/D3D12 only. Scene editing is the first shared
document workflow; most existing editor tools are not connected yet. The
ImGui hosts and panels remain available during migration. See
`docs/design/QT_EDITOR_FEATURE_COVERAGE.md` for the functional/test ledger and
`docs/design/EDITOR_QT_UX_ROADMAP.md` for the staged migration and remaining
validation gates.
