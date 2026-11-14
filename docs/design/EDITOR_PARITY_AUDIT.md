# Editor parity audit: Aether against Unreal, Unity and Godot

Audited on 2026-11-13 against `master` at `01a013e7`. Line numbers below are from that commit; they move.

This is the audit the [editor roadmap](EDITOR_ROADMAP.md) did not have: a feature-by-feature comparison with the three
editors the request names, evidence for every Aether claim, and the measured cost of the gaps that have been measured.
It is deliberately written before any code changes, so the order of work can be argued with.

## The short version

The hard parts are present. There is a real renderer running inside the editor (Live Viewport), triangle-accurate picking,
a transform gizmo with world/local spaces and snapping, one command stack, 33 tools, a CVar console with history and
completion, a cook/package pipeline, and an installer. What is missing is the scaffolding that makes those parts feel like
one application: a project hub, a global toolbar, document tabs with dirty state, preferences, remappable commands, a
viewport that can show the scene lit from any angle, and a content pipeline that can import, reimport and preview.

The most serious gap is not visual. **Quit discards unsaved work with no prompt** (`File > Quit` sets the shell's quit
flag at `editor/shell/main.cpp:78`), even though `SceneDocument` already implements autosave
(`editor/src/core/scene_document.cpp:202`) and nothing in the editor calls it. A parity audit that fixes menus before
this would be upside down.

## What is already strong

- **Live editing without a reload.** Edits are patched into the running game: transforms, names, models, lights, sky,
  wind, post-process volumes, decals, material parameters, parent changes and physics bodies. The patch is 0.3 ms at
  2,014 entities; the reload it replaces is 5 ms to save plus 11.5 ms to load before the slower frame after it
  (`docs/design/PERFORMANCE.md:1475`).
- **Picking by shape, not bounding box.** A click is tested against the model's triangles up to 300,000 triangles
  (`editor/src/world/viewport_tools.h:36`, `editor/src/world/model_bounds.h:43`), with the box as the broad phase.
- **One undo stack for the scene.** Every reflected component edit, gizmo drag, hierarchy change and prefab placement
  is a command (`editor/src/core/command_stack.cpp`).
- **A real console.** It is a CVar command console with history, Tab completion and filtering
  (`editor/src/devtools/console_panel.cpp:61-83`), not a read-only log.
- **A complete desktop ship path.** Cook, package, manifest verification, smoke run and a tested installer
  (`tools/package/main.cpp`, `tools/build_installer.py`, `tools/installer/`).

These are worth protecting while the workflow pieces are built; several later stages reuse them rather than replacing
them.

## Scorecard

No dimension is marked `Ahead`: in the areas where Aether is strongest, the reference editors do the same job at least
as well. The verdicts are a judgement from the source audit; the evidence column is the part that is verifiable.

| Dimension | Aether today | Reference editors | Verdict |
|---|---|---|---|
| Project lifecycle | A Project Hub shown on a launch with no project (and File > Project Hub): recent projects with missing ones pruned, a picked or typed path, and template creation; File > Open Project goes through it | Project Browser/templates (Unreal), Unity Hub, Project Manager (Godot) | Partial |
| Menus and commands | File, Edit, Asset, Actor, Tools, View, Window and Help; Window > Layout has the four built-in arrangements, saved presets and Save Layout As; one command table drives the menus, the palette and a generated shortcut cheat sheet | Complete menu bars plus searchable asset/command surfaces in all three | Partial |
| Preferences and theme | One hardcoded dark theme; no settings file, theme choice or UI scale | Editor Preferences (Unreal, Unity), Editor Settings/themes (Godot) | Missing |
| Shortcut handling | One command table with default chords and scopes, conflict detection (a startup test requires none) and a cheat sheet generated from it; no remapping yet | Remappable shortcuts and conflict reporting in all three | Partial |
| Documents and layout | One scene document; one tool shown at a time (popped-out tools are separate windows); only the open tool and ImGui docking are remembered | Multi-document tabs, per-asset tabs, layout presets | Partial |
| Data safety on exit | Quit is immediate; autosave exists but is unwired; unsaved-scene prompts exist for New/Open only | Close/quit prompts and recoverable autosave in all three | Missing |
| Hierarchy | Tree with name filter, drag reparent, Ctrl multi-select, create/duplicate/delete/copy/paste | Outliner/Scene dock with visibility, lock, icons, multi-edit | Partial |
| Viewport | Wireframe overlay, orbit camera, ±20 m grid; Live Viewport is a separate rendered tool | Lit/wireframe modes, fly camera, orthographic views, bookmarks, show flags | Partial |
| Selection | Click, Ctrl toggle, marquee (replace/add/subtract), Alt click-through, Ctrl+A and invert in the Scene View and Live Viewport | Marquee, click-through, select-all/invert in all three | Partial |
| Transform | Move/rotate/scale, world/local, per-mode snap, pivot/centre, plane squares for Move and Scale, uniform scale, Euler rotation fields | Pivot/center modes, plane and uniform scale handles, Euler entry, numeric transform | Partial |
| Content browser | Flat folder list, name search, rename/move, references, VCS badges, drag sources; no thumbnails, filters, multi-select or import UI | Thumbnails, type filters, multi-select, import/reimport, create/duplicate/delete | Partial |
| Inspector | Every reflected component, add/remove with search, prefab revert, asset drop targets; single-object only | Multi-object editing, property search, reset/copy/paste, mixed values | Partial |
| Play in editor | Play/Pause/Resume/Stop in the Scene panel; a separate Live Viewport with **Run game**; `Step()` exists and is unreachable | Play/Simulate, Pause, Step, Eject/Possess in the viewport | Partial |
| Asset editors | 33 tools with uneven document models: some save and undo, terrain/foliage have neither, localization has no undo | One tab/document model, save and undo everywhere | Partial |
| Build and package | Build and Package tool, `aether_package` CLI, verified installers | Build/package from the main toolbar with progress | At parity for Windows desktop |
| Diagnostics | Console filter/history/completion, profiler, crash reports; error overlay widget exists but is wired nowhere | Message log linked to objects, console severity filters, frame debugger, profiler | Partial |
| Settings, accessibility, localization | No UI scale, keyboard-navigation pass, colorblind check or editor localization | UI scale and accessibility options in all three | Missing |
| Collaboration | VCS status badges and stage-all commit | Per-file source control, scene/asset diffs, review workflows | Partial |

## Shell and project workflow

