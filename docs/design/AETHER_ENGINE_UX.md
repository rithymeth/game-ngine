# Aether Engine: complete product UX architecture

**Status:** Proposed product UX baseline for Aether Editor 2.0  
**Audience:** game developers, technical artists, level designers, animators, audio designers, QA, and engine/plugin developers  
**Related:** [Editor UI visual system](EDITOR_UI.md), [Editor workflow UX](EDITOR_UX_2.md), [UI toolkit migration ADR](EDITOR_UI_TOOLKIT_ADR.md)

This document broadens the UX design from editor chrome to the complete
Aether development experience: launching, creating projects, authoring,
testing, diagnosing, collaborating, extending, and shipping games. It
organizes existing engine capabilities into one discoverable product
workflow. It does not claim every capability is already implemented.

## 1. Product outcome

A developer should be able to:

1. Find or create a project and understand its build/platform state.
2. Import assets and see their dependencies, problems, and preview.
3. Build worlds and gameplay through visual and code workflows.
4. Test changes without losing authored data.
5. Diagnose performance and correctness issues from the failing object
   directly to the relevant tool or source.
6. Cook and package a reproducible game build.
7. Return later to the same project, selection, documents, and layout.

The editor is the common control surface for the engine. Specialized tools
remain focused, while navigation, commands, search, selection, history,
diagnostics, asset context, and project status work consistently everywhere.

## 2. UX principles

- **Lead with tasks.** Organize entry points around Create, Open, Import,
  Build, Test, and Ship. Feature names remain available for expert navigation.
- **Keep context intact.** Workspaces and asset editors share the same project,
  active document, selection, undo history, and diagnostics.
- **Make state visible.** Show which project, world, configuration, target,
  play state, save state, and build result the user is acting on.
- **Recover at the point of failure.** Errors link to the responsible asset,
  entity, component, source line, or build step when that relation is known.
- **Keep actions predictable.** The same search, inspector field, drag/drop,
  undo, notification, and confirmation rules apply across tool families.
- **Respect expertise and accessibility.** Provide discoverable defaults and
  hints, plus keyboard-first operation, compact density, scalable UI, and
  direct access to advanced controls.
- **Separate authoring and runtime.** Editor UX services produce validated
  authoring data and cooked runtime data through explicit boundaries.
- **Remain useful offline.** Core editor, project, build, and debug flows must
  not depend on network services or AI.

## 3. Product surfaces

### Project Hub

The Hub is the first launch surface. It provides:

- Recent projects with name, path, engine version, project type, last opened,
  source-control state when available, and last build result.
- Create, Open, Import, Clone, Open Folder, and project recovery actions.
- Search and filters for recent projects.
- A creation flow that collects only essentials first: template, project
  name, location, renderer/platform defaults. Optional systems and advanced
  settings live in an expandable step.
- Validation before creation: path exists/writable, project name valid,
  required disk space when estimable, and engine version compatibility.
- A clear outcome screen with Open Project, Show Folder, and View Log actions.

Templates should be task-based (2D, third-person, first-person, top-down,
multiplayer, open world, blank) and state what systems/assets they include.
Do not imply a template contains capabilities that the project does not
actually generate.

### Editor

The desktop editor hosts all authoring and diagnostic workspaces. The default
workspace is World. Other workspaces are named by developer task, not by
internal engine module.

### Build & Ship

Build, cook, package, launch, and inspect outputs in one target-focused flow.
Remember the last valid target/configuration per project, display missing
requirements before starting, and retain a navigable history of results.

### Help & Learning

Contextual help opens the relevant manual page, API reference, or tutorial
for the current panel, asset, error, or setting. Tutorials use the same
project and tools as production work. The first-run path is skippable and
resumable; it must not block opening an existing project.

## 4. Application navigation and shared shell

Use the five-region shell described in EDITOR_UX_2.md:

