# Aether Editor: UI design specification

This is the design reference for every editor panel. It expands the design
system summary in [ROADMAP.md Phase 7](../ROADMAP.md) into exact values,
component specs and behavior rules, so panels built by different people
look and behave the same.

Contents:

1. Design principles
2. Design tokens (color, type, spacing, radius, motion)
3. Iconography
4. Layout and docking
5. Component library
6. Panel specifications
7. Interaction rules (selection, drag and drop, focus, editing)
8. Feedback: states, notifications, progress, errors
9. Accessibility requirements
10. Plan for splitting `editor/main.cpp`

---

## 1. Design principles

1. **The viewport is the hero.** Panels frame the scene; they never cover
   it by default. The existing default layout already avoids overlap; keep
   that as a rule.
2. **One way to do a thing, everywhere.** Asset fields, color pickers,
   search bars and confirmations use the same widget in every panel.
3. **Everything is undoable, so confirmations are rare.** Only ask for
   confirmation when an action can't be undone (deleting files on disk,
   discarding unsaved changes, overwriting a build).
4. **Show state, don't hide it.** Unsaved changes, compile errors,
   overridden prefab values and PIE mode are always visible.
5. **Keyboard reachable.** Every action has a name in the command
   palette; common actions have shortcuts (see
   [ROADMAP_DETAILS.md §F](../ROADMAP_DETAILS.md)).
6. **Beginners first, experts not slowed down.** Sensible defaults, empty
   states that explain what to do, and tooltips on every control. Experts
   get shortcuts, the command palette, and dense mode.

---

## 2. Design tokens

All colors, sizes and timings live in `editor/src/ui/Theme.h` as named
tokens. Panels reference tokens (`theme.color.accent`), never literal
values. `ApplyFriendlyEditorStyle()` in today's `editor/main.cpp` becomes
`Theme::Apply(ThemeId)`.

### 2.1 Color: dark theme (default)

| Token | Hex | Use |
|---|---|---|
| `bg.app` | `#16171A` | behind docked panels, empty dock space |
| `bg.panel` | `#1E1F22` | panel body |
| `bg.panel.alt` | `#232428` | alternating table rows, inset areas |
| `bg.header` | `#2B2D31` | panel title bars, table headers, node bodies |
| `bg.input` | `#111214` | text fields, sliders, dropdowns |
| `bg.hover` | `#2F3136` | hovered rows and buttons |
| `bg.active` | `#35373C` | pressed buttons |
| `border.subtle` | `#2E3035` | separators |
| `border.strong` | `#43464D` | input outlines, focused panel edge |
| `text.primary` | `#E6E7EA` | main text |
| `text.secondary` | `#A6A9B0` | labels, hints |
| `text.disabled` | `#62666E` | disabled controls |
| `accent` | `#4C8DFF` | primary buttons, selection, focus ring |
| `accent.hover` | `#6BA1FF` | |
| `accent.muted` | `#1F3A66` | selection background in lists |
| `success` | `#3FB950` | compile OK, saved |
| `warning` | `#D29922` | warnings |
| `error` | `#F85149` | errors, destructive buttons |
| `info` | `#58A6FF` | info toasts |
| `pie.border` | `#3FB950` | 2 px viewport border while playing |
| `pie.paused` | `#D29922` | border while PIE is paused |
| `prefab` | `#5AA9E6` | prefab icons and override bars |
| `axis.x / y / z` | `#E5484D` / `#46A758` / `#3E63DD` | gizmos, vector field labels |

### 2.2 Color: light theme

| Token | Hex |
|---|---|
| `bg.app` | `#E9EAEE` |
| `bg.panel` | `#F6F7F9` |
| `bg.panel.alt` | `#EEF0F3` |
| `bg.header` | `#E1E4E9` |
| `bg.input` | `#FFFFFF` |
| `bg.hover` | `#E4E7EC` |
| `border.subtle` | `#D5D9E0` |
| `border.strong` | `#B5BBC6` |
| `text.primary` | `#1C1E22` |
| `text.secondary` | `#5A606B` |
| `accent` | `#2F6FEB` |
| `accent.muted` | `#D6E4FF` |
| `success` / `warning` / `error` | `#1A7F37` / `#9A6700` / `#CF222E` |