| Capability | Unreal Editor | Unity Editor | Godot Editor | Aether today | Evidence | Gap |
|---|---|---|---|---|---|---|
| Launch and project selection | Project Browser with templates and recent projects | Unity Hub, templates, recent projects | Project Manager, templates, import/scan | Project Hub on launch when no project is given; the template list, a picked path and the recent list in one window | `editor/src/workspace/editor_workspace.cpp` (`DrawProjectHub`), `editor/shell/main.cpp` | Partial |
| Recent projects | In the project browser | In the Hub | In the Project Manager | Listed newest first and saved per user; missing entries are marked, removable individually or in one action; opening a project records it | `editor/src/core/recent_projects.cpp`, `editor_workspace.cpp` (`Impl::OpenProject`) | Partial |
| Main toolbar | Save/undo/redo, transform tools, play controls, build/package, settings | Transform tools, play controls, layers, layout | Transform tools, play controls, scene tabs | One row under the menu bar: Save, Undo, Redo, the Scene View's move/rotate/scale, world/local and snap, Play/Pause/Step/Stop with a state underline, Build and Package, Settings | `editor/src/workspace/editor_workspace.cpp` (`DrawMainToolbar`), `editor/shell/main.cpp` | Partial |
| Menus | File, Edit, Window, Tools, Build, Help | File, Edit, Assets, GameObject, Component, Window, Help | Scene, Project, Debug, Editor, Help | File, Edit, Asset, Actor, Tools, View, Window and Help | `editor/shell/main.cpp` (`DrawEditor`), `editor_workspace.cpp` (`DrawMenuItems`, `DrawHelpMenu`) | Partial |
| Document tabs | Level and asset editors as tabs | Scene, Game, asset editors as tabs | Scenes and scripts as main-screen tabs | One scene; one hub tool visible; popped-out tools | `editor/src/world/scene_panel.h:88` | Partial |
| Layout presets | Saved layouts, per-user | Layouts per project/user | Editor layouts | Window > Layout: Default, Level design, Scripting and Animation, saved per project in `Saved/Layouts/*.ini`, plus Save Layout As and Reset to Default | `editor/src/workspace/layout_presets.{h,cpp}`, `editor/shell/main.cpp` (`BuildDockLayout`, `DrawWindowMenu`) | Complete for this shell |
| Preferences | Editor Preferences, project settings, plugins | Preferences, Project Settings, Package Manager | Editor Settings, Project Settings, Input Map, plugins | None | — | Missing |
| Theme and UI scale | Theme, UI scale, DPI awareness | Skin, UI scaling | Editor themes, editor scale | One dark theme; `ApplyEditorTheme(font_size)` has no caller-supplied setting | `editor/src/ui/editor_theme.cpp:33`, `main.cpp:189` | Missing |
| Shortcut mapping | Remappable, conflict reporting | Shortcuts manager | Remappable input map/editor shortcuts | One table of default chords with scopes, conflict detection at startup and a cheat sheet generated from it; no remapping UI | `editor/src/workspace/command_registry.cpp`, `editor_workspace.cpp` (`DrawShortcutCheatSheet`) | Partial |
| Command palette | Command palette in recent versions | Search (Ctrl+K) | Quick Open | Implemented and bound to Ctrl+Shift+P and View > Command Palette | `editor/src/workspace/command_registry.cpp` (`view.command_palette`), `editor_workspace.cpp` (`DrawCommandPalette`) | Partial |
| Native file dialogs | Native open/save/import | Native | Native | Open Scene, Save Scene As and the toolbar's "..." use the system picker (`IFileDialog`; zenity/kdialog on Linux); the typed path remains the fallback | `editor/src/core/file_dialog.cpp`, `editor/src/world/scene_panel.cpp:74-100` | Partial |
| Unsaved work on exit | Prompt per document | Prompt; play-mode changes discarded with warning | Prompt | Quit immediate; only New/Open prompt | `editor/shell/main.cpp:78`, `scene_panel.cpp:70-96` | Missing |
| Autosave/recovery | Autosave and recovery | Autosave and recovery | Autosave backups | `SaveAutosave`/`LoadAutosave` exist and are unused | `editor/src/core/scene_document.cpp:202-226` | Missing |
| Status bar | Project, source control, progress, PIE state | Scene, play state, console counts | Debug state, network, version | Project, document name with a dirty mark, play state, an active cook/package or launched game, and unread messages as a button; the host adds backend and frame time; no source-control branch yet | `editor/src/workspace/editor_workspace.cpp` (`StatusBar`, `DrawStatusBar`), `editor/shell/main.cpp` | Partial |

## World editing, viewport and transforms

| Capability | Unreal Editor | Unity Editor | Godot Editor | Aether today | Evidence | Gap |
|---|---|---|---|---|---|---|
| View modes | Lit, Unlit, Wireframe, Buffer Visualization, show flags | Shaded, Wireframe, Shaded Wireframe, draw modes | Perspective/orthogonal, debug draw modes | Wireframe boxes and lines only; rendered picture only in Live Viewport | `editor/src/world/scene_view_panel.cpp:70-140` | Partial |
| Projection | Perspective and orthographic (top/front/side) | Perspective and orthographic, 2D/3D toggle | Perspective and orthogonal, 3D/2D screens | Perspective only | `editor/src/world/scene_view.cpp:41` | Missing |
| Camera navigation | Fly (WASD), orbit, pan, focus, speed control, bookmarks | Fly/orbit/pan, focus, speed, bookmarks | Fly/orbit/pan, focus, view menu | Right-drag orbit, middle/shift pan, wheel zoom, F focus | `scene_view.cpp:64-79`, `scene_view_panel.cpp:26-45` | Partial |
| World extent | Large-world grid, snap to grid, world partition | Grid with configurable size | Grid with configurable size | Endless fading grid; zoom 0.5–20 000 m | `scene_view_panel.cpp:78-86`, `scene_view.cpp:54` | Partial |
| Viewport stats | FPS, draw calls, triangles, memory | Stats overlay | Monitor/debug overlays | FPS and entity count only in Live Viewport | `editor/src/host/live_viewport_panel.cpp:224` | Partial |
| Selection | Click, Ctrl, marquee, select-all, click-through | Click, Ctrl, marquee, select-all, scene picking | Click, Ctrl, box select, select-all | Click, Ctrl toggle, marquee, select-all/invert, Alt click-through | `editor/src/world/viewport_tools.cpp:51-110` | Partial |
| Picking accuracy | Rendered object ID / physics | Rendered object ID | Rendered object ID | Bind-pose triangles (≤300k), else box; no animated/alpha-accurate pick | `editor/src/world/model_bounds.h:43`, `scene_view.cpp:126-206` | Partial |
| Transform gizmo | Translate/rotate/scale, screen/plane handles, pivot modes | Translate/rotate/scale/rect, pivot/center, plane and uniform handles | Translate/rotate/scale, local/global, snap | Move/rotate/scale, world/local, pivot/centre, plane squares for Move and Scale, uniform scale box, snap per mode, handles a fixed number of pixels long | `editor/src/world/viewport_tools.h:16-56`, `scene_view.cpp:427` | Done |
| Pivot | Center of selection or each actor's pivot | Center/Pivot toggle | Local/global transform pivot | Pivot (first selected origin) / Centre (middle of the selection) toggle in the toolbar and on P; turns and group scaling go round it | `viewport_tools.cpp:35-70`, `scene_view.cpp` (`TransformGizmo::Pivot`) | Done |
| Numeric transform | Location/rotation/scale fields, relative/absolute | Vector fields, Euler rotation | Transform fields, Euler | Reflected Transform position/scale rows; rotation is three Euler drags in degrees (X pitch, Y yaw, Z roll), written back as the quaternion | `editor/src/ui/reflected_inspector.cpp` | Partial |
| Multi-edit undo | One transaction per user action | One undo entry per action | One action per undo | Delete, duplicate and paste of several entities are one entry each; gizmo drags group the whole subject set | `editor/src/world/scene_panel.cpp:232-268` | Done |
| Play state in the viewport | PIE border, pause tint, banner, eject | Play-mode tint, Game view, pause | Embedded game, pause overlay | No viewport PIE; `Run game` checkbox in Live Viewport | `live_viewport_panel.cpp:216` | Partial |

## Content browser, inspector and hierarchy

| Capability | Unreal Editor | Unity Editor | Godot Editor | Aether today | Evidence | Gap |
|---|---|---|---|---|---|---|
| Browser presentation | Grid/list, thumbnails, collections | Grid/list, thumbnails, labels | Grid/list, thumbnails | List of names only | `editor/src/content/content_browser_panel.cpp:122-235` | Partial |
| Search and filters | Name, type, path, collections | Name, type, labels, `t:` queries | Name, type, path | Name substring in the current listing | `content_browser_panel.cpp:138` | Partial |
| Multi-select and file operations | Multi-select, duplicate, delete, rename, move, fix up redirectors | Multi-select, duplicate, delete, rename, move | Multi-select, duplicate, delete, rename, move | Single selection; rename, move, new folder | `content_browser_panel.cpp:207-215` | Partial |
| Thumbnails | Rendered asset thumbnails | Rendered asset thumbnails | Rendered resource thumbnails | None; the engine's thumbnail API is unused by the editor | `engine/include/aether/assets/content_browser.h:121-127` | Missing |
| Import and reimport | Import dialog, per-asset import settings, reimport | Import settings in the Inspector, reimport | Import dock, per-asset import settings | No importer UI; cook imports assets | No `ImportSettings`/reimport UI in `editor/` | Missing |
| Open an asset | Double-click opens its editor | Double-click opens its editor | Double-click opens its editor | Openers registered for `.dialogue`, `.astrings`, `.asav`, `.asettings` only | `editor_workspace.cpp:155`, `:651-657`, `:753-762` | Partial |
| Create assets | Right-click create menu for every asset type | Create menu for every asset type | Create New menu | New folder; entities are created in the hierarchy; no create-asset menu in the browser | `content_browser_panel.cpp:212-215` | Partial |
| References | Reference viewer and dependency graph | Find references/dependencies | Resource dependencies | References window (referencers/references) | `content_browser_panel.cpp:351` | Partial |
| Inspector coverage | Every reflected property, categories | Every serialized field, custom editors | Every exported property | Every reflected component, categories, tooltips, prefab revert | `editor/src/ui/reflected_inspector.cpp:445` | At parity |
| Property search | Details search | Inspector search | Property search | Component filter only | `reflected_inspector.cpp:445` | Partial |
| Multi-object editing | Shared values, mixed state | Shared values, mixed state | Multi-node editing | First selected entity only | `editor/src/world/scene_panel.cpp:436-448` | Missing |
| Copy/reset values | Copy/paste values, reset to default | Copy/paste values, reset | Copy/paste, revert | Prefab revert only | `reflected_inspector.cpp:153` | Partial |
| Hierarchy entity controls | Visibility, lock, folders, icons | Active toggle, icons, prefab state | Visibility, lock, icons | Name, selection, reparent, context menu; no visibility/lock | `scene_panel.cpp:330-430` | Partial |
| Hierarchy search | By name, type, path | By name and type | By name and type | Name substring; filtered rows lose the tree | `scene_panel.cpp:379-416` | Partial |

