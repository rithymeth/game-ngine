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
   discarding unsaved changes, overwriting a build). One user action is one
   undo step: deleting, duplicating or pasting several entities, or dragging
   a multi-selection with the gizmo, undoes as a whole.
4. **Show state, don't hide it.** Unsaved changes, compile errors,
   overridden prefab values and PIE mode are always visible.
5. **Keyboard reachable.** Every action has a name in the command
   palette; common actions have shortcuts (see
   a retired planning doc (removed)).
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
  other (uses ImGui multi-viewport). Not offered yet: the shell does not
  enable multi-viewport.

Users can save their own layouts. Layouts are stored per project in
`Saved/Layouts/*.ini` (the per-user config folder when no project is open).
**Reset Layout** always restores Default.

As implemented: the four built-ins are `DockBuilder` arrangements the shell
builds (`BuildDockLayout`); a saved layout is exactly the ini text ImGui
already writes for its own settings, captured with
`SaveIniSettingsToMemory` and applied with `LoadIniSettingsFromMemory`, so
the docking tree needs no second format. Window > Layout lists the built-ins
first (marked "(custom)" when a saved file of that name exists) and the saved
layouts after them, then Save Layout As... and Reset to Default. A file saved
under a built-in name wins when that name is chosen; Reset to Default ignores
it and rebuilds the built-in. The chosen name is remembered with the open
tool in `editor_layout.json`, and panel positions stay in `editor_panels.ini`
beside it.

### 4.3 Document tabs

- The open documents are tabs on their own strip under the main toolbar. The
  scene is the first; an asset editor's document joins when it is opened, and
  opening it again focuses its existing tab instead of opening a second copy.
  Today the scene is the only built document, so the strip has one tab; the
  other asset documents join the contract in Stage 6 of the parity audit.
- The active tab is drawn in the active-button color. A tab shows the
  document's display name, with a filled dot drawn beside it while it has
  unsaved changes.
- Middle-click closes a tab (and asks to save first when it is dirty); the
  right-click menu has Close and Close Others, which keeps the chosen tab and,
  while another document has unsaved changes, refuses rather than guessing. The
  last tab always stays open: its document still exists, and an empty strip
  would leave the toolbar and quit prompt with nothing to mean.
- The close prompt offers *Save and close*, *Close without saving* and *Cancel*;
  a save that fails leaves the prompt up with the reason.
- A tab reads its document through `core/editor_document.h`
  (`EditorDocument`: path, display name, kind, dirty state, undo history
  and save), not through the panel that draws it. Activating the scene's tab
  brings the Scene tool back. `SceneDocument` is the first adopter; the other
  asset documents follow in Stage 6 of the parity audit.

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
- **Rotations:** a quaternion field is presented as three Euler drags in
  degrees (X pitch, Y yaw, Z roll, turned about Y, then X, then Z -- the
  order the scene cameras use) and written back as the quaternion, so the
  file and every consumer still see a rotation.
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

One row under the menu bar, drawn by the workspace (not the shell), left to
right:

- Save, Undo, Redo. Each is the command table's entry: the enabled state and
  the chord in the tooltip come from the registry, and Undo's tooltip names
  what it would undo.
- The Scene View's transform tools: Move, Rotate, Scale, World/Local,
  Pivot/Centre, Snap with the step for the current mode. These are the same
  `ViewportTools` the Scene View drives, so W/E/R/Q/P and the buttons cannot
  disagree.
- Play controls. While editing there is one Play button. While playing it
  becomes Pause, Resume when paused, plus Step (enabled only while paused,
  and it advances exactly one frame) and Stop. The play state is also a thin
  amber underline across the toolbar, so it is visible without reading labels.
- Build and Package, and Settings (Preferences), at the right.

The current scene is not chosen here: the toolbar acts on the focused
document -- the active tab's -- which is the scene today and becomes whichever
asset editor is focused once those documents adopt the contract in Stage 6.

### 6.2 Viewport

- Overlays: see a retired planning doc (removed).
- **Selection outline:** 2 px `accent` outline around selected meshes;
  hovered objects get a 1 px `text.secondary` outline.
- **Grid:** endless fading grid on the XZ plane around the camera target,
  1 m minor / 10 m major lines at normal zoom; when a metre would be
  closer than 10 px the spacing steps up by tens (10/100 m, then 100/1000 m)
  so it stays readable, and each line fades to nothing at the grid's radius,
  so there is no edge. The X and Z axes are coloured and labelled near the
  origin.