Every text/background pair must meet **WCAG AA contrast (4.5:1)** in both
themes. A unit test checks the token table.

### 2.3 Typography

| Token | Font | Size | Use |
|---|---|---|---|
| `font.ui` | Inter | 15 px | default UI text |
| `font.ui.small` | Inter | 13 px | tooltips, status bar, table meta |
| `font.ui.bold` | Inter SemiBold | 15 px | section headers, selected tabs |
| `font.title` | Inter SemiBold | 18 px | dialog titles, welcome screen |
| `font.mono` | JetBrains Mono | 14 px | console, code editor, numeric readouts |
| `font.icon` | Lucide (merged into atlas) | 16 px | icons inline with text |

- All sizes scale by `ui_scale` (user setting, 75–200%) × monitor DPI
  factor. The font atlas rebuilds when the scale changes.
- Numbers in tables use tabular figures so columns line up.

### 2.4 Spacing, size and radius

| Token | Value | Use |
|---|---|---|
| `space.1` | 4 px | icon-to-label gap |
| `space.2` | 8 px | panel padding, default gap |
| `space.3` | 12 px | between groups |
| `space.4` | 16 px | dialog padding |
| `row.height` | 26 px (22 px in dense mode) | list rows, property rows |
| `toolbar.height` | 36 px | main toolbar |
| `input.height` | 24 px | text fields, dropdowns |
| `radius.small` | 3 px | inputs, buttons |
| `radius.medium` | 6 px | cards, thumbnails, nodes |
| `radius.large` | 10 px | dialogs, toasts |
| `label.column` | 38% of panel width (min 90 px, max 220 px) | property label column |

### 2.5 Motion

| Token | Value | Use |
|---|---|---|
| `motion.fast` | 80 ms ease-out | hover, press |
| `motion.normal` | 150 ms ease-out | expand/collapse, toast in |
| `motion.slow` | 250 ms ease-in-out | panel slide, camera focus (F) |

When the user turns on **reduced motion**, all animations become instant
and the exec-wire animation in Blueprint debugging becomes a static
highlight instead.

---

## 3. Iconography

- One icon set only (Lucide, MIT license), 16 px grid, 1.5 px stroke.
- Every asset type has a fixed icon **and** a fixed color bar in the
  Content Browser:

| Asset type | Icon | Color bar |
|---|---|---|
| Scene | map | `#8B949E` |
| Prefab | box | `#5AA9E6` |
| Blueprint | workflow | `#4C8DFF` |
| Mesh | shapes | `#3FB950` |
| Material / Material Instance | palette | `#3FB950` / `#7EE787` |
| Texture | image | `#E5484D` |
| Animation clip / graph | footprints / git-fork | `#D29922` |
| Sound | volume-2 | `#BC8CFF` |
| Widget (UI) | layout | `#F778BA` |
| Script (Luau) | file-code | `#79C0FF` |
| Input Action / Mapping | gamepad-2 | `#FFA657` |
| Data Table / Struct / Enum | table / braces / list | `#8B949E` |

- Entity icons in the Hierarchy come from the entity's "most important"
  component (camera, light, mesh, audio, and so on), falling back to a
  plain dot.

---

## 4. Layout and docking

### 4.1 Default layout

The default layout proportions (at 1920×1080):

| Region | Size | Panels (tabs) |
|---|---|---|
| Top | 24 px menu bar + 36 px toolbar | — |
| Left | 18% width | Hierarchy \| Place Actors |
| Center | remaining | Viewport (tabs: Viewport 1, plus opened asset editors) |
| Right | 22% width | Inspector \| World Settings |
| Bottom | 26% height | Content Browser \| Console \| Output \| Messages |
| Status bar | 22 px | — |

### 4.2 Layout presets (Window > Layout)

- **Default**, as above.
- **Level design:** larger viewport, Content Browser as a drawer
  (Ctrl+Space) instead of docked.
- **Scripting:** Blueprint/code editor in the center, viewport small at
  top-right, Output large.