## Play in editor and diagnostics

| Capability | Unreal Editor | Unity Editor | Godot Editor | Aether today | Evidence | Gap |
|---|---|---|---|---|---|---|
| Enter play | Play, Simulate, selected viewport or new window | Play, pause, step in the main toolbar | Play scene/main, pause, step | Play/Pause/Resume/Stop in the Scene tool only | `editor/src/world/scene_panel.cpp:248-260` | Partial |
| Pause and step | Pause and single-frame step | Pause and step | Pause and step | Pause works; `Step()` exists with no caller | `editor/src/core/play_session.cpp:44`, no UI caller | Partial |
| Simulate vs play | Simulate runs without possessing a pawn | Play is the only mode | Play scene/main | `Run game` in Live Viewport is closest | `live_viewport_panel.cpp:216` | Partial |
| Eject/possess camera | Eject and possess | Scene/Game view switching | Remote scene tree while running | None | — | Missing |
| Play in the rendered viewport | Yes, inside the level viewport | Yes, Game view | Yes, embedded game | Separate Live Viewport tool; Scene View stays wireframe | `editor/shell/main.cpp:197`, `scene_view_panel.cpp:70` | Partial |
| Stop behavior | Restores editor state | Restores editor state | Restores editor state | Live Viewport keeps running until stopped/reloaded; scene PIE discards edits | `scene_panel.cpp:260` | Partial |
| Console filters | Categories, verbosity, search, source links | Log/error/warning filters, collapse, clear on play | Output, errors/warnings, search | One substring filter; no severity/category/collapse | `editor/src/devtools/console_panel.cpp:83` | Partial |
| Messages linked to objects | Message Log links to actor/asset | Console double-click opens source/asset | Errors link to script line/node | Notification service keeps the history with an entity, asset or file target; the Messages tool links back (entity selects it, asset opens its folder) | `editor/src/core/notifications.cpp`, `editor/src/ui/notification_overlay.cpp` | Partial |
| Profiler/debugger | Insights, GPU Visualizer, stat commands | Profiler, Frame Debugger, Memory Profiler | Profiler, Monitors, debugger with breakpoints | Profiler tool, net profiler, crash reports | `editor/src/devtools/`, `editor/src/net/` | Partial |

## Asset editors, build and platform

| Capability | Unreal Editor | Unity Editor | Godot Editor | Aether today | Evidence | Gap |
|---|---|---|---|---|---|---|
| One document model | Asset editors share tabs, save, undo | Asset editors share Inspector/undo | Resource editors share tabs/undo | Each tool owns its document and behavior | `editor/src/workspace/editor_workspace.cpp:305-347` | Partial |
| Save/dirty per asset | Yes | Yes | Yes | Blueprint, material, animation, audio, graph editors save; terrain and foliage do not | `editor/src/world/world_panels.cpp` (no save/load) | Partial |
| Undo per asset | Yes | Yes | Yes | Most graph editors have undo; localization does not | `editor/src/loc/localization_panel.cpp:66` | Partial |
| Live material preview | Material editor in viewport | Shader Graph preview | Shader editor preview | Material graph not drawn by the player; per-entity overrides show live | `docs/releases/v0.30.0.md` (Materials) | Partial |
| Build/package | Toolbar build, cook, package, launch | Build Settings, Build And Run | Export templates, one-click export | Build and Package tool plus `aether_package` CLI; no toolbar action | `tools/package/main.cpp`, `editor/src/packaging/` | At parity (desktop) |
| Plugin management | Plugin browser, enable/disable | Package Manager | Project Settings > Plugins, Asset Library | Plugins tool; no enable/disable workflow documented in-editor | `editor_workspace.cpp:343` | Partial |
| Help and documentation | Help menu, docs, console `help` | Help menu, manual, API docs | Help menu, class reference | Help menu with a generated keyboard-shortcut sheet (F1) and About (engine version, build kind, tool count, open project); console has `help`; no docs or API links in-editor yet | `editor/shell/main.cpp` (`DrawHelpMenu`), `editor_workspace.cpp` (`DrawShortcutCheatSheet`, `DrawAbout`) | Partial |
| Editor localization | Yes | Partial/localizable | Yes | No; UI strings are literals | source audit | Missing |

## The gaps that cost the most, with numbers

These are the gaps where "it is missing" can be given a cost, or where the measured number changes the priority.

1. **Iteration latency is dominated by the frame, not by the edit.** At 960x540 with 2,014 entities, patching an edit
   costs 0.3 ms and the next frame 76 ms; the reload route costs 116 ms end to end. The Live Viewport caps itself at
   30 frames a second (`editor/src/host/live_viewport_panel.h:60`). All of these numbers are from lavapipe, the CPU
   Vulkan driver; the same scene on a real GPU was not measured (`docs/design/PERFORMANCE.md:1475-1512`). Until it is,
   "the editor is slow" cannot be separated from "this machine has no GPU driver".
2. **Picking is linear in triangles.** 0.514 ms for an 81,920-triangle model, about 6 ns per triangle, so the 300,000
   triangle cap is about 2 ms per pick — and the Scene View picks every frame for the hover outline
   (`docs/design/PERFORMANCE.md:1514`). Measured at the cap since: 1.3 ms a pick at 297,192 triangles (2.1 ms with the gate running
   four jobs), and `ModelBounds_PickingAtTheTriangleCapStaysLinear` fails if the loop stops being linear. A bounding-volume tree is not
   needed yet; it becomes needed if the cap rises or several large models overlap.
3. **The viewport cannot show a large world.** (Done 2026-11-14: the grid is endless and fades, and the orbit distance reaches
   20,000 m, so the wireframe view can frame what the large-world systems make.)
4. **Only four asset extensions have an opener.** `.dialogue`, `.astrings`, `.asav` and `.asettings`
   (`editor_workspace.cpp:155`, `:651`, `:753`). A `.amat`, `.abp`, `.avfx`, `.acue`, `.aui`, `.abt` or `.aseq`
   double-clicked in the Content Browser does nothing, although an editor for it exists as a tool.
5. **Multi-entity actions are not one undo step.** (Done 2026-11-14: `DeleteSelection`, `DuplicateSelection` and
   `PasteClipboard` wrap several entities in one transaction, so one undo takes the whole action back; the gizmo already
   grouped a drag over its subject set.)
6. **Step is implemented and unreachable.** `PlaySession::Step()` exists (`play_session.cpp:44`) and no editor code calls
   it or `ShouldSimulate()`. This is the cheapest parity win in the audit: one button and one caller in the toolbar.
7. **The command palette and the error overlay were built but unused.** The palette is now bound to Ctrl+Shift+P through
   the command registry, and the error overlay became the severity-aware notification overlay, driven by the
   notification service. Both are wired; kept here because they show how long a finished widget can sit unconnected.