- **Gizmo:** axis colors from tokens; the hovered handle turns yellow
  (`#FFD33D`). Move has the three arrows and the two-axis plane squares;
  Scale has arrows on the entity's own axes, the same squares growing two
  axes together, and a box in the middle that scales all three. The handles
  are a fixed 90 px long on screen whatever the zoom. The toolbar's
  Pivot/Centre choice (P) sits them on the first selected entity's origin or
  at the middle of the selection; with Centre a turn and a group scale work
  round that middle.
- **In PIE:** 2 px `pie.border` around the viewport (amber when paused);
  a small banner at the top: "Playing — Shift+F1 to release the mouse,
  Esc to stop".
- **Camera speed:** shows briefly in the middle ("Speed 4") when changed
  with the mouse wheel during fly mode.
- **Fly camera:** with the right mouse button held the view looks and
  W/A/S/D (Q/E down/up) moves; Shift runs; the wheel changes the speed
  instead of zooming. Focus (F) frames the selection by its models' boxes.
- **Camera bookmarks:** Ctrl+1..9 stores the current pose and 1..9 returns
  to it; a short message in the middle names the bookmark.
- **Projection:** the Scene View opens on the perspective orbit camera.
  Persp/Ortho toggles a parallel projection without changing the angle, and
  Top, Front and Side jump to those axis views (orthographic); the orthographic
  zoom is kept apart from the perspective distance, so switching back restores
  the other.
- **Selection:** click picks (Ctrl toggles); a plain drag draws a marquee that
  replaces the selection, Ctrl+drag adds what it touches and Alt+drag removes
  it (Shift+drag still pans). Alt+click steps through whatever is stacked under
  the mouse, front to back. Ctrl+A selects every entity with a Transform and
  Ctrl+I inverts that selection. The marquee is blue for replace, green for
  add and red for subtract.

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
- **Messages:** the editor's own warnings and errors that are not a log line, newest
  first, with severity, repetition count and age. Each can carry an entity, asset or
  file; clicking it selects the entity in the scene or opens the asset's folder in the
  Content Browser. *Clear* and *Mark all read* are in the header. This is the "fix
  these problems" list; Blueprint and asset checks post here as they gain them.

### 6.7 Status bar

Left to right: the open project (tooltip: its full path), the active
document's name with `*` while it has unsaved changes, how many entities are
selected (left out at zero), the play state ("Editing", or "Playing"/"Paused" in
amber), an active cook/package ("Cooking 42%") or "Game running", the
source-control branch (left out outside a repository; a detached HEAD shows the
commit id), and the unread message count as a button — clicking "Messages (2)"
opens the Messages tool and marks them read. The host appends the backend and
frame time on the same line. The branch is read from the project's repository
when it opens and by the shell every few seconds, not every frame, because it
asks git.

### 6.8 Project Hub

The start screen when the editor is launched with no project, and File >
Project Hub at any time. The open project's path sits at the top, then a path
field with Browse beside it where the platform has a picker ("pick or type" is
one row, not two ways to do the same thing). The recent list is newest first:
clicking a row opens it, a row whose file is gone is marked Missing and
cannot be opened, its Remove button drops just that row, and "Remove missing
projects" drops all of them and saves. The New Project template list is
embedded below it rather than in a second window. Opening or creating a project
closes the hub; a failed open leaves it open, says why in its status line, and
posts the same error to Messages.

### 6.9 Help

*Help > Keyboard Shortcuts* (F1) opens a sheet of every chord the editor has,
grouped by menu (File, Edit, then the rest, then "Other" for toolbar and
palette actions that have no menu) and with a Where column — Anywhere, Scene
tool or Scene hierarchy — because a Scene chord does nothing until that tool has
focus. The rows are generated from the command table (`CommandRegistry::Shortcuts`),
not written by hand, so a chord that is removed disappears from the
sheet and a new one appears without a second edit; a footer points at the
command palette for actions that have no chord.

*Help > About Aether Editor* shows the engine version, whether this is a release
or an assertions-on build, the tool count and the open project, with a button to
the shortcut sheet.

### 6.10 Menus

The bar is File, Edit, Asset, Actor, Tools, View, Window and Help. Every item comes from
the command table (except the shell's own Window > Layout entries and the Tools menu's tool
list), so a label, a chord and the action are written once.