1. Application bar: project identity, menus, global search, dirty state.
2. Workspace bar: task-oriented workspaces.
3. Main authoring area: viewport, graph, document, or timeline.
4. Context panels: Hierarchy, Project/Content, Inspector, Details.
5. Diagnostics and status: Console, Profiler, Build, Debug, Network, Logs.

The shell owns window focus and activation, document tabs, common commands,
notifications, global selection display, and layout persistence. Workspace
switching changes the panel arrangement and primary tool; it never opens a
different project world or discards state.

### Global command and search model

Ctrl+K opens a unified command palette. Search modes can be scoped by prefix
or filter chips:

- Commands and menu actions.
- Tools and workspaces.
- Assets and folders.
- Entities in the active world.
- Open/recent documents.
- Settings and help pages.

Each result shows its category and shortcut. Results are permission/context
aware; unavailable commands explain what prerequisite is missing. Keyboard
behavior is consistent: typing filters, arrows move, Enter invokes, Escape
closes and restores focus. Search results must not trigger destructive
actions without an explicit second step.

### Menus and shortcuts

Keep a small stable menu taxonomy: File, Edit, Asset, Entity, Scene, Build,
Debug, Window, Help. Shortcuts appear in menus, tooltips, and the command
palette. Shortcuts are user-customizable with conflict reporting. Use platform
conventions for modifier keys and reserve a documented global set for Save,
Undo/Redo, Search, Play/Stop, and Command Palette.

## 5. Workspaces and their purpose

Every workspace uses the common shell, selection, command service, document
tabs, diagnostics, and content browser. Panels can be docked, tabbed, split,
floated, hidden, and restored. Ship role presets and allow user layouts.

| Workspace | Primary task | Core context |
|---|---|---|
| **World** | Place, select, transform, and validate entities | Viewport, Hierarchy, Inspector, Content |
| **Script** | Edit Luau/C++ integration and inspect code diagnostics | Code, API/help, Console, Debug |
| **Blueprint** | Build visual gameplay logic | Graph, node library, variables/functions, validation/debug |
| **Animation** | Edit clips, graphs, blend spaces, skeletons, IK, retargeting | Preview viewport, timeline, skeleton, details |
| **AI** | Build and debug behavior, perception, and navigation | Behavior graph, blackboard, live state, nav visualization |
| **Audio** | Create cues and tune mixer routing | Cue graph, waveform, mixer, preview |
| **Material** | Build and validate materials/shaders | Node graph, live preview, parameters, compile/complexity |
| **UI** | Design runtime interfaces | Canvas preview, widget tree, properties, asset palette |
| **2D** | Paint tilemaps and inspect 2D scenes | 2D viewport, tile palette, layers, entity properties |
| **Sequence** | Arrange cinematics and timed events | Timeline, tracks, preview, event details |
| **VFX** | Author and preview particle/effect systems | Graph, preview, parameters, validation |
| **Network** | Inspect sessions, replication, prediction, and traffic | Session view, entity ownership, rates, traces |
| **Profiler** | Find frame-time, memory, job, and network bottlenecks | Timeline, hierarchy, captures, comparisons |
| **Gameplay Debugger** | Inspect live entities and gameplay systems | World selection, attributes/effects/abilities, logs |
| **Data & Localization** | Inspect save data, data assets, and localized strings | Tables, resource/locale selection, validation |
| **Project** | Configure plugins, modules, settings, and build targets | Project settings, plugins, build/package |

Role presets are starting arrangements rather than permission levels:

- Level Designer: World, Hierarchy, Inspector, Content.
- Programmer: Script/Blueprint, Console, Debug, Profiler.
- Technical Artist: Material, VFX, Animation, Profiler, Content.
- Animator: Animation, preview viewport, skeleton, timeline.
- Audio Designer: Audio, waveform, mixer, Content.
- Network Engineer: Network, Profiler, Console, World.
- QA: Build history, test/debug tools, logs, reproducibility information.