8. **Unsaved work is lost on quit.** `Quit` exits immediately (`main.cpp:78`); autosave and recovery exist in the
   document layer and are unused (`scene_document.cpp:202-226`). This is a correctness gap, and it is why it comes
   before visual parity in the plan.

## The staged plan

Sizes are relative for one engineer familiar with the code: **S** up to 3 days, **M** 1–2 weeks, **L** 3+ weeks. They
sequence the work; they are not release commitments.

The order is by dependency and workflow leverage, not by how visible a feature is. Stage 0 exists because every later
stage needs command identity, settings and a document contract; building the toolbar first would hardcode a second set
of shortcuts and a second notion of "the open document".

### Stage 0 — Safety and shared services (M)

Outcome: no work is lost on exit, and later stages have one place to register a command, a setting and a document.

- Prompt on quit for every dirty document; wire the existing autosave (interval in settings) and offer recovery on the
  next launch.
- `EditorSettings` file under `%APPDATA%/Aether` (versioned JSON): theme, UI scale, camera speed, autosave interval,
  recent projects, last project. Reuse `RecentProjects` instead of its crash-directory-only use.
- Command registry: stable id, label, default chord, context, enabled predicate. Menus, the palette and the shortcut
  poller are generated from it. Startup test rejects duplicate ids and conflicting chords.
- `EditorDocument` contract: path, dirty, save, save-as, undo stack and label, close prompt. The scene document adopts
  it in this stage; the other asset documents migrate in Stage 6. The shell uses the contract for tabs and quit.
- Notification service: toasts (info/success/warning/error), a Messages panel, and the existing error overlay wired to
  it. Errors carry an optional entity/asset/file target.
- Native open/save dialogs behind one small interface, so every path field and save action can use them.

Gate: settings round-trip test; duplicate-command and chord-conflict tests; quit-with-dirty-document test (headless
host); autosave recovery test; a headless screenshot at 100% and 150% scale.

Done so far (2026-11-13): the scene's quit prompt on both File > Quit and the title-bar X (a cancelable
`Window::on_close`), `EditorSettings` v1 (autosave on and off, interval), a per-project recovery slot written while the
document is dirty and offered on the next launch, and a Preferences window. Tests: settings defaults/round-trip/clamping,
slot write/find/clear/corruption, the autosave timer and quit flow on a headless workspace.

Done so far (2026-11-14): the command registry (`editor/src/workspace/command_registry.{h,cpp}`). A `Command` carries a
stable dotted id, a label, an optional menu and separator, a default chord, a `CommandScope` (`Global`, `Scene`,
`SceneTree`) and optional `enabled`/`label_now` functions; `Add` refuses an empty or duplicate id, and `Conflicts()`
reports two chords that could fire at once, where `Global` overlaps every scope and the others overlap only themselves.
File, Edit and View menus are drawn from the table, the palette lists tools, then commands, then assets, and both the
workspace (`Global`) and the Scene panel (toolbar `Scene`, hierarchy `SceneTree`) poll the same table, so a chord is
written down once. Ctrl+Shift+P opens the palette through the table, not a separate check. Tests: add/find/run/refusals,
chord formatting and exact-modifier matching, the conflict rule, the built-in table's ids and menus and empty conflict
list, menus and commands acting on the scene, and both chord scopes through injected ImGui key events. Still open at that
point: the document contract, notifications and native dialogs; autosave and the quit prompt cover the scene document
only.

Done so far (2026-11-14): the `EditorDocument` contract (`editor/src/core/editor_document.{h,cpp}`): file path, display
name (the file name, or "Untitled"), kind ("scene"), dirty state, undo/redo with labels, and a save that takes the target
path. `SceneDocument` implements it, and `EditorWorkspace::ActiveDocument()` returns it, so the quit prompt asks kind,
name and dirty state of a document rather than of the Scene panel. Tests: a scene answering every contract question
(including clean-after-undo and save-as naming), the default display name, a non-scene stand-in implementing the
contract, and the workspace's quit flow agreeing with `ActiveDocument()`. At that point the notification service and native
dialogs were still open.

Done so far (2026-11-14): the notification service (`editor/src/core/notifications.{h,cpp}`) and its drawing
(`editor/src/ui/notification_overlay.{h,cpp}`, which replaces the unused error-only overlay). A message is a severity, a
title, optional detail, an optional entity/asset/file target and a repetition count; info and success toasts last 4 s,
warnings 8 s, errors until dismissed, and the history outlives all of them. Identical back-to-back posts join the newest
row, so a repeating autosave warning is one line. The Messages tool lists newest first and links back: an entity target
selects it in the scene, an asset or file opens its folder in the Content Browser; drawing the list marks it read.
`EditorWorkspace::Update` advances the clock, save/autosave/recovery results and failures post, and clicking a toast opens
Messages. Tests: history and read state, lifetime per severity, repeat grouping, age and label text, both halves drawn
headless, and a workspace whose Messages tool reads the same service.

Done so far (2026-11-14): native open/save dialogs behind one interface. `FileDialog` (`editor/src/core/file_dialog.{h,cpp}`)
takes a title, a start path, a suggested save name, file-type filters and save-or-open, and returns the absolute path or
nothing; `Available()` says whether the platform has a picker at all, which is what lets a caller fall back to typing a
path instead of mistaking "no picker" for "cancelled". Windows uses the shell's common item dialog (`IFileDialog`,
`FOS_FILEMUSTEXIST`/`FOS_OVERWRITEPROMPT`, filters from the options); Linux runs zenity or kdialog without a shell, so a
cancelled dialog is not retried on the other picker. `File > Open Scene` and `File > Save Scene As` and the Scene
toolbar's "..." button go through it, and a picked file under the content folder is shown there by its scene name. The
save picker already asked about replacing a file, so the panel is told to overwrite rather than asking twice. Tests: the
options each action builds (verb, filter, start file), a picked scene opening, Save As writing the picked name, cancel
versus no picker, a scene outside the content folder staying absolute, and both File-menu commands through to the panel,
with the typed-path fallback where no picker exists. Stage 0 is complete.

### Stage 1 — Shell, project hub and workflow (M)

Outcome: the editor opens like an application, not a tool launcher.

- Project hub on launch (open, create from template, recent, remove missing), reachable from File > Open Project.
- Main toolbar: Save, Undo/Redo, transform mode, space and snap, Play/Pause/Step/Stop, Build/Package, Settings. Step
  wires the existing `PlaySession::Step()`.
- Menu bar completion: File, Edit, Asset, Actor, Window, Help. Window owns layout presets; Help holds About (version,
  commit), documentation, API reference and the shortcut cheat sheet.
- Document tabs for scenes and asset editors, with dirty dots, middle-click close, close-others and "reopen focuses the
  existing tab".
- Status bar: project name, scene name, dirty state, PIE state, active background job, source-control branch.
- Preferences dialog driven by `EditorSettings`: theme, UI scale, autosave, camera speed, font size.
- Bind the command palette (`Ctrl+Shift+P`) and the shortcut cheat sheet; generate both from the registry.
- Layout presets: Default, Level design, Scripting, Animation, plus save-current-as. The four built-ins are DockBuilder
  arrangements; a saved preset is the ImGui ini text, so restore does not need a second layout format.
- Scene toolbar: a recents list beside the native Open/Save dialogs from Stage 0, so the typed path is the third way,
  not the only one.

Gate: launch with no project shows the hub; recent-project round-trip; tab dirty-state and close prompts; preset restore
test; shortcut-conflict test; toolbar screenshot with PIE idle/playing/paused; Step advances exactly one frame in a
headless play session.

Done so far (2026-11-14): the project hub, the first gate item. `EditorWorkspace` owns the per-user recent list
(`RecentProjects`, `%APPDATA%/Aether/recent_projects.json`, beside the settings file) and records a project when one
opens; the hub
is a window the workspace draws itself, so both shells and a headless test show the same thing. It offers a typed path
and, where the platform has a picker, Browse (the Stage 0 `FileDialog`, filtered to `.aproject`); the recent list with
each row's folder, a missing file marked and removable, and one "Remove missing projects" action; and the existing
`NewProjectPanel` template list, whose Create now opens the new project and closes the hub. File > Open Project asks
the picker and falls back to the hub when there is none, File > Project Hub opens it directly, and the shell starts on
it when neither an argument nor `AETHER_PROJECT` names a project. A project that will not load posts an Error (with the
file as its target) instead of failing quietly, and neither the open project nor the list changes. Tests:
recent-list round-trip across two workspaces, remove-missing pruning and saving, File > Open Project through the picker
(including cancel and the no-picker fallback), a bad project posting an error and changing nothing, and the hub drawn
headlessly over an open project.

