# Qt Editor UI/UX migration plan

- **Status:** In progress — Qt 6 Windows host spike
- **Scope:** Desktop editor presentation and host. The engine renderer, RHI,
  runtime, and in-game UI stay independent from Qt.
- **Design concept:** [Qt editor World workspace](qt-editor-concept.html) ·
  [Rendered screenshot](qt-editor-concept.png)

This plan turns the toolkit recommendation in
[EDITOR_UI_TOOLKIT_ADR.md](EDITOR_UI_TOOLKIT_ADR.md) into reviewable delivery
stages. The ADR remains the go/no-go authority: production panels move to Qt
only after the native viewport, platform support, packaging, and licensing
gates pass.

The first implementation lives in `editor/qt` behind
`AETHER_BUILD_QT_EDITOR`. It has a dockable World shell, native Aether D3D12
viewport, project create/open/settings-save flows, a project-backed Content
Browser, scene-backed Hierarchy and Inspector, a Ctrl+K palette, and saved dock
layouts. The Inspector uses explicit selection and empty states, exposes
undoable position and rotation reset actions, and the shared scene document
supports JSON/binary load and save, entity creation/deletion/rename, transform
position and quaternion rotation edits, undoable Camera projection and lens
settings, and editable Cine Camera lens controls (focal length, sensor, aperture,
and focus distance) with undo/redo,
dirty state, and undo/redo through the editor command stack. Rotation is
edited as local pitch/yaw/roll degrees; the Qt self-test verifies quaternion
normalization, Euler round-tripping, and undo. The target builds with Qt
6.8.3/MSVC 2022.
The Content Browser now gives users a useful no-project start state with
direct Create/Open actions. Its File menu also exposes the persisted recent
project list, backed by the shared editor-core model and capped at ten entries.
In an open project, users can narrow the current folder by model, scene,
sequence, or project type; empty folders and filtered no-match results have
separate guidance. Search labels make its current-folder scope clear, and the
unselected browser state reports the visible or matching entry count. This
improves project entry and asset discovery but does not replace the fuller
project hub in stage 3.
Hierarchy filtering is keyboard reachable with Ctrl+F, reports matches against
the scene entity count, and no longer switches Inspector targets while the
user types; a selected entity stays selected while visible and clears if the
filter hides it.
The viewport now shows a depth-tested perspective representation of the active
scene, renders project-relative glTF/GLB mesh instances with base-color
materials and textures, and retains shaded proxies for missing assets and
editor-only entities. It selects models by their visible bounds, moves them on
the ground plane or along X/Y/Z handles, supports uniform scale by drag and
per-axis scale in the Inspector, and provides a local-Y rotation ring. Move,
rotation, and scale edits are undoable and serialized. Camera navigation uses
right-drag orbit, middle-button pan, wheel zoom, and Shift speed boost. In Move
mode WASD and Q/E move the selected object; in other modes those keys fly the
camera. Focus and Fit All remain available. Skeletal deformation,
PBR lighting, and full 3-axis rotation/scale gizmos remain. Viewport interaction
coverage still needs automated input and visual smoke checks.
Play control, existing tool panels, and most shared
editor services are not migrated. Other platforms/backends and deployment
still need validation. Keep the ImGui editor available until the migration
acceptance criteria below pass.

## Product outcome

The editor should feel like one desktop tool for the full game workflow:
open a project, author a world, edit gameplay and assets, play the game,
diagnose issues, and package a build. The World workspace is the default
starting point. Its viewport gets most of the space; Hierarchy and Inspector
stay beside it; Content Browser and diagnostics sit below it. A workspace bar,
command search, visible Play controls, project dirty state, and build/runtime
status stay easy to reach.

The visual concept in the linked files is a direction for the World screen,
not a final component specification. It shows a scene with a selected player,
transform gizmo, entity tree, component inspector, asset thumbnails, run
controls, and compact project status. Validate hierarchy, terminology,
contrast, density, and responsive behavior with editor users before treating
it as final.

## Design principles

- Keep the scene viewport central and preserve enough room to work in it.
- Make project, selection, dirty state, Play state, build state, and errors
  visible where they affect the next action.
- Use the same search, selection, keyboard, property editing, and undo rules
  in every workspace.
- Let users switch between Level Designer, Programmer, Technical Artist,
  Animator, Audio Designer, Network Engineer, and QA layouts without changing
  project data or selection.
- Keep routine authoring undoable. Confirm only irreversible file actions.
- Make controls keyboard reachable, clearly labeled, DPI-aware, and legible
  at supported window sizes.
- Keep Qt types in presentation code. Shared editor services and runtime code
  must not depend on Qt or ImGui.

## Delivery stages

### 0. Confirm the product and build gates

**Work:** Confirm the project license and the intended Qt modules/deployment
model; define supported editor platforms and renderer combinations; establish
the Qt version and dependency source; inventory the existing workflows and
capture baseline screenshots from the Windows editor and portable shell.