The current categorized Tools hub remains available during migration and as
a complete alphabetical/category directory.

## 6. Core shared panels and services

### Content Browser / Asset Registry

The Content Browser is the project-wide home for assets. Support list and
thumbnail views, folder tree, breadcrumbs, search, type/status filters,
sorting, source-control badges, dependencies, and importer status. Every
asset row supports a useful preview or type icon, meaningful metadata, and
context actions appropriate to its type.

Double-click opens the correct editor. Single-click selects. Drag/drop previews
the target operation and reports incompatibility before applying it. Common
actions include reimport, re-cook, duplicate, rename, find references,
show dependencies, create related asset, and show on disk. File deletion and
overwrite actions clearly distinguish reversible editor operations from
irreversible disk operations.

### Hierarchy and selection

Hierarchy search, filters, grouping, multi-select, parent/child traversal,
visibility/lock state, and create/reparent actions behave consistently. A
selection made in the viewport, hierarchy, graph, timeline, debugger, or
search result is reflected in the shared selection service. Where an item
has no world entity, the Inspector shows that item's properties.

### Inspector and Details

Inspector sections are searchable, collapsible, and grouped by user task.
Reflected fields share consistent controls, labels, units, validation, reset,
copy/paste, and tooltips. Multi-selection shows common fields and marks
mixed values. Long and advanced sections remain collapsed until needed.
Asset and entity inspectors link to dependencies and related tools.

### Undo, save, and documents

Each opened scene or asset has a document tab with dirty state. Save, Save
All, Undo, Redo, close, and recovery behavior work across editor hosts.
Undo history is transaction-based and grouped by user gesture. Play mode
makes the active world and history behavior explicit; stopping play restores
the authored world and selection. No panel silently writes runtime-only state
into authoring data.

### Console, diagnostics, and notifications

Console entries have severity, timestamp, category, source, and searchable
context. Selecting a diagnostic navigates to its cause where possible.
Notifications are concise, non-modal, and actionable. Long operations expose
progress, cancellation when safe, and a persistent result record. Errors
distinguish recoverable warnings, blocked actions, failed imports, compile
errors, runtime exceptions, and build failures.

### Profiler and capture system

Profiler captures can be started, stopped, named, saved, and compared.
Show frame time plus CPU/GPU tracks and subsystem breakdown only when
instrumentation provides those values. Clicking a sample navigates to the
responsible system, entity, or resource. Capture metadata includes build,
target, configuration, device, and scene so comparisons remain meaningful.

### Settings

Separate Project Settings from Editor Preferences. Project settings cover
rendering, physics, input, audio, networking, AI, animation, packaging, and
plugins. Editor preferences cover appearance, scale, shortcuts, layout,
performance, debugging, and accessibility. Every setting explains its
scope, default, restart requirement, and effect on builds where relevant.

## 7. End-to-end workflows

### Create a playable project

1. Create from a task-based template or blank project.
2. Show the generated files and systems and open a guided first scene.
3. Import or select starter assets; expose missing dependencies directly.
4. Place an entity and add a gameplay component or Blueprint.
5. Play, pause, step, and stop from the World workspace.
6. Surface runtime errors in Console and link to the entity/source.
7. Save and confirm the project is in a recoverable state.

### Import and use an asset

1. Drop files into Content or choose Import.
2. Select an importer and preview detected settings.
3. Validate format, dependencies, size, and destination.
4. Import with progress and cancellation where supported.
5. Show the asset preview, metadata, warnings, and related actions.
6. Drag into a scene, graph, material, or timeline with target feedback.

### Implement gameplay

1. Choose Blueprint or Script from an entity/component context.
2. Open the correct document with the relevant entity/API context.
3. Use shared search, autocomplete/node search, diagnostics, and help.
4. Save and validate before play; show errors at their source.
5. Debug live values and execution state; stop without altering authored data.