- **File**: New, Open, Save, Save As, Open Project, Project Hub, Quit.
- **Edit**: Undo, Redo, Duplicate, Delete, Copy, Paste, Rename.
- **Asset**: Content Browser (focuses it), Refresh Assets, Validate Assets. The
  browser is where assets are created, moved and committed, so the menu opens it
  rather than repeating those actions.
- **Actor**: Create Empty Actor, Create Camera, Create Cine Camera — the same
  kinds as the hierarchy's Create menu, selected and undoable.
- **Tools**: every tool by category, checked while its window is open.
- **View**: Command Palette, Preferences.
- **Window**: Layout (Default, Level design, Scripting, Animation, then the
  saved layouts, Save Layout As..., Reset to Default; §4.2).
- **Help**: Keyboard Shortcuts (F1), About.

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
- Every menu item, the command palette and the keyboard read one table,
  `editor/src/workspace/command_registry.{h,cpp}`, so a label, a chord and
  the action behind it exist once. A chord declares a scope (`Global`,
  `Scene`, `SceneTree`); the workspace polls `Global` and the Scene panel
  polls its two, which is what keeps Ctrl+S in a focused graph editor from
  meaning "save the scene" as well. Modifiers match exactly, and a startup
  test fails on a duplicate id or on two chords that overlap. The same table
  generates the F1 shortcut sheet (§6.9).
- Tab/Shift+Tab move between fields in the Inspector; Enter commits;
  Esc reverts a field being edited.

### 7.4 Editing and saving

- Scenes and assets save explicitly (Ctrl+S). The open scene is also **autosaved** while it has
  unsaved changes, every 5 minutes by default (*View > Preferences*: on or off, 30 s to 1 h), into a
  per-project recovery slot under the user's config folder (`%APPDATA%/Aether/Autosave`). An
  autosave never writes the scene file, and saving the scene or undoing back to the saved state
  removes it.
- Closing the editor (File > Quit, or the title-bar X) with an unsaved scene asks: *Save and quit*,
  *Quit without saving*, *Cancel*. A normal quit removes the recovery slot; only a session that ends
  without one leaves it behind.
- If the editor ends without a normal quit (a crash, a kill), the next launch on the same project
  offers to recover the autosave: *Recover the autosave* (the scene comes back dirty, under its own
  name, and the file on disk is untouched until it is saved), *Discard it*, or *Not now*.
- Target, not yet built: one dialog listing every unsaved asset with checkboxes. Today the prompt
  covers the scene document; the other asset editors keep their own documents and do not ask.
- The quit prompt asks the active document through the `EditorDocument` contract (kind, name,
  `IsDirty()`), not the Scene panel directly. A panel may front `Save()` with its own naming
  defaults and overwrite check -- the Scene toolbar does -- but the document is what changes.
- Opening and saving a scene can go through the system's own file picker: *File > Open Scene* and
  *File > Save Scene As* use it, and the Scene toolbar has a "..." button for Open. A picked file
  under the content folder is shown there by its scene name; one from elsewhere keeps its absolute
  path. The picker is `core/file_dialog.{h,cpp}` (`IFileDialog` on Windows, zenity or kdialog on
  Linux); where the platform has none, the toolbar's typed path is still the way, so the commands
  fall back to it instead of doing nothing. The toolbar's own *Save As* stays typed, for renaming
  without a dialog.
- The Scene toolbar also keeps a *Recent* list: the last ten scenes opened, saved or recovered, newest
  first, stored per project in `Saved/recent_scenes.json`. Opening or saving a scene that is already
  listed moves it to the front rather than adding a duplicate, and a path written absolutely is shown
  by its scene name when it is under the content folder. A listed file that no longer exists is marked
  "(missing)", with a *Remove missing scenes* item to prune the list; choosing an entry goes through
  the same unsaved-changes prompt as the content-folder list. The typed path and the picker therefore
  remain one way in, not the only one.

---

## 8. Feedback: states, notifications, progress and errors

### 8.1 Toasts

- Bottom-right, stacked, up to 4 visible. Info and success disappear after
  4 s; warnings after 8 s; errors stay until dismissed. Each has an X.
  Clicking a toast opens Messages, where the links are.
- Built: `core/notifications.{h,cpp}` keeps the history and expiry, and
  `ui/notification_overlay.{h,cpp}` draws the toasts and the Messages list. The
  editor posts save, autosave and recovery results and failures. A warning that
  repeats back to back is one row with a count.
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