- **Animation:** asset viewer center, timeline bottom, Details right.
- **Two monitors:** viewport maximized on one monitor, all panels on the
  other (uses ImGui multi-viewport).

Users can save their own layouts. Layouts are stored per project in
`Saved/Layouts/*.ini`. **Reset Layout** always restores Default.

### 4.3 Asset editor tabs

- Opening an asset editor (Blueprint, Material, and so on) adds a tab next
  to the viewport. Each asset editor has its **own dock space** inside the
  tab, with its own panel layout.
- A tab title shows the asset name, with a `●` when it has unsaved
  changes. Middle-click closes the tab (and asks to save if needed).
- Reopening an asset focuses its existing tab instead of opening a
  second copy.

---

## 5. Component library (`editor/src/ui/`)

Each widget is one function with consistent parameters. Signatures are
shown for the main ones.

### 5.1 Property rows

```cpp
// Two-column row: label (with tooltip) on the left, widget on the right.
// Returns true when the value changed this frame; begins/ends an undo
// transaction automatically (drag start → end = one undo step).
bool ui::Property(const char* label, f32& value, const PropertyOptions& opt = {});
bool ui::Property(const char* label, Vec3& value, const PropertyOptions& opt = {});
bool ui::Property(const char* label, bool& value, const PropertyOptions& opt = {});
bool ui::PropertyEnum(const char* label, const reflect::EnumInfo&, i64& value);
bool ui::PropertyColor(const char* label, Vec4& rgba, bool hdr = false);
bool ui::PropertyAsset(const char* label, AssetRef& ref, AssetTypeId filter);
```

Behavior rules for every property row:

- **Label:** `text.secondary`, truncated with ellipsis, full name in the
  tooltip together with the field's `Meta::tooltip`.
- **Numbers:** drag horizontally to scrub (Shift = 10× faster, Alt = 10×
  slower), double-click to type. Typing accepts math expressions
  (`2*pi`, `+=5`). Units show greyed out after the number (`1.5 m`).
- **Vectors:** three fields labeled X/Y/Z with the axis colors as a 2 px
  left bar; a lock icon for uniform scale.
- **Reset:** a small ↺ icon appears on hover when the value differs from
  its default; clicking it resets to default.
- **Prefab override:** a 3 px `prefab`-colored bar at the left edge and a
  bold label. Right-click shows *Revert to Prefab* and *Apply to Prefab*.
- **Multi-selection with different values:** shows `—` instead of a
  number. Editing sets all of them.
- **Read-only:** `text.disabled`, not editable, but selectable and
  copyable.

### 5.2 Asset field

```
┌──────────────────────────────────────────────┐
│ [thumb]  T_Brick              [🔍] [↗] [✕]    │
└──────────────────────────────────────────────┘
```

- Accepts drag and drop from the Content Browser. The field highlights
  with an accent outline if the dragged asset type fits, and a red outline
  if it doesn't.
- 🔍 opens a searchable picker popup filtered to the allowed type, with
  thumbnails. ↗ shows the asset in the Content Browser. ✕ clears it.

### 5.3 Buttons

| Variant | Look | Use |
|---|---|---|
| Primary | `accent` fill, white text | the one main action in a dialog (Create, Save, Package) |
| Secondary | `bg.header` fill | other actions |
| Ghost | no fill until hover | toolbar icon buttons |
| Destructive | `error` text (fill on hover) | Delete, Discard |
| Toggle | Ghost with `accent.muted` fill when on | snapping, view options |

Icon-only buttons always have a tooltip with name and shortcut
("Translate (W)").

### 5.4 Other common widgets

- `ui::SearchBar(buffer, hint)`: magnifier icon, clear button, Ctrl+F
  focuses it. Fuzzy matching, highlighting the matched characters.
- `ui::Toolbar` / `ui::ToolbarButton` / `ui::ToolbarSeparator`.
- `ui::TreeRow`: used by Hierarchy, Content Browser folders, and bone
  lists; supports rename-in-place (F2), drag handles, and an eye (visibility)
  and lock toggle on the right.
