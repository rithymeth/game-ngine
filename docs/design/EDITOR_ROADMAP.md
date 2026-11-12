# Editor roadmap: from a tool collection to an advanced editor

The editor is the Dear ImGui shell (`aether_editor_shell`; see [the toolkit decision](EDITOR_UI_TOOLKIT_ADR.md)). This page is
the honest gap between what it is and what an advanced, AAA-grade editor does, in the order the work should be done. It is a
plan, not a status report: a line is only checked when it works and a test or a screenshot shows it.

## What exists

- 33 tools in one window with a command palette, searchable and grouped: Scene, Terrain, Blueprint, Material, Animation, Sequencer,
  Sound Cue, Mixer, UI Designer, Navigation, Behavior Tree, Particle Systems, Save Inspector, Build and Package and more.
- A theme, dockable panels that remember their arrangement, File and Edit menus, a scene list, undo and redo on one command stack.
- The **Scene** tool (hierarchy with drag-to-reparent, inspector on every reflected component) and the **Scene View**: a wireframe
  view of the scene (ground grid, entity boxes, cameras, the selection's axes) with an orbit camera and click-to-select.

## What an advanced editor has that this does not

In priority order; each is a project of its own.

1. **A rendered viewport.** The Scene View draws boxes, not the lit scene. A real one needs the scene renderer to draw a live
   in-memory scene into an offscreen texture that ImGui can show. Today `SceneRenderer` draws a cooked package through a running
   `Game` into a swap chain, and more than a dozen passes take the swap chain, so this is a rework of that boundary (stage 1: render a
   live scene offscreen with a pixel readback test; stage 2: show the texture in the shell).
2. **Transform gizmos.** Move, rotate and scale handles in the Scene View, with snapping (the snapping maths exists in `core/snapping.h`),
   on the selection as one undoable command.
3. **A content browser.** Browse and search the project's assets with thumbnails, drag an asset into the scene or a field. The asset
   database and importers exist; there is no panel for them.
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