Done so far (2026-11-14): the main toolbar, the Stage 1 gate's other piece. `EditorWorkspace::DrawMainToolbar` is drawn
into the shell's top side bar and is plain ImGui, so tests draw it headlessly. Save, Undo and Redo run the command
table's ids, so their enabled state and chord tooltips are the registry's (an undo says what it would undo); the
transform row is the Scene View's own `ViewportTools::DrawToolbar`, so the W/E/R/Q keys and the buttons are one state;
Play/Pause/Step/Stop drive the open document, with `SceneDocument::Step()` wired to the existing `PlaySession::Step()`
and a new `SceneDocument::ShouldSimulate()` for a host to ask, and an amber underline reports the play state without
reading labels; Build and Package and Settings open their tools (the latter Preferences) through new palette commands
`project.build` and `project.settings`. Tests: Step lets exactly one frame simulate and is ignored while playing (the
stage gate), the toolbar's enabled state following the document through undo and redo, both tool actions opening their
windows, and the toolbar drawn in editing, playing and paused states.

Done so far (2026-11-14): the Help menu and the generated shortcut cheat sheet. `CommandRegistry::Shortcuts()` returns
every chorded command as a `ShortcutRow` (id, label, chord, menu, context), in menu order and by label within a menu,
with a command that has no menu listed under "Other" instead of being dropped; `DrawShortcutCheatSheet` draws exactly
that table, so a chord the editor no longer has cannot stay listed and a new one appears with no second edit. Each row
names its context (`CommandScopeName`: anywhere, Scene tool, Scene hierarchy): a Scene chord does nothing until the
Scene tool has focus, and that is the part a reader needs. Help holds Keyboard Shortcuts (F1) and About Aether Editor
(engine version, build kind, tool count, open project), and About links to the sheet. Tests: the rows are exactly the
chorded commands, once each and carrying the table's label/chord/menu; menu ordering; F1 registered under Help; both
commands opening their windows and drawing headlessly; and a chord-only command appearing under "Other".

Done so far (2026-11-14): the status bar. `EditorWorkspace::StatusBar()` is the snapshot the bar draws -- the open
project's name, the active document's display name, whether it is dirty, the play state, an active cook/package
(with its stage and percent) or a launched game, and the unread message count -- and `DrawStatusBar` renders it into
the shell's bottom side bar, which appends the backend and frame time on the same line. The play state is amber while
not editing, matching the toolbar's underline. The message count is a `SmallButton`: clicking it opens the Messages
tool and marks the list read; merely drawing the bar does not, so an unread count means "arrived since you last
looked" in the bar too. Tests: the project/document/dirty snapshot following the document, the play state through
Playing, Paused and back, the unread count through a post and a draw (drawing alone must not clear it), and the bar
drawn headlessly in every play state.

Done so far (2026-11-14): the Asset and Actor menus. `actor.empty`, `actor.camera` and `actor.cine_camera` create the
same kinds the hierarchy's Create menu does on the open scene, select the new entity and record one undo step each;
`asset.browser`, `asset.refresh` and `asset.validate` act where assets are managed by putting the Content Browser on
screen (Refresh also posts a warning with the panel's own error when the scan fails, and Validate opens the issue
list). All six are ordinary command-table entries with no chords, so the startup conflict test covers them; the shell
draws both menus from the table. Tests: each Actor command making its kind (Camera/CineCamera components present) and
undo removing one, the registration (menu, no chord, enabled) and an empty conflict list, the Asset commands opening
the Content Browser, and both menus drawn headlessly.

Done so far (2026-11-14): the Window menu and layout presets. `LayoutPresets`
(`editor/src/workspace/layout_presets.{h,cpp}`) stores a preset as exactly the ini text ImGui already writes for its
own settings -- docking tree included -- captured with `ImGui::SaveIniSettingsToMemory` and applied with
`LoadIniSettingsFromMemory`, so there is no second layout format to keep in step with the docking system. It sanitizes
names into files inside the preset folder (a separator, colon or other punctuation becomes `_`, leading and trailing
dots and spaces go, 64 characters at most, `..` refused), writes atomically through a temp file, and lists the four
built-ins first and saved layouts after them without repeats. The shell's Window > Layout menu offers the built-in
arrangements (Default, Level design, Scripting, Animation), builds each from `DockBuilder` in `BuildDockLayout`, lists
the saved layouts found in the project's `Saved/Layouts`, opens a Save Layout As dialog that captures the current
arrangement, and has Reset to Default, which always rebuilds the built-in and ignores any file of that name. A file
saved under a built-in name wins when that name is chosen ("Default (custom)" in the menu). The chosen name is
remembered with the open tool in `ShellLayout.layout`; the panel positions remain ImGui's own `editor_panels.ini`, so a
preset is a snapshot of it rather than a parallel layout system. View > Reset layout moved here as Window > Layout >
Reset to Default. Tests: the built-in names and menu order, saved layouts after them with a built-in-named file not
repeated and still loadable, a round-trip that leaves no temp file and makes the folder, empty and unusable names and
empty text refused with a reason, a missing or empty file reading as no preset, and the name sanitizing cases (path
separators, `../`, only dots or spaces, a 200-character name, a trailing dot). The shell layout file's round-trip test
now covers the remembered preset name, including files from before the field existed.

Done so far (2026-11-14): the Scene toolbar's recents list. `RecentScenes`
(`editor/src/core/recent_scenes.{h,cpp}`) keeps at most ten scene paths newest first, moving a path that is opened or
saved again to the front instead of repeating it, and persists them as `{"recent":[...]}` beside the project's other
saved state (`Saved/recent_scenes.json`, through the same file helpers as `RecentProjects`). The toolbar has a Recent
button after Scenes that lists those paths; one whose file is gone is marked "(missing)" and a "Remove missing scenes"
item prunes them and saves the pruned list. A choice goes through the same unsaved-changes prompt as the Scenes list, and
the toolbar records on open, on save and on recovering an autosave, so all three ways of naming a scene -- Recent, the
content-folder list, and the typed path or picker -- feed the same list. Tests: the list capped at ten with the newest
first, re-adding moving a path rather than duplicating it, removal, a save/load round-trip, and a damaged or missing file
reading as an empty list; and the panel recording saves and opens, deduping a path written absolutely, persisting across
two panels, and pruning a deleted scene (the persisted file is pruned too).

Done so far (2026-11-14): the status bar's source-control branch and selection count. `QueryGitBranch`
(`engine/src/assets/vcs.cpp`) asks git for the repository's branch with `symbolic-ref --short HEAD`, so a repository
with no commit yet still has a name, and falls back to the abbreviated commit id on a detached HEAD; outside a
repository it returns empty and says why instead of failing. The workspace caches the branch -- read when a project
opens and by `RefreshSourceControl()`, which the shell calls every 300 frames so a checkout outside the editor shows up
within seconds, rather than spawning git per frame -- and a host may set it directly (`SetSourceControlBranch`) for
another VCS or a test. `StatusBarInfo` gained `branch` and `selected` (the scene document's selection count; zero leaves
the segment out). Tests: the selection count following `SetSelection` and `ClearSelection`, the branch drawn and
cleared, a host-set branch surviving a refresh, and a real repository read through init, an unborn branch and a
renamed branch, with a quote in the path refused.