### Diagnose performance

1. Open or record a capture from Play or Profiler.
2. Show a stable frame summary and available subsystem tracks.
3. Select an expensive event to reveal call/system/resource context.
4. Navigate to the responsible entity, asset, or setting.
5. Preview changes and capture again for an apples-to-apples comparison.
6. Any automated assistant suggestion is optional, explains evidence and
   expected impact, and requires review before making edits.

### Build and ship

1. Select target, architecture, configuration, and renderer.
2. Validate toolchains, plugins, assets, platform support, and disk location.
3. Cook assets with per-stage progress and actionable failures.
4. Package reproducibly, record settings and source revision, and preserve
   logs/manifests.
5. Present success/failure summary with duration, size, warnings/errors, and
   Launch, Open Folder, Copy Log Path, and Rebuild actions.
6. Keep previous successful outputs and build history accessible.

### Extend the editor

1. Discover installed plugins, project scripts, and available extension points.
2. Inspect permissions, version compatibility, and dependencies before enable.
3. Show plugin-provided tools under Extensions while preserving the global
   shell conventions.
4. Report load failures with plugin identity and log link; allow safe disable.
5. Project/editor APIs remain stable and versioned; extension panels cannot
   bypass core command, selection, and undo rules for supported edits.

## 8. UI states and feedback contract

Every major surface defines these states:

| State | Required presentation |
|---|---|
| Empty | State what is absent and provide the next action |
| Loading | Show progress or activity without blocking unrelated work |
| Ready | Show current context and available actions |
| Dirty | Identify the exact document/project needing save |
| Read-only | Explain the reason and how to regain edit access |
| Warning | Describe the consequence and offer a safe continuation |
| Error | Identify the failed operation, affected object, and recovery path |
| Offline | Keep local workflows usable and label unavailable services |
| Play/Simulate | Mark active world, editability, and stop behavior |
| Build/Cook | Show target, stage, progress, logs, cancellation, and result |

Do not use color as the only signal. Avoid false precision: unknown metrics are
labeled unavailable instead of represented as zero.

## 9. Accessibility, input, and visual consistency

Follow the detailed design tokens in EDITOR_UI.md and apply these product
requirements across all workspaces:

- Complete keyboard navigation, visible focus, deterministic tab order, and
  shortcut conflict reporting.
- Text contrast and status meaning do not rely only on color.
- UI scale supports small and high-DPI screens; layouts recover when panel
  geometry from another display is unusable.
- Dense mode is optional and does not remove labels or tooltips needed for
  comprehension.
- Reduced-motion preference disables nonessential animation.
- Screen-reader names exist for actionable controls and status values.
- Error text can be copied; icons have text labels in menus and accessible
  names in the UI.
- Preserve consistent units, numeric precision, validation, search, and
  property editing patterns.

## 10. UI framework and rendering boundary

The proposed toolkit direction is Qt 6 Widgets for the desktop shell and
panels, pending the validation gate in EDITOR_UI_TOOLKIT_ADR.md. Qt is a
presentation dependency of editor targets only. It does not replace the Aether
RHI, renderer, runtime, or game UI module.

Aether's native viewport remains rendered by the existing RHI. The editor
host owns menus, docks, documents, input routing, and platform window
integration. Prove native surface creation, resize, focus, input, render-loop
ownership, and shutdown with the Vulkan and D3D12 backends before replacing
production panels.

Dear ImGui may remain available for debug overlays and during migration.
New shared services must not expose Qt or ImGui types.

### Target service boundaries

- **Editor shell:** menus, workspaces, document tabs, layout, global search.
- **Project service:** project open/create, settings, plugins, module state.
- **Document service:** open/close/save, dirty state, recovery.
- **Selection service:** entity/asset/document selection and context changes.
- **Command service:** IDs, enablement, shortcuts, undo transactions, history.
- **Asset service:** registry, imports, dependencies, references, previews.
- **Diagnostics service:** console events, errors, notifications, navigation.
- **Build service:** validation, cook/package steps, manifests, history.
- **Engine adapters:** authoring world, play session, RHI viewport, systems.
- **Presentation adapters:** Qt Widgets and temporary ImGui host implementations.