**Exit:** The license and packaging path are reviewed, supported platform
combinations are written down, and the current baseline is captured. If the
licensing or platform model is unresolved, keep the Qt host experimental.

### 1. Prove the Qt host and native viewport

**Work:** Build a disposable Qt Widgets host with Hierarchy, Inspector,
Content, and Console docks; tab, split, float, close, save, and restore those
docks; add a keyboard-operated Ctrl+K command palette; host the existing
Aether RHI viewport through a native child surface. Check resize, focus,
input, minimize/restore, high DPI, shutdown, packaging, and representative
Vulkan/D3D12 configurations.

**Exit:** The viewport uses the existing RHI device and frame loop, survives
the lifecycle and input checks, and builds and packages on the supported
platform matrix. Record measured gaps and make the ADR go/no-go decision.
Do not migrate production panels before this gate passes.

### 2. Separate editor behavior from presentation

**Work:** Define presentation-neutral services for commands and shortcuts,
documents and dirty state, selection/context, undo transactions, project
state, assets, diagnostics, and Play sessions. Adapt existing ImGui tools to
these services so the current editor keeps working while Qt is introduced.
Give commands stable IDs and context-aware enablement.

**Exit:** Core state and command behavior can be exercised without a Qt or
ImGui widget. A change to selection or document state is reflected by either
host. Runtime and player targets remain free of editor dependencies.

### 3. Build the Qt application shell

**Work:** Add the optional editor-only Qt target. Build the application menu,
project hub, document tabs, workspace switcher, Ctrl+K palette, dock layout,
status strip, settings, and recovery/help entry points. Add default and role
layouts, plus safe layout versioning, reset, save, and restore. Keep the Tools
directory available while tools migrate.

**Exit:** Users can open a project, switch workspace, open and close a tool,
search commands by keyboard, restore a saved layout after restart, and see
project/build/run state. Verify at minimum 1280×720 and 1920×1080 plus a
high-DPI display.

### 4. Migrate the core create–edit–test loop

**Work:** Port the viewport host, World toolbar, Hierarchy, Inspector,
Content Browser, Blueprint, Script, Console, and Build/Package surfaces.
Connect native Qt controls to the shared selection, document, asset,
command, and diagnostics services. Preserve scene and asset workflows while
the old host remains available as a fallback.

**Exit:** A user can create or open a project, author a world, edit gameplay,
run it, inspect errors, and produce a package in the Qt host. Compare the
same workflows against the baseline and fix interaction or data-loss gaps.

### 5. Migrate specialist workspaces

**Work:** Port Animation, AI/Navigation, Audio/Mixer, UI, 2D, Sequencer,
Material, VFX, Network, Profiler, Gameplay Debugger, Save Inspector,
Localization, Plugins, and project settings in small groups. Use shared
property, command, search, document, and error patterns. Provide a plugin
adapter and documented compatibility boundary for editor extensions.

**Exit:** Every shipped tool has a named workspace and remains discoverable
through the Tools directory. Plugin behavior and representative large-project
workflows are validated.

### 6. Stabilize and retire the production ImGui host

**Work:** Validate keyboard-only workflows, accessibility names, contrast,
layout persistence/version upgrades, multi-monitor DPI, focus recovery,
asset scale, large scenes/projects, crash recovery, and startup/performance.
Capture consistent CI screenshots for both supported platforms and renderer
configurations. Document the remaining optional ImGui debug overlay.

**Exit:** The acceptance checklist below passes on the supported platform
matrix. Only then remove ImGui as a production editor presentation layer;
retain it only where engine debug overlays still need it.

## Acceptance checklist

- World is the useful default workspace and the viewport remains the primary
  work area.
- Project, World, Script, Blueprint, Animation, AI, Audio, UI, Material,
  Sequence, Network, Profiler, and other shipped tools are reachable through
  named workspaces and the Tools directory.
- Ctrl+K filters and invokes commands, tools, assets, and open documents with
  keyboard-only operation and context-aware availability.
- Selection, dirty state, undo/redo, document state, and Play state agree
  across migrated surfaces.
- Hierarchy, Inspector, Content, Console, and Build docks can be moved,
  tabbed, hidden, restored, and reset without losing project data.
- Errors lead to useful recovery actions or relevant assets/entities/source
  when those links exist.
- Representative workflows pass on every supported Qt platform and RHI
  combination; screenshots are reviewed at common and high-DPI sizes.
- Qt remains editor-only; runtime/player and packaged game targets do not
  acquire a Qt dependency.

## Deferred decisions

- Final theme, density, icon set, light theme, and motion details should be
  validated against the World concept and user feedback.
- Confirm whether source-control UI is first-party or plugin-provided.
- Set usability targets after capturing a baseline with new and experienced
  editor users.
- Decide extension trust, permissions, signing, and compatibility policy
  before opening third-party Qt extension APIs.
