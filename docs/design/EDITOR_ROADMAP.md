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

1. **A rendered viewport.** Partly there, in two forms. **Render Preview** is a still of the saved scene from the real renderer (about two seconds, the
   scene's own camera). **Live Viewport** runs the project's scene under the real renderer inside the editor's own process, drawn on the editor's device, with
   the Scene View's orbit camera, at up to 30 frames a second (about 12 on a software driver). What it is not: it shows the project as last cooked, so an edit
   appears after Reload (which saves, cooks and loads again), and the game runs (physics, scripts), it is not a paused edit view of the in-memory scene. That last
   part needs the scene renderer to draw an in-memory scene: today `SceneRenderer` draws a cooked package through a running `Game`, and the Live Viewport is
   built on exactly that, so the remaining rework is making the renderer take a `World` directly (and then selection, gizmos and picking can draw on the rendered
   image instead of the wireframe).
2. **Transform gizmos.** Move, rotate and scale are done (W, E and R): arrows along the world axes, a ring around each world axis, and
   handles along the entity's own axes; each drag is one undoable edit, works in world space under a parent, and snaps to a step. With several
   entities selected they all follow (the handles sit on the first; an entity whose ancestor is also selected rides along instead of moving twice).
   Still to do: plane handles, and a local-space option for move and rotate.
3. **A better content browser.** One exists (Project > Content Browser: folders, search, git status badges, rename and move, references,
   asset validation, commit). Dragging a model from it into the Scene View places it on the ground under the mouse. Still to do: thumbnails
   (the preview is the way to render one today, a few seconds each, so thumbnails need a cache), dropping onto the hierarchy and onto inspector fields (a texture onto a material slot, a prefab into the scene),
   and prefab placement.
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