Done so far (2026-11-14): document tabs, the last Stage 1 item. `DocumentTabs`
(`editor/src/core/document_tabs.{h,cpp}`) keeps the open documents in tab order with one focused. A document is its own
identity, so opening it again focuses the tab it already has rather than adding a second copy; a dirty document closes
only when the caller discards it, and the last tab always stays open, since its document still exists and an empty strip
would leave the toolbar and quit prompt with nothing to mean. The workspace opens the scene as the first tab, and
opening another document (an asset editor's, from Stage 6) activates it and brings its view back -- the scene's is the
Scene tool. `DrawDocumentTabs` draws the strip (the active tab in the active-button color, a drawn dot rather than a
glyph when a document is dirty), middle-click and a right-click menu close a tab, and Close Others keeps the chosen one
or refuses as a whole while another document has unsaved changes. The close prompt is the same shape as the quit
prompt: Save and close (through the document's `Save`, or the Scene panel's naming defaults for the scene), Close
without saving, Cancel, with a failed save keeping the prompt up and saying why; its answers are public so a headless
host can make them. The strip is a viewport side bar under the main toolbar. Today the scene is the only real document,
so the production strip has one tab (which cannot be closed); the model and the prompt are exercised with a second document in tests. Tests: opening once and re-focusing, close-with-neighbor, dirty refusal and discard, the
last tab staying, Close Others both ways, the workspace's scene tab and its activation selecting the Scene tool, the
prompt's save/discard/cancel answers (including a failed save leaving the document open), and the strip and prompt
drawn headlessly.

Stage 1 is complete.

### Stage 2 — Viewport and selection parity (L)

Outcome: the Scene View can be the main 3D view for everyday work; the Live Viewport becomes the lighting check.

- Shaded mode in the Scene View by reusing the Live Viewport's offscreen renderer at editor resolution; Wireframe,
  Unlit and Normals as cheap alternatives. Show flags for grid, gizmos, bounds, helpers and collision.
- Orthographic top/front/side views with independent zoom, plus a perspective/ortho toggle; keep the orbit camera as
  the default.
- Fly camera (WASD + Q/E, right-drag look, speed modifier, speed shown briefly), focus by selection bounds (not origin),
  and camera bookmarks.
- Infinite fading grid with metre/decametre lines and axis labels; remove the ±20 m and 1,000 m framing limits.
- Selection: marquee (replace/add/subtract), Alt click-through, Ctrl+A, invert; keep Ctrl toggle.
- Transform: pivot mode (first selected origin vs selection centre), uniform-scale handle, plane handles for Scale,
  Euler rotation entry in the inspector, gizmo screen-constant size.
- Grouped undo: one command per user action for multi-delete, multi-duplicate, multi-paste and multi-transform.

Gate: camera/projection unit tests; marquee hit tests; multi-edit undo-count test; headless screenshots for each view
mode and ortho view; picking regression test at the 300k cap.

Dependency: shaded mode depends on the Live Viewport's offscreen path, which already exists. Material-graph preview
waits for the renderer work in Stage 6.

Done so far (2026-11-14): the perspective/orthographic toggle and the orthographic top, front and side views.
`SceneViewCamera` gained `projection`, `ortho_height` and `Look`/`Orientation` (`editor/src/world/scene_view.{h,cpp}`):
the orbit camera stays the default, Ortho keeps the current angle under a parallel projection, and Top, Front and Side set
the exact angles (looking down -Y, along -Z and along -X) and switch to orthographic. The orthographic zoom is its own
field, so the wheel changes `ortho_height` and leaves the perspective `distance` alone, and the other way round; Focus
sets both. The camera pulls its eye back (`EyeDistance`, at least 1000 units) so a parallel view's near plane never cuts
an object the perspective orbit distance would have, and it levels the view with -Z/+Z at the poles, where +Y would be
along the view direction. Panning and the gizmo's handle length use `ViewHeight()`, so both stay the same on screen in
either projection, and `ProjectToViewport` now ranks depths by the NDC z when the matrix's w is 1 (orthographic), which
keeps click-to-select's nearer-wins tie-break working there; it also rejects points before the near plane, which the
depth range's zero start makes visible as a negative z. The Scene View's toolbar has Persp/Ortho and Top/Front/Side
buttons, lit for the current view. Tests: a world unit projects to the same pixel length at the target and 100 units
beyond it, each projection keeps its zoom across switching, the top view puts +X right and -Z up, front and side keep +Y
up, and orbiting a preset makes it free again; the existing perspective, focus, ground-point and gizmo tests still pass.

Done so far (2026-11-14): the fly camera, focus by selection bounds and camera bookmarks. `SceneViewCamera` gained
`Move`, which translates eye and target together along the view's own forward, right and up, and `HandleCameraInput` runs
the fly keys only while the right mouse button is looking (`active`), so W/A/S/D and Q/E do not fight the gizmo's W/E/R
mode keys, which are suppressed during a look; Shift multiplies the speed four-fold. While flying the wheel is the speed
control (`fly_speed`, clamped and shown as "Speed N m/s" in the middle for a second and a half) and outside it the wheel
zooms as before. Focus (`FocusSelection`) now takes the model bounds cache: it frames every selected entity by its real
box placed by its world transform -- all eight corners, so scale and rotation count -- and falls back to the unit box
where no model is known, instead of framing origins; the F key passes the view's cache. Bookmarks are a whole camera pose
(`SceneViewCamera::State`: angles, target, perspective distance, orthographic height and projection) kept by the camera in
nine slots; the panel binds Ctrl+1..9 to store the current pose and 1..9 to return to one, with a brief message, and leaves
Alt combinations alone. Tests: W does nothing without looking and moves a metre a frame while looking (the eye travels
with the target), the wheel changes the speed while flying and the zoom otherwise, the move maths along all three view
axes, focus framing a scaled model's box centre rather than its origin (and the unit box without a cache), a whole-pose
save/recall round-trip with empty and out-of-range slots refused, and the panel's Ctrl+3/3 keys storing and recalling
through headless frames.

Done so far (2026-11-14): marquee selection, Alt click-through, Ctrl+A and invert, shared by the Scene View and the Live
Viewport. `PickEntities` (`editor/src/world/scene_view.cpp`) returns everything under the mouse, nearest first -- model
boxes the ray passes through (with the triangles refining them) and origins within the pick radius -- which is the stack
Alt-click steps through: the first Alt-click on an unselected stack takes the one behind the front, and each further one
moves back a place, wrapping round. `EntitiesInRectangle` is the marquee's hit test: every entity whose screen box (the
model's real box with the cache, the unit box otherwise, the origin for one partly behind the near plane) meets the
rectangle. `ViewportTools::HandleInput` turns a plain drag into a replace marquee, Ctrl+drag into an add and Alt+drag
into a subtract (Shift+drag is still the camera pan), draws the live rectangle in blue/green/red, and adds Ctrl+A
(select every entity with a Transform) and Ctrl+I (invert among them) while the View's mouse is over it; Alt presses no
longer grab a gizmo handle, so Alt selects through even on the pivot. The rectangle is drawn over both pictures.
Tests: a marquee replaces, a Ctrl marquee adds and an Alt marquee subtracts (each checked to leave an entity outside it
alone), Alt-click cycling a two-deep stack both ways and starting behind the front from an empty selection, select-all
and invert, and a marquee meeting a model's real box where only its origin would miss.

Done so far (2026-11-14): the endless fading ground grid, and the framing limits gone with it. The panel draws the grid around the
camera target instead of a fixed ±20 m square: its minor spacing is `GridSpacing(world_per_pixel)`, a power of ten that keeps a minor
line at least ten pixels from the next (a metre at normal zoom -- metres and decametres -- then tens and hundreds as the view backs
off), with every tenth line stronger, and each line drawn in six pieces whose alpha `GridFade` eases from full at half the radius to
nothing at the radius, so the ground has no visible edge in any direction. The X and Z axes stay coloured through the whole visible
ground and are labelled near the origin. The framing limits went with the old grid: `SceneViewCamera::Zoom` now backs the perspective
camera off to 20 000 m (orthographic height to 100 000 m) rather than 1 000, Focus clamps match, and the perspective far plane follows
the eye (`max(2000, EyeDistance() * 4)`) so a view backed off by kilometres still contains its scene. Tests: the spacing is a power of
ten that grows by tens when a metre would be 5 px and is safe for a zero or negative scale, the fade is full over the inner half and
exactly nothing at the radius and beyond while never brightening outwards, and the camera can now zoom past the old kilometre cap with
the target still projecting; the headless panel draw exercises the new grid every run.

Done so far (2026-11-14): grouped undo for multi-entity actions. `ScenePanel::DeleteSelection` and `::DuplicateSelection` count what
they are about to change and, when it is more than one entity, wrap the per-entity commands in one `CommandStack` transaction
("Delete 3 entities"), so one undo brings the whole action back; a single entity keeps the per-entity label. `SceneDocument::PasteClipboard`
does the same for a clipboard of several snapshots ("Paste 2 entities"), while pasting one stays "Paste <name>". Multi-transform was
already grouped by the gizmo (`Move 3 entities`), so all four actions now answer to one undo. Tests: pasting two entities is one undo
and the paste test says so, duplicate-then-delete of two entities undoes in two steps (one per action) rather than four, and a
three-entity gizmo drag lands all three and one undo puts every one of them back.

Done so far (2026-11-14): the transform upgrades -- pivot/centre, the uniform and plane scale handles, Euler rotation entry and a
gizmo of constant pixel size. `GizmoPivot` (`editor/src/world/scene_view.h`) chooses where the handles sit:
`TransformGizmo::Pivot` returns the first subject's world origin (Origin, the old behaviour and still the default) or the middle of
every subject (Centre); `ViewportTools` owns the choice, toggles it with P and the new "Pivot"/"Centre" toolbar button, and passes
it to hit-testing, dragging and drawing. With Centre, a turn swings every subject round the middle (the first included, instead of
staying put), a per-axis Scale drag also moves the others out from the middle along that axis, and the uniform box scales their
offsets too, so a group keeps its proportions. Scale gained the two handles the audit asked for: the three plane squares are drawn
and hit-tested in Scale as well as Move, and a drag inside one is measured along its diagonal and grows both of its axes; the box in
the middle (`GizmoAxis::Uniform`, first in the hit test within 8 px of the pivot) scales all three, with the ratio taken from the
mouse's travel from where the box was pressed (90 px of travel doubles it), so it works from the centre and feels the same as an
axis handle. `TransformGizmo::HandleLength` now takes the pivot and the viewport: it computes the world length that covers a fixed
90 px at the pivot's depth (`2 * depth * tan(fov/2) * pixels / height`, or `ortho_height * pixels / height`), so the gizmo no
longer shrinks as the selected entity moves away from where the camera aims. The inspector's reflection pass draws a top-level
Quaternion field as three Euler drags in degrees -- `EulerDegreesFromRotation`/`RotationFromEulerDegrees`
(`editor/src/ui/reflected_inspector.{h,cpp}`), turned about Y, then X, then Z, the order the editor's cameras already use, with a
gimbal-locked straight-up/down case folded into yaw -- and writes the quaternion back as the field's edited value, so the existing
command, dirty and prefab-override paths see an ordinary rotation change. Tests: the middle box doubles all three axes at 90 px of
travel and one undo restores them; a Scale square doubles X and Z and leaves Y; Origin and Centre pivots sit at the expected
points; a Centre move shifts a pair and a Centre quarter-turn puts them at (2, 0, ±2) round the middle; the handle projects to 90 px
at the target, grows tenfold in world units after a tenfold zoom, halves at half the depth, and is `height`/90 px in an orthographic
view; Euler round-trips (30, -20, 10), matches a camera's yaw-then-pitch rotation, and typing 45 into the rotation row's X entry
yields a 45-degree pitch quaternion.

Done so far (2026-11-14): the shaded Scene View and the show flags. The shell gives the Scene View a picture from the Live
Viewport's own renderer. `SceneViewShading` (`editor/src/world/scene_view_shading.h`) is the whole of what the portable panel knows --
`Advance(document, camera, width, height, mode, dt)`, a picture and a note -- and `LiveSceneShading`
(`editor/src/host/scene_shading.{h,cpp}`, Vulkan only) implements it on a `LiveViewport`; the shell makes one and hands it to the
workspace's new `SceneView()` accessor. The first shaded frame cooks the project's models and starts the open scene from its bytes, so
nothing has to be saved; after that an edit is patched into the running scene exactly as the Live Viewport patches, which is what makes
a gizmo drag show in the picture as it is made, and only a change that cannot be patched -- or another scene -- loads it again. The
picture has the view's aspect (the overlays share it), is drawn at most 1920 px on its long side, is rebuilt at a new size only once the
view has kept it for 0.3 s (a splitter drag does not rebuild it every frame), and runs at up to 30 fps with the world frozen, since the
Scene View shows the scene as authored. `SceneRenderer::ViewMode` (installed header) adds Unlit -- each surface's own base colour, no
light on it -- and Normals -- the world-space normal as a colour -- to Lit; the scene shader reads the mode from `g_Frame.counts.w` and
returns before the tone-map, leaving the lit path untouched. `LiveViewport::Frame` takes the projection and the orthographic height and
makes an "Editor Camera" for a scene with none (so Lit works on a cameraless scene, as the Scene View always has a camera of its own), the
orthographic eye is put far enough back that the whole scene is in front of it, and `SetViewMode` chooses the look. The toolbar gained a
Wireframe/Lit/Unlit/Normals combo and a Show popup (Grid, Gizmos, Bounds, Helpers, Collision); over a picture only the selected and hovered
entities are boxed so the lit scene shows through, and Wireframe -- or a build without Vulkan, which says so in the middle of the view --
keeps the old drawing. Collision draws every BoxCollider, SphereCollider and CapsuleCollider as a wireframe read through reflection (no
physics link, so the portable editor has it too; a trigger is yellow). Tests: Lit, Unlit, Normals and an orthographic view each draw a
different picture; a scene with every camera removed gains exactly one editor camera and draws; the panel asks the shading for the chosen
mode at the view's size and stops it in Wireframe; each show flag removes draw-list vertices and Collision adds them; and the bridge cooks
on first use, follows a moved object by patch without reloading the scene, changes with the mode, and stops. Not yet: a GPU wireframe (it
needs a line pipeline, which the Vulkan path has none of).

Done so far (2026-11-14): the Stage 2 gate's last two checks. `LiveViewport_HeadlessScreenshotsOfEachViewModeAndAnOrthographicView` renders
Lit, Unlit, Normals and an orthographic top view with no window, writes each as a PNG (to `AETHER_PARITY_OUT` when set, so the captures can be
kept, else the test project's `Saved/Screenshots`), checks each file is a real, non-flat PNG and that no two looks are the same picture, and
prints where they went -- a person can open the four files and see what the shader really draws. `ModelBounds_PickingAtTheTriangleCapStaysLinear`
writes two flat grid models, one of 297,192 triangles (just under `ModelBoundsCache::kMaxPickTriangles`) and one of 300,288 (just over), each
with a square hole a quarter of the way out, so the pick necessarily tells the two paths apart: the first is picked by triangles and the ray
through its hole misses, the second is picked by its box and the same ray hits. Ten picks through the solid grid are then timed against a
250 ms budget (2,500 ms under a sanitizer) -- the measured cost in a RelWithDebInfo build is 1.3 ms a pick alone and 2.1 ms with the gate
running four jobs, so the budget only trips on a change that makes the triangle loop superlinear.

### Stage 3 — Content browser and importer parity (L)

Outcome: assets can be found, judged, imported, fixed and opened without leaving the editor.

- Grid view with thumbnails using the existing `AssetThumbnail` API, cached on disk under `Saved/Thumbnails` and
  invalidated by source timestamp. Render thumbnails asynchronously so the browser does not stall.
- Multi-select and box-select; create asset, duplicate, delete, rename inline, move; operations are one undo step where
  the asset database supports undo, and otherwise one grouped transaction.
- Type filter chips, folder scope (current folder vs subtree), breadcrumbs, sortable columns, and a search syntax for
  type/path.
- Import settings drawer persisted in `.ameta`: per-importer options, reimport, and drag files from Explorer to import.
- An opener for every asset type that has an editor: material, blueprint, animation graph/clip, audio cue/mixer, UI,
  behavior tree, sequencer, VFX. A missing opener must say so instead of doing nothing.
- Reference viewer upgrades: referencers vs dependencies, find usages, and a broken/missing asset list.
- Reveal in Explorer and copy path in the context menu.

Gate: thumbnail cache test (second open does not re-render unchanged assets); reimport identity test; multi-select
operations and their undo count; headless open of every registered asset type; importer settings round-trip.

Done so far (2026-11-14): the grid of thumbnails. `ThumbnailStore` (`editor/src/content/thumbnail_store.{h,cpp}`) is the
whole of it: `Get(entry)` answers a picture or nothing, `Pump()` once a frame, and counters -- `Rendered`, `LoadedFromDisk`,
`WithoutPicture`, `Queued` -- so a test can say which path answered. Pictures are made on one worker thread that owns its
importer registry and derived data cache and a copy of the asset database, because `ImportAsset` registers sub-assets and
the panel is still reading the database it scanned. Each one is written to `Saved/Thumbnails/<guid>-<key>.png` (a temporary
file renamed into place; the older key for that asset removed). The key mixes the source file's size and modified time, the
`.ameta`'s import settings, the importer name and the dependencies' source hashes; the `.ameta`'s timestamp is not in it,
since every import -- including a cache hit -- rewrites the sidecar's source hash. The panel draws a 96 px tile per entry
with its picture fitted inside (or the importer's colour and the first letter for kinds without one -- models, scripts,
scenes, folders), the same git badge, drag payload, context menu and folder drop target the rows have, and Grid/List in the
toolbar switches between the two listings. Textures and materials are the pictured kinds; a material's picture follows its
base-colour texture's change. Tests: `ContentBrowserGrid_ThumbnailsRenderOnceAndComeFromDiskTheSecondTime` (the list asks
for no picture; the grid renders one per texture; a fresh panel on the same project decodes all of them from disk and
renders none; a changed source renders exactly one and does not accumulate files) and
`ContentBrowserGrid_KindsWithoutAPictureGetATypeTile` (models are counted as picture-less and write nothing).

Dependency: Stage 0's document contract and Stage 1's tabs, so opening an asset has somewhere to put it.

### Stage 4 — Inspector and hierarchy parity (M)

Outcome: editing many objects and finding a property are as fast as in the reference editors.

- Multi-object editing: show values shared by all selected objects, show a mixed state where they differ, apply an edit
  to all as one undo step. At minimum, transforms, names and the components all selected objects share.
- Property search across all components; reset property/component to default; copy/paste component values.
- Metadata for ranges, sliders, colour pickers and units, so reflected fields present well without per-type UI code.
- Hierarchy: visibility and lock toggles, type icons, prefab-instance state, multi-drag reparent, and a search that
  keeps ancestors visible and supports `type:` and `name:` tokens.
- Prefab overrides shown at the instance level, with revert per property (revert-to-prefab exists; it needs the
  granular view).

Gate: mixed-value semantics tests; edit-across-selection undo test; reset/copy/paste tests; hierarchy filter tests;
screenshot of a multi-selection inspector.

### Stage 5 — Play in editor and diagnostics parity (L)

Outcome: testing the game and finding the cause of a failure both happen in the editor.

- PIE in a rendered viewport: Play, Simulate, Pause, Step and Stop from the main toolbar; the Scene View or Live
  Viewport can host the picture (the offscreen path exists). Step is already implemented.
- PIE affordances in the playing viewport: border, pause tint and banner, so the state is visible without reading the
  Scene panel.
- Eject/possess camera, and a clear return to the editor camera and selection on Stop.
- PIE console tab with severity/category filters, collapse, clear-on-play and source links; runtime errors surface in
  the error overlay and Messages with an entity/asset target.
- Stop restores the pre-play document and camera; the "edits are discarded" state is shown before play, not after.
- Runtime stats overlay: frame time, draw calls, triangles, entities, physics bodies, script time.

Gate: N-step determinism test (the same state after N steps); stop-restores-document test; error-to-entity link test;
console filter tests; headless screenshot of PIE idle/paused/playing.

Dependency: Stage 2's view modes and Stage 0's notification/error service. A frame debugger and GPU capture are engine
work and are explicitly not part of this stage.

### Stage 6 — Asset documents, build and extensibility parity (L)

Outcome: every asset editor behaves the same, and shipping is a toolbar action.

- Put every asset editor on the Stage 0 document contract: save, save-as, dirty, undo, close prompt. Terrain and foliage
  gain save/load; localization gains undo; the Save Inspector states clearly that it is read-only where it is.
- Material graph preview in the viewport. This is renderer work (the player does not draw `.amat` graphs), not a UI
  task; keep it behind a capability check so the editor degrades honestly when the renderer cannot do it.
- Build/Package in the main toolbar with progress, cancel and an output log; run the packaged game from the same menu.
- Plugin manager: enable/disable, load errors, restart prompt; document the extension API beside it.
- Importer and project settings panels that persist to project files rather than only to sample documents.

Gate: open/save/undo tests for every asset document; package-from-toolbar test that verifies the manifest; plugin
enable/disable startup test.

### Stage 7 — Accessibility, localization and collaboration (M)

Outcome: the editor is usable beyond the default theme and keyboard layout, and changes are reviewable.

- UI scale from 75% to 200%, keyboard navigation for the main panels, focus visibility, and contrast checks.
- Colorblind-safe axis and pin colours, with the shape encoding already specified for Blueprint pins
  (`docs/design/EDITOR_UI.md:482-495`).
- Editor localization through the same string table system the games use; no literal UI strings.
- Source control: per-file stage/commit, diff for scene and asset documents, and a review-friendly change list. Real-time
  multi-user editing is out of scope.

Gate: screenshot matrix at 100/150/200%; keyboard-only workflow script; localization extraction check (no new literals);
scene diff test.

## How to tell a stage is done

A stage is done when its gate runs in `ctest` where it can be automated, the screenshot checks pass, and the matching
workflow can be completed in the editor by hand (editor tests are already part of `aether_tests`,
`tests/CMakeLists.txt:71-81`):

1. Launch, open a project, open a scene, place a model and a light, play, pause, step, stop, save, quit without losing
   work.
2. Find a broken asset from an error message, open it, fix it, reimport, and see the fix in the viewport.
3. Change a material parameter and see it in the rendered view without a reload.
4. Select ten objects, transform them together with snapping, undo once, and get all ten back.
5. Build, package and run the game from the editor.

These are the acceptance scripts; they should be written down as manual test steps and, where the host allows, driven
headlessly with `AETHER_EDITOR_MAX_FRAMES` and `AETHER_EDITOR_SCREENSHOT` (`editor/shell/main.cpp:139-141`). Until
they are timed, "parity" is a feature count, not a workflow claim.

## Sequencing rationale and risks

- **Why the shell before the viewport.** Every feature is discovered through the shell. A shaded viewport that cannot be
  reached from a project hub, cannot be saved, and loses work on quit does not shorten a task.
- **Why content before inspector multi-edit.** The content pipeline is where assets enter; multi-edit improves work that
  already has assets. Both are needed, but the pipeline blocks more tasks.
- **Why PIE after the viewport.** The renderer path and camera work are shared; building PIE first would duplicate them.
- **The main risk is scope drift into engine work.** Shaded view modes, material-graph preview, frame debugging and
  large-world streaming are renderer/engine features with an editor surface. Each stage above states which side owns
  the work; do not let an editor-stage estimate absorb an engine project.
- **The second risk is a second command system.** Extensions already register chords as strings
  (`editor/src/ui/extensions.cpp:225-261`). Stage 0 must move them onto the registry rather than adding a parallel
  shortcut path beside them.
- **The third risk is measuring the wrong machine.** The Live Viewport numbers above are from a CPU Vulkan driver. Do
  not tune the editor against them; record a real-GPU baseline before setting frame budgets.

## Not in this plan

- Replacing Dear ImGui. The toolkit decision is made; see [EDITOR_UI_TOOLKIT_ADR.md](EDITOR_UI_TOOLKIT_ADR.md).
- Multi-viewport floating panels across monitors. It is useful, but it is platform work that pays off only after the
  single-window workflow is complete.
- Real-time multi-user editing, an asset marketplace, and AAA content pipelines. These are separate products.
- Reimplementing engine systems inside the editor. Previews and PIE use the real renderer and the real runtime.
- Copying a reference editor's visual style. The target is the workflow: the same job in the same number of steps.
