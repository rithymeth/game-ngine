# Editor roadmap: from a tool collection to an advanced editor

The editor is the Dear ImGui shell (`aether_editor_shell`; see [the toolkit decision](EDITOR_UI_TOOLKIT_ADR.md)). This page is
the honest gap between what it is and what an advanced, AAA-grade editor does, in the order the work should be done. It is a
plan, not a status report: a line is only checked when it works and a test or a screenshot shows it.

## What exists

- 33 tools in one window with a command palette, searchable and grouped: Scene, Terrain, Blueprint, Material, Animation, Sequencer,
  Sound Cue, Mixer, UI Designer, Navigation, Behavior Tree, Particle Systems, Save Inspector, Build and Package and more.
- A theme, dockable panels that remember their arrangement, File and Edit menus, a scene list, undo and redo on one command stack.
- The **Scene** tool (hierarchy with drag-to-reparent, inspector on every reflected component) and the **Scene View**: a wireframe
  view of the scene (ground grid, entity boxes, cameras, the selection's axes) with an orbit camera, click-to-select and move, rotate and scale gizmos.

## What an advanced editor has that this does not

In priority order; each is a project of its own.

1. **A rendered viewport.** Largely there, in two forms. **Render Preview** is a still of the saved scene from the real renderer. **Live Viewport** runs the
   open scene under the real renderer inside the editor's own process, with the Scene View's orbit camera, at up to 30 frames a second (about 12 on a
   software driver): an edit, a gizmo drag or an undo shows a moment later, **without saving**, because the editor hands the game its in-memory scene
   (`GamePackage::SetContentOverride`, the scene as `SaveSceneToMemory` writes it). Lighting and material changes in the scene (a light's colour, a
   transform) therefore show live. A move (a gizmo drag, a nudge) and an edit to a light, the sky, the wind, a post-process volume or a decal (colour, intensity, shadows, ...) are patched into the running scene in place, and so is adding or removing an object (a model, a light, an empty, a prefab instance, a duplicate), moving one under another in the hierarchy, or a light, model or name on one; an entity or an edit that involves anything the running game builds when it loads (a script, a character controller, a camera), and an undo or redo of anything else, reload the whole scene into the game (the world, physics
   and scripts start over), so it suits small and medium scenes; **Run game** is off by default so the view is the scene as authored; a model added since the
   last cook is not drawn until **Reload** (which cooks again), and the panel says so. The editing tools are shared with the Scene View and work on the rendered
   picture: click an object to select it, drag a gizmo handle to move, rotate or scale it (W, E, R), with the selection outlined, and the picture follows the drag.
   A per-entity material override (a `MaterialParameters` component: base colour, metallic, roughness, emissive, a base colour texture) shows live, but Material *asset* edits (the Material editor's graph) are not pushed into the view: the player has no material graph, so that is renderer work, not a cook of the changed asset. Objects are outlined by their models' real bounding boxes and picked by their real shape (read from the glTF files; a ray through
   the mouse picks the nearest model whose box it enters, and a camera or light in front of a model is picked over it), and the click must then meet one of the model's triangles (the bind pose, up to 300,000 triangles; a bigger model is picked by its box), so the gap
   in a model's shape or the empty corner of its box is not clicked. Not by what is drawn: an alpha-cut leaf or an animated pose is still its bind-pose geometry, which is what a renderer object-id buffer would fix.
2. **Transform gizmos.** Move, rotate and scale are done (W, E and R): arrows along the world axes, a ring around each world axis, and
   handles along the entity's own axes; each drag is one undoable edit, works in world space under a parent, and snaps to a step. With several
   entities selected they all follow (the handles sit on the first; an entity whose ancestor is also selected rides along instead of moving twice).
   Move and rotate can follow the object's own axes (World/Local, Q). Move also has plane squares that slide in two axes at once. Still to do: plane handles for Scale.
3. **A better content browser.** One exists (Project > Content Browser: folders, search, git status badges, rename and move, references,
   asset validation, commit). Dragging a model or a prefab (.aprefab) from it into the Scene View or the Live Viewport places it on the ground under the mouse, and onto a row of the hierarchy places it under that entity (a prefab as a linked instance, root and children, one undo step). Still to do: thumbnails
   (the preview is the way to render one today, a few seconds each, so thumbnails need a cache), dropping onto an inspector field that is not a path (the inspector takes a picture on a texture path, a sky image, and a material on a material path, but not yet a model on a model reference), and a prefab's own rotation and scale (a placed instance takes the drop position only).
4. **Previews.** Material, mesh and animation previews that use the real renderer (needs item 1), so tuning a material shows the result.
5. **Multi-scene and prefab workflow.** Several scenes open at once, prefab instance overrides shown in the inspector, a prefab editing mode.
6. **Play in editor with the real runtime** shown in the viewport, with pause, step and eject-the-camera (needs items 1 and 2).
7. **Project-wide search, a console that links errors to the entity or file, an asset dependency graph.**
8. **Collaboration and review:** source-control status in the content browser, a diff view for scenes.
9. **Polish that is not optional at this level:** icons and a font set with symbols, keyboard shortcut customization, accessibility
   (scaling, contrast), localization of the editor itself, and a layout preset per task.

## Not in scope here

"AAA" is also a content and rendering target (virtual geometry, global illumination, a cinematics pipeline). Those are engine
work tracked in the readiness matrix, not editor work, though the editor needs to expose them once they exist.