- `ui::Dialog` / `ui::ConfirmDialog(title, message, confirm_label, danger)`:
  modal, Enter confirms, Esc cancels, the destructive button is never the
  default. This replaces today's `Confirm Delete` popup.
- `ui::Toast(kind, message, action = {})`: see §8.
- `ui::EmptyState(icon, title, body, button)`: see §8.
- `ui::Curve(curve&)`: inline mini curve with a click to open the full
  curve editor.
- `ui::Gradient(gradient&)`: color stops bar.
- `ui::KeyCapture(binding&)`: "Press a key…" for input and shortcut
  rebinding.

---

## 6. Panel specifications

### 6.1 Main toolbar

Left to right: Save, Undo, Redo | Select/Translate/Rotate/Scale, space
(Local/World), snap toggles with values | **Play controls** centered
(Play ▾ with options: Selected Viewport / New Window / Simulate, and
number of clients once networking exists; Pause; Step; Stop) | Build ▾,
Package ▾, Settings on the right.

- While playing, Play is replaced by Stop, and the whole toolbar gets a
  thin `pie.border` underline.

### 6.2 Viewport

- Overlays: see [ROADMAP_DETAILS.md §E.7](../ROADMAP_DETAILS.md).
- **Selection outline:** 2 px `accent` outline around selected meshes;
  hovered objects get a 1 px `text.secondary` outline.
- **Grid:** infinite fading grid on the XZ plane, 1 m minor / 10 m major
  lines, fading out with distance.
- **Gizmo:** axis colors from tokens; the hovered axis turns yellow
  (`#FFD33D`); plane handles for two-axis moves; a center handle for
  screen-space moves.
- **In PIE:** 2 px `pie.border` around the viewport (amber when paused);
  a small banner at the top: "Playing — Shift+F1 to release the mouse,
  Esc to stop".
- **Camera speed:** shows briefly in the middle ("Speed 4") when changed
  with the mouse wheel during fly mode.

### 6.3 Hierarchy

- Columns: name (with icon), and eye/lock toggles on the right.
- Label the tree with the active scene name and entity count. Search entity
  names in place with Ctrl+F (keeps matching parents visible and expanded,
  shows the match count, and preserves selection when it still matches;
  filtering out the selected entity clears Inspector selection rather than
  silently retargeting edits).
- Drag rows to reparent (a line indicator shows above, below, or "into").
  Alt-drag duplicates.
- Right-click: Create Empty Child, Rename, Duplicate, Delete, Create
  Prefab, Select Children, Focus, Copy/Paste.
- Prefab instances: `prefab`-colored name; an arrow on the right opens the
  prefab.
- Inactive (disabled) entities: `text.disabled`.

### 6.4 Inspector

- Header: entity icon, editable name, enabled checkbox, tag dropdown,
  layer dropdown.
- One collapsible **card per component**, header shows icon + name +
  a ⋮ menu (Reset, Remove, Move Up/Down, Copy/Paste values, Open Script).
- Fields come from reflection (Phase 6), grouped by `Meta::category`.
- **Add Component** button at the bottom, full width, opens a searchable
  list grouped by category, with recently used at the top.
- **Lock** icon in the header keeps the Inspector showing the current
  entity while you select others.

### 6.5 Content Browser

Covered in [ROADMAP.md Phase 8](../ROADMAP.md). Additional rules:

- Thumbnail size slider (64–256 px); list view for large folders.
- Filter current-folder entries by supported asset type and name; search and
  type filters combine. Label the folder scope, show the visible/matching
  entry count when nothing is selected, focus search with Ctrl+F, and explain
  how to clear a no-match result.
- New assets are created with a name field focused for renaming right
  away.
- Assets with import errors show a red ⚠ corner badge; hovering shows the
  error.
- Unsaved assets show a `●` on the thumbnail.

### 6.6 Console / Output / Messages

- **Console:** engine log with severity filters, search, timestamps
  toggle, "Clear on Play" toggle, and a command input line at the bottom
  (CVars and commands, with auto-complete).
- **Output:** build and compile output (C++ builds, shader compiles, cooks)
  with clickable file:line links.
