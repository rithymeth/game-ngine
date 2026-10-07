# Aether Editor 2.0: unified workflow UX

This document turns the proposed Aether Editor 2.0 direction into an
implementation-ready workflow spec. It complements
[EDITOR_UI.md](EDITOR_UI.md), which remains the source for visual tokens,
component behavior, and accessibility details.

## Goal

A developer should be able to create or open a project, author a world,
build gameplay, test it, diagnose problems, and package a game through one
consistent editor shell. Feature areas keep their specialized tools, while
selection, assets, commands, diagnostics, and project state behave the same
across them.

The editor is an optional authoring application over the engine. Runtime
code must not depend on editor UI or editor services.

## Current baseline

The current editor already hosts world, scripting, Blueprint, animation,
AI, audio, UI, material, networking, profiling, and packaging tools.
EditorWorkspace presents these through a categorized Tools hub and
separate tool windows. The Windows editor and portable shell share that
workspace. The main editor currently uses explicitly positioned ImGui
windows; it does not yet have a docking-branch layout manager.

The UX rollout must preserve that working tool access and make the shared
workflow more direct before attempting a large rewrite of the host.

## Shell model

Use five predictable regions:

1. **Application bar** — project name and dirty state; File, Edit, Asset,
   Entity, Scene, Build, Debug, Window, and Help menus; global search.
2. **Workspace bar** — World, Script, Blueprint, Animation, AI, Audio, UI,
   Material, Sequence, Network, and Profiler. Choosing a workspace changes
   the central task context and its useful panel arrangement.
3. **Authoring area** — the active editor, with the World viewport receiving
   the most space in the default workspace.
4. **Context panels** — Project/Assets, Hierarchy, Inspector, and Details.
   They act on the current project, selection, or active document.
5. **Diagnostics and status** — Console, Profiler, Build, Debug, Network,
   and Logs, plus a compact status strip.

A workspace is a saved arrangement and entry point, not a separate copy of
the project. All workspaces share the same selection, undo history, asset
registry, command system, and project state.

### Default World workspace

- Left: Hierarchy with tabs for Hierarchy and Place.
- Center: World viewport and opened asset documents.
- Right: Inspector with tabs for Inspector and World Settings.
- Bottom: Content Browser with Console, Output, and Messages tabs.
- Footer: build/dirty state and live CPU, GPU, memory, entity, and network
  indicators when those metrics are available.

On narrower windows, collapse side panels to drawers before reducing the
viewport below a usable size. The content browser may be opened as a drawer
in the level-design preset.

### Workspace presets

Provide built-in Programmer, Level Designer, Technical Artist, Animator,
and Network Engineer presets. Each preset selects relevant existing tools
and panel sizes. Users can save, rename, export, import, and reset layouts.
Persist layouts per project under Saved/Layouts/; keep editor-wide
preferences separate from project layout data.

Until a docking-capable host is available, implement presets using the
current explicit-window model. Do not make docking a prerequisite for
switching workspaces or restoring saved panel visibility and geometry.

## Primary flows

### Open or create a project

The start screen lists recent projects with their path, template/type, and
last-opened time. Actions are Create, Open, Import, and Open Folder. The
create wizard is short: choose a template, renderer (Auto, D3D12, or Vulkan
when supported), and optional gameplay modules; then choose a location and
create. Validate the path before enabling Create and show actionable errors
in place.

### Author a world

The viewport toolbar groups Select/Move/Rotate/Scale with terrain, foliage,
spline, camera, lighting, collision, navigation, and debug controls. The
Hierarchy filters entities, supports search and multi-selection, and exposes
parenting and creation actions. The Inspector keeps component sections
collapsible and searchable. Component add, reset, copy/paste, multi-edit,
and Blueprint exposure use consistent commands where supported.

### Find and run tools

Ctrl+K opens a command palette over the current workspace. It searches
commands, tools, assets, and recently opened documents, with category and
shortcut shown for each result. Arrow keys move, Enter invokes, Escape
closes, and the current selection/focus is restored on close. Results are
filtered as the user types. Commands that need an object say so and remain
disabled until the context is valid.

Every tool remains reachable through the workspace bar, menus, and the
existing Tools hub during migration. Ctrl+P can be reserved for project
search only after it has an unambiguous definition.

### Test, diagnose, and ship

Play, Pause, Step, and Stop stay visible while running. The viewport frame
shows distinct playing and paused states. The editor makes the active world
(Edit or Play) and command-stack availability explicit. Console entries
filter by severity and subsystem; selecting an error opens the related
asset, entity, or source location when one exists.

Build & Ship presents target, configuration, asset cooking, packaging
options, and Build/Package/Run actions in one place. Results include elapsed
time, output size, asset counts, warning/error counts, and direct actions
to launch or open the output folder.

## Shared interaction rules

- Search fields keep their query while a panel is temporarily hidden.
- Double-click opens an asset; single-click selects it. Dragging an asset
  onto a compatible target previews the result before applying it.
- Destructive file operations require confirmation. Undoable scene edits do
  not prompt for confirmation.
- Unsaved documents show a dirty marker in their tab and project context.
  Save, Save All, Undo, and Redo have consistent menu entries and shortcuts.
- Empty panels explain the next useful action. Errors identify the failed
  item and provide a direct recovery action where possible.
- All workspace changes, menus, dialogs, and command-palette results are
  keyboard reachable. Tooltips explain controls without replacing labels.
- Status indicators never invent metrics: unavailable measurements are
  omitted or marked unavailable.

## Delivery sequence

1. **Workflow layer:** standardize shared search, selection, dirty state,
   command invocation, notifications, and diagnostics across existing tools.
2. **Workspace navigation:** add the workspace bar and presets on top of the
   shared EditorWorkspace; retain the Tools hub as a compatibility view.
3. **Command palette:** index tools and existing actions first, then add
   assets and recent documents as their discovery APIs support them.
4. **Project entry and ship flow:** improve the project manager and unify
   build/package results using the existing project and packaging tools.
5. **Layout backend:** add real docking and layout import/export when the
   editor host and vendored ImGui support it; migrate without changing the
   workspace model or command IDs.

Ship each step with the Windows editor and portable shell kept in parity.
New interactions should have a headless or automated verification path;
visual layout changes should include a captured screenshot at a common
window size.

## Acceptance checklist

- Every existing tool is reachable from a named workspace and remains
  reachable from the Tools hub.
- Ctrl+K opens, filters, invokes, and closes without losing prior focus.
- Workspace selection preserves project, selection, open documents, and
  dirty state.
- Built-in layouts can be restored after restart; reset returns to Default.
- Play/Pause/Stop state is visible and consistent in both editor hosts.
- Build failures and console errors expose useful context and recovery paths.
- Keyboard navigation works through the shell and primary panel controls.
- No authoring UI dependency is introduced into runtime targets.
