# Qt editor host spike

This is the first Qt 6 Widgets host for the editor migration. It currently
provides project creation/opening, a schema-backed Project Settings dialog, a project-backed
Content Browser with filtering and folder creation, movable Hierarchy,
Inspector, Output, and Animation Sequencer docks, and a native QWidget viewport backed by the Aether
D3D12 RHI. The Hierarchy and Inspector now edit a shared `SceneDocument`:
selection, hierarchy filtering, entity rename/create/delete/duplicate, nested
child creation and unparenting, camera creation, project model placement, transform position, dirty state, undo
and redo, and JSON/binary scene save/load use the engine ECS and editor command
stack. The Inspector exposes local Transform rotation as pitch/yaw/roll in
degrees while scene data keeps its quaternion representation. Rotation
automation is still pending. New projects start with an in-memory sample world that can be saved as
the startup scene.

The viewport now draws a depth-tested perspective 3D representation of the active
`SceneDocument`, including a ground grid, imported glTF/GLB mesh instances,
base-color materials and textures, selected-entity axis handles,
hierarchy-aware world transforms, and click-to-select. Missing or invalid models
retain shaded editor proxies so they remain editable. Right-drag orbits the
editor camera, middle-drag pans, and the wheel dollies. Focus and Fit All frame
the selection or scene. It does not yet provide skeletal deformation, PBR
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
and clear position keys; and scrub or loop a live viewport preview. Restore
returns every previewed transform to its pre-preview value. Double-clicking a
sequence in the Content Browser opens it, while double-clicking a glTF/GLB
asset places a real `ModelRenderer` entity in the map.

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