- **Messages:** grouped, deduplicated warnings and errors from Blueprints,
  assets and map checks. Each links to the node, asset or entity. This is
  the "fix these problems" list.

### 6.7 Status bar

Left: current task or "Ready", plus a progress bar for background work
(imports, shader compiles). Middle: selection count. Right: source control
branch/state, autosave time, frame time. Clicking the frame time opens the
Profiler.

---

## 7. Interaction rules

### 7.1 Selection

- Click selects; Ctrl+click toggles; Shift+click selects a range (in
  lists); drag a box in the viewport to box-select (Ctrl adds, Alt
  removes).
- Selection is **shared** between Viewport, Hierarchy and Inspector, and is
  part of undo history (undoing a delete restores the selection too).
- Esc clears selection (outside PIE).

### 7.2 Drag and drop

| Drag from | Drop onto | Result |
|---|---|---|
| Content Browser asset | Viewport | spawn at the surface under the cursor (ghost preview first) |
| Content Browser asset | Asset field | assign |
| Content Browser asset | Hierarchy row | spawn as child of that entity |
| Content Browser asset | Blueprint graph | a node that references it (Spawn Prefab, Play Sound, and so on) |
| Hierarchy entity | Content Browser | create a prefab from it |
| Hierarchy entity | Hierarchy entity | reparent |
| Hierarchy entity | Entity field / Blueprint graph | reference / Get node |
| OS file explorer | Content Browser | import |

The drop target always shows a preview (outline plus a label saying what
will happen) before release. Invalid targets show a "no" cursor.

### 7.3 Focus and keyboard

- The panel under the mouse gets scroll input; the **focused** panel
  (last clicked, shown with a 1 px `accent` top edge) gets keyboard input.
- Shortcuts are context-sensitive: W/E/R go to the viewport only when the
  viewport or Hierarchy is focused, never while typing in a text field.
- Tab/Shift+Tab move between fields in the Inspector; Enter commits;
  Esc reverts a field being edited.

### 7.4 Editing and saving

- Scenes and assets save explicitly (Ctrl+S) with **autosave** every 5
  minutes to `Saved/Autosaves/` (never overwriting the real file).
- Closing the editor or a tab with unsaved changes shows one dialog
  listing every unsaved asset with checkboxes: *Save Selected*, *Don't
  Save*, *Cancel*.
- If the editor crashes, the next start offers to restore from the latest
  autosave.

---

## 8. Feedback: states, notifications, progress and errors

### 8.1 Toasts

- Bottom-right, stacked, up to 4 visible. Info and success disappear after
  4 s; warnings after 8 s; errors stay until dismissed.
- A toast can carry one action ("Show in Messages", "Undo").
- Examples: "Saved Level_01" (success), "BP_Door compiled with
  1 warning" (warning, action: Show), "Import failed: T_Rock.png is not a
  valid PNG" (error).

### 8.2 Empty states

Every panel that can be empty explains what to do:

| Panel | Empty state text | Button |
|---|---|---|
| Hierarchy | "This scene is empty. Drag assets from the Content Browser, or create an entity." | + Create |
| Inspector | "Select something in the viewport or Hierarchy to see its properties." | — |
| Content Browser with no project | "Create a project or open one to browse assets, place models, and edit scenes." | Open Project… · Create Project… |
| Content Browser folder | "This folder is empty. Drag files here to import them." | Import… |
| Blueprint event graph | "Right-click or press Tab to add your first node. Try Event BeginPlay." | Add BeginPlay |
| Console | "No messages yet." | — |

### 8.3 Progress

- Short tasks (under 1 s): no indicator.
- Background tasks: status-bar progress with the task name, clickable for a
  task list with Cancel buttons.
- Blocking tasks (should be rare, for example a first-time shader cache
  build): modal with progress, an estimate, and Cancel where possible.

### 8.4 Errors

- Errors appear **where the problem is**: a red node header in a
  Blueprint, a red badge on an asset thumbnail, a red underline in the
  code editor. They also appear in Messages, linked back.