The data flow is:

    Qt/ImGui presentation
        -> editor services and authoring commands
        -> authoring data / editor world
        -> validate, compile, cook
        -> runtime data and packaged game

Runtime/player targets depend on none of the presentation adapters.

## 11. Migration and delivery plan

### Stage 0 — Baseline and toolkit spike

Capture current workflows and screenshots for both editor hosts. Prototype
Qt Widgets with dockable Hierarchy, Inspector, Content, Console, saved
layouts, Ctrl+K, and the existing RHI viewport. Gate adoption on platform
builds, input/focus, DPI, viewport lifecycle, packaging, and license review.

### Stage 1 — Shared services

Add stable command IDs, document state, selection context, global diagnostics,
search indexing, and common undo transactions. Keep existing ImGui panels
operational through adapters. Verify headless command and state tests.

### Stage 2 — Shell and first-run experience

Deliver Project Hub, menus, workspace bar, command palette, status strip,
saved layouts, settings separation, recovery and help entry points. Preserve
the Tools hub as a migration directory.

### Stage 3 — Core authoring surfaces

Migrate World, Hierarchy, Inspector, Content Browser, Blueprint, Script,
Material, Console, and Build first. These cover the main create-edit-test-ship
loop. Validate keyboard and mouse parity in Windows/Vulkan and D3D12 hosts.

### Stage 4 — Specialist workspaces

Migrate Animation, AI/Navigation, Audio/Mixer, UI, 2D, Sequencer, VFX,
Network, Profiler, Gameplay Debugger, Save Inspector, Localization, Plugins,
and project settings. Each retains shared commands, selection, document, and
diagnostic behavior.

### Stage 5 — Stabilization and retirement

Validate recovery, saved layout versioning, multi-monitor/DPI behavior,
accessibility, performance on large projects, plugin compatibility, and
headless/CI screenshots. Retire ImGui as the production editor framework only
after every listed workflow has an equivalent; keep optional debug overlays.

## 12. Acceptance and success measures

The redesign is complete when:

- Every shipped editor tool appears in a named workspace and the Tools index.
- Core project, asset, world, gameplay, test, debug, profile, and package flows
  can be completed without leaving the editor.
- Both editor hosts use the same command IDs, selection/document semantics,
  and shared workflow rules.
- Ctrl+K can find all registered commands, tools, assets, and open documents
  and is fully keyboard-operable.
- Workspace layouts survive restart and reset safely after version changes.
- Errors navigate to relevant context where available; build outputs retain
  useful logs and reproducibility metadata.
- Play/Stop never corrupts authored data; dirty state and recovery are clear.
- Runtime/player targets have no editor UI toolkit dependency.
- Accessibility and platform input checks run for representative controls and
  workflows in CI or a documented manual gate.

Before broad release, establish a usability baseline with new and experienced
developers. Measure task completion, recovery from common errors, discoverability
of major tools, and time spent navigating between related tasks. Set numeric
targets after that baseline rather than inventing them in advance.

## 13. Open product decisions

Resolve these as implementation gates, not hidden assumptions:

1. Confirm Aether's project license and Qt module/deployment compatibility.
2. Confirm supported desktop platforms and renderer combinations for the
   editor, distinct from game runtime targets.
3. Decide whether source-control UI is first-party or plugin-provided.
4. Define plugin trust, permissions, signing, and project compatibility.
5. Define crash recovery and autosave retention policy.
6. Decide what collaboration is in scope; the initial workflow remains useful
   without a remote service.
7. Decide whether editor AI is offered; core operation must remain independent
   of it.