- Error text says what happened, why, and what to do: "Can't compile
  BP_Door: the Branch node's Condition pin needs a bool, but it's connected
  to a float. Connect a comparison node, or use a Float to Bool
  conversion."

---

## 9. Accessibility requirements (editor)

- Every control is reachable and operable by keyboard.
- UI scale 75–200%; all layouts must still work at 200% on 1920×1080.
- Colors are never the only signal: Blueprint pin types also have shapes
  (circle = value, triangle = exec, grid = array, diamond = set, a
  two-color circle = map), and errors also have an icon.
- Colorblind-safe palette check for axis colors and pin colors (the test
  simulates protanopia/deuteranopia/tritanopia and checks distinguishability).
- Screen reader names for toolbar buttons and dialogs (Phase 35).

---

## 10. Plan for splitting `editor/main.cpp`

Today's editor is one ~2,000-line file. This maps its current contents to
the new layout from [ROADMAP.md Phase 7](../ROADMAP.md). Move one area per
PR, and run the headless screenshot check
(`AETHER_EDITOR_MAX_FRAMES` + `AETHER_EDITOR_SCREENSHOT`) before and after
each move to confirm nothing changed.

| Current code in `editor/main.cpp` | Moves to |
|---|---|
| `ApplyFriendlyEditorStyle()` | `editor/src/ui/Theme.{h,cpp}` (as `Theme::Apply`) |
| `ModelRenderer`, `SetModelPath`, `Parent`, `ComputeWorldTransform`, `WorldPosition`, `WouldCreateCycle` | runtime components: `engine/include/aether/scene/{components,hierarchy}.h` (they're game-relevant, not editor-only) |
| `ForEachWithEntity` | `engine/include/aether/ecs/world.h` (a general ECS helper) |
| `EditorCamera` | `editor/src/tools/EditorCamera.{h,cpp}` |
| `InvertMatrix4x4`, `InverseGeneral` | `engine/include/aether/math/mat4.h` (general math) |
| `Ray`, `ScreenPointToRay`, `RaySphereIntersect`, `ScreenPos`, `WorldToScreen`, `PointSegmentDistance2D`, `ClosestPointOnAxisToRay` | `engine/include/aether/math/geometry.h` (math) plus `editor/src/tools/Picking.cpp` |
| Instanced quad shader, `CreateRootSignature`, `CreatePSO` | `editor/src/render/DebugShapesRenderer.cpp` (until Phase 14 replaces it) |
| glTF shader, `GltfFrameConstants`, `CreateGltfRootSignature`, `GltfRenderData` | `editor/src/render/ModelRenderer.cpp` (until Phase 14) |
| Gizmo shader, `GizmoVertex`, `CreateGizmoRootSignature` | `editor/src/tools/Gizmo.cpp` |
| `ListAvailableGltfModels` | `editor/src/panels/ContentBrowserPanel.cpp` (until Phase 8's AssetDatabase) |
| `SpawnSphere` | `editor/src/core/EditorActions.cpp` (becomes an undoable command in Phase 7) |
| `SaveBackbufferScreenshot` | `editor/src/app/Screenshot.cpp` (shared with the demos later) |
| Main menu bar (View, Help menus) | `editor/src/app/MainMenu.cpp` |
| "Aether Editor" window (spawn, play/pause, save/load) | `editor/src/panels/ToolbarPanel.cpp` + `editor/src/core/PlayMode.cpp` |
| "Confirm Delete" popup | `editor/src/ui/Dialogs.cpp` (`ui::ConfirmDialog`) |
| "Asset Browser" window | `editor/src/panels/ContentBrowserPanel.cpp` |
| "Hierarchy" window | `editor/src/panels/HierarchyPanel.cpp` |
| "Inspector" window | `editor/src/panels/InspectorPanel.cpp` (made generic in Phase 6) |
| "Help" window | `editor/src/panels/HelpPanel.cpp` |
| `main()` frame loop, device/swap chain setup | `editor/src/app/EditorApp.cpp` + a tiny `editor/main.cpp` |

Suggested PR order: math and geometry helpers → runtime components →
Theme → each panel → render helpers → `EditorApp`. After the last step,
`editor/main.cpp` is about 20 lines.
