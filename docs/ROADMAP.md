# Aether Roadmap: from rendering tech demo to full game engine

> Companion documents (index in [`docs/README.md`](README.md)):
> - [`ROADMAP_DETAILS.md`](ROADMAP_DETAILS.md): file formats, the Phase 6
>   build spec, Blueprint VM opcodes, more mockups, shortcuts, estimates, risks
> - [`design/PHASE_SPECS.md`](design/PHASE_SPECS.md): build specs for
>   Phases 7, 8, 9, 11 and 13
> - [`design/EDITOR_UI.md`](design/EDITOR_UI.md): full editor UI design spec
> - [`design/UPGRADE_PLAN.md`](design/UPGRADE_PLAN.md): the PR-sized plan for M7, the Aether Upgrade (Phases 37-48)
> - [`design/BLUEPRINT_NODES.md`](design/BLUEPRINT_NODES.md): Blueprint node reference
> - [`tutorials/FIRST_GAME.md`](tutorials/FIRST_GAME.md): the target
>   "first game" walkthrough and M2 acceptance test

This document is the plan for turning Aether from what it is now (a strong
rendering, ECS and physics core with a single-file ImGui editor) into a
complete, Unreal/Unity/Godot-class engine. That means an editor people can
build a whole game in, **visual scripting (Blueprints)**, text scripting, and
a packaged standalone player.

The phases are ordered by **dependency**, not by how exciting they are. Each
phase lists:

- **Why:** what it unlocks.
- **Engine / runtime work:** new modules, types, and function signatures.
- **Editor UI:** panels, layouts, and interactions.
- **Done when:** acceptance criteria, in this repo's existing style: verified
  by actually running it, with screenshots or tests, not by inspection.

Phase numbers continue from the README (Phases 1–5 plus follow-ups are
done).

---

## 0. Where we are today (baseline)

| Area | State | Notes / gaps that block a "full engine" |
|---|---|---|
| Memory, logging, math, filesystem | ✅ Solid | — |
| Job system (work-stealing) | ✅ Solid | No task graph / automatic dependency inference yet |
| ECS (archetype, chunked SoA) | ✅ Solid | `kMaxComponentTypes = 64` hard cap; component names come from `typeid(T).name()` (compiler-specific, which breaks cross-compiler scene files); **no reflection** of fields |
| RHI (D3D12 + Vulkan) | ✅ Unified pipeline/draw API, vertex/index buffers, bindless textures, depth, blend | Editor and sandbox still use the D3D12-only `gfx::` layer |
| Renderer | ✅ PBR, normal maps, IBL (split-sum), shadow maps (2 lights), GPU culling, render graph with aliasing | All of this lives in **separate demos** (`pbr_demo`, `gltf_demo`, `sandbox`), not in one reusable renderer the editor/game uses |
| Assets | ✅ glTF 2.0 (meshes, hierarchy, skinning, animation), PNG textures, cached `AssetManager` | No GUIDs, no `.meta` files, no import settings, no hot reload, no cooked format |
| Physics | ✅ Jolt with job-system adapter, sphere bodies | Only spheres; no colliders, triggers, character controller, queries, or events |
| Scene | ✅ Binary save/load by component name | No prefabs, no scene references, no versioning/migration |
| Editor | ✅ Fly camera, picking, translate gizmo, hierarchy, inspector, asset browser, help | One 2,000-line `editor/main.cpp`; no docking, **no undo/redo**, no play-in-editor snapshot, no rotate/scale gizmo, no project concept |
| Scripting | ❌ None | Needed for games |
| Visual scripting | ❌ None | Main goal of this roadmap |
| Audio, input mapping, runtime UI, AI, VFX, networking, packaging | ❌ None | — |
| Platforms | Windows only (Win32 window) | Linux/macOS need a window layer and a Vulkan-hosted editor |

**The main architectural gap:** nothing in the engine can describe its own
types (no reflection). The inspector, serialization, undo/redo, scripting
bindings, Blueprints, and networking all need that same information, so
**reflection comes first (Phase 6)**.

---

## 1. Target architecture

```
                         ┌──────────────────────────────────────────────┐
                         │                 Aether Editor                │
                         │  Docking UI · Undo/Redo · Content Browser ·  │
                         │  Blueprint Graph · Material Graph · Anim     │
                         │  Graph · UI Designer · Profiler · PIE        │
                         └───────────────┬──────────────────────────────┘
                                         │ uses (never the other way)
┌────────────────────────────────────────▼────────────────────────────────────────┐
│                               Aether Runtime (engine/)                          │
│                                                                                 │
│  Gameplay Framework ── Scene/Prefab ── Script VM (Blueprint bytecode + Luau)    │
│         │                   │                     │                             │
│  ┌──────┴──────┬────────────┴───┬─────────────────┴───┬───────────┬───────────┐ │
│  │  Renderer   │  Physics       │  Animation          │  Audio    │  UI (RT)  │ │
│  │ (RenderGraph│  (Jolt)        │  (state machines,   │ (miniaudio│  widgets, │ │
│  │  + RHI)     │                │   blend, IK)        │  mixer)   │  layout   │ │
│  └──────┬──────┴────────────────┴─────────────────────┴───────────┴───────────┘ │
│         │                                                                       │
│  Core: Reflection · ECS · Jobs/Task Graph · Assets (GUID DB) · Input · Events   │
│  Platform: Window (Win32 / X11-Wayland / Cocoa) · FS · Threads · Time           │
│  RHI: D3D12 · Vulkan (· Metal later)                                            │
└─────────────────────────────────────────────────────────────────────────────────┘
                                         │ cook + package
                                         ▼
                              Standalone Player (.exe)
```

Rules we keep:

1. **The runtime never depends on the editor.** Editor-only code lives under
   `editor/`, or behind `AETHER_EDITOR` where it has to touch runtime types
   (for example, property metadata used only for display).
2. **Everything is an ECS component or an asset.** No hidden side tables.
3. **Reflection is the single source of truth** for inspector, serialization,
   undo, scripting, Blueprints, and replication.
4. **Every feature ships with a test or a headless verification mode** (the
   `AETHER_EDITOR_MAX_FRAMES` / `AETHER_EDITOR_SCREENSHOT` pattern).

Proposed new top-level layout (additions shown):

```
engine/     core runtime (existing) + reflection/, input/, events/, script/
renderer/   NEW: the reusable renderer extracted from pbr_demo/gltf_demo/sandbox
physics/    existing, extended
animation/  NEW
audio/      NEW
ui/         NEW: runtime (in-game) UI
ai/         NEW
editor/     split into editor/src/{panels,graph,tools,...}
player/     NEW: standalone game runtime executable
tools/      aether-cook, aether-reflect (header parser/codegen), packaging scripts
templates/  NEW: starter projects (FPS, third-person, 2D platformer, top-down)
```

---

## 2. Milestones at a glance

| Milestone | Phases | What you can do at the end |
|---|---|---|
| **M1: Editable** | 6 → 9 | Open a project, build a level with prefabs, undo anything, save and reopen it |
| **M2: Scriptable** | 10 → 13 | Make gameplay with Blueprints or Luau, press Play, debug with breakpoints |
| **M3: Good-looking and alive** | 14 → 19 | Full PBR renderer in the editor, materials, animation, audio, game UI, particles |
| **M4: Complete games** | 20 → 23 | AI, world building, multiplayer, profiling tools |
| **M5: Shippable** | 24 → 26 | Cook and package a standalone game for Windows/Linux; templates; docs |
| **M6: Advanced** | 27 → 36 | Cinematics, save games, localization, abilities/RPG kits, ray tracing, XR, mobile/web, modding, accessibility, optional AI assistants |

---

# M1: Editable

## Phase 6: Reflection & Property System

**Why:** the inspector, serializer, undo, Blueprints, scripting, and
networking all need to enumerate a type's fields, functions, and events at
runtime.

### Engine / runtime work

New module `engine/include/aether/reflection/`.

```cpp
// Declaring a reflected component (macro-based; no external codegen needed
// to start, an optional clang-based `aether-reflect` tool can generate these later)
struct Health {
    f32 current = 100.0f;
    f32 max     = 100.0f;
    bool invulnerable = false;
};
AETHER_REFLECT(Health,
    AETHER_FIELD(current, Meta::Range(0, 1000), Meta::Tooltip("Current HP")),
    AETHER_FIELD(max,     Meta::Range(1, 1000)),
    AETHER_FIELD(invulnerable)
)
AETHER_FUNCTION(Health, ApplyDamage, Meta::BlueprintCallable)   // void ApplyDamage(f32)
AETHER_EVENT(Health, OnDied)                                     // multicast event
```

Core types:

| Type | Purpose |
|---|---|
| `TypeId` | Stable 64-bit hash of the **declared name** (replaces `typeid(T).name()` as the on-disk key, so files work across compilers) |
| `TypeInfo` | name, size, alignment, kind (primitive/struct/enum/component/asset-ref/array/map), fields, functions, events, base type, metadata |
| `FieldInfo` | name, `TypeId`, offset, flags (`EditAnywhere`, `ReadOnly`, `Transient`, `Replicated`, `BlueprintReadWrite`), metadata (range, tooltip, category, units) |
| `FunctionInfo` | name, param list, return type, a type-erased `invoke(void* self, Span<Any> args, Any* ret)` thunk, flags (`BlueprintCallable`, `Pure`, `Latent`, `Static`) |
| `EventInfo` | name, signature; backing `MulticastDelegate<Args...>` |
| `EnumInfo` | name to value table, `Flags` support |
| `Any` / `Variant` | small-buffer typed value box, used by scripting and the Blueprint VM |
| `TypeRegistry` | `Find(name)`, `Find(TypeId)`, `ForEachType`, `ForEachDerived(base)` |

Functions to add:

```cpp
const TypeInfo* Reflect<T>();
const TypeInfo* TypeRegistry::Find(std::string_view name);
Any  FieldInfo::Get(const void* obj) const;
void FieldInfo::Set(void* obj, const Any& v) const;
bool FunctionInfo::Invoke(void* self, std::span<Any> args, Any* ret) const;

// Generic, versioned serialization built on reflection (replaces raw memcpy default)
void SerializeObject(const TypeInfo&, const void* obj, ArchiveWriter&);
void DeserializeObject(const TypeInfo&, void* obj, ArchiveReader&); // tolerant of added/removed fields
```

- **Text archive (JSON)** for scenes, prefabs, and assets while developing
  (diffable and mergeable in git), plus a **binary archive** for cooked
  builds. Keep the current binary path as the binary archive's first
  version.
- **Schema versioning:** each type carries `version`; register
  `Migrate(from, to, archive)` hooks.
- Lift `kMaxComponentTypes = 64` to 256 (or a dynamic bitset) because
  gameplay projects exceed 64 quickly.
- Port existing components: `Transform`, `RigidBody`, `Parent`,
  `ModelRenderer`, `EditorCamera`.

### Editor UI

- The Inspector becomes **fully generic**: it draws any reflected component
  from `FieldInfo` + metadata (sliders for `Range`, color pickers for
  `Meta::Color`, asset pickers for `AssetRef<T>`, dropdowns for enums,
  foldable categories). The hand-written per-component inspector code in
  `editor/main.cpp` goes away.
- "Add Component" button lists every registered component type, with search.

### Done when

- Tests: round-trip every primitive, enum, nested struct, array, and asset
  ref through JSON and binary; add a field, reload an old file, and the value
  defaults correctly; `FunctionInfo::Invoke` calls a real method.
- Editor: add a brand-new reflected component in game code and it appears
  and edits in the Inspector with zero editor code changes.

---

## Phase 7: Editor Foundation (architecture, docking, undo/redo, Play-in-Editor)

**Why:** every later tool (Blueprint graph, material graph, and so on) is a
panel. The editor needs a real framework before more panels are added to
`editor/main.cpp`.

### Engine / editor work

1. **Split `editor/main.cpp`** into:
   ```
   editor/src/app/EditorApp.cpp        main loop, window, layout persistence
   editor/src/core/EditorContext.h      selection, active world, project, command stack
   editor/src/core/Commands.h           undo/redo
   editor/src/panels/{Viewport,Hierarchy,Inspector,ContentBrowser,Console,Toolbar,...}.cpp
   editor/src/tools/{Gizmo,Picking,Snapping}.cpp
   editor/src/graph/                     reusable node-graph widget (Phase 12)
   ```
   Each panel implements
   `class IPanel { virtual const char* Name(); virtual void OnGui(EditorContext&); }`.
2. **Switch ImGui to the `docking` branch** (same version line). Enable
   multi-viewport so panels can be torn out into OS windows.
3. **Undo/redo command stack**:
   ```cpp
   struct ICommand { virtual void Do(World&)=0; virtual void Undo(World&)=0;
                     virtual bool MergeWith(const ICommand&) { return false; } // drag coalescing
                     virtual const char* Label() const = 0; };
   class CommandStack { void Execute(std::unique_ptr<ICommand>); void Undo(); void Redo();
                        void BeginTransaction(const char*); void EndTransaction(); };
   ```
   Generic commands built on reflection: `SetFieldCommand` (stores old/new
   `Any`), `CreateEntityCommand`, `DestroyEntityCommand` (snapshots
   components), `AddComponentCommand`, `RemoveComponentCommand`,
   `ReparentCommand`. Gizmo drags coalesce into one undo step.
4. **Stable entity handles** (`EntityGuid`, 128-bit) on top of `Entity` so
   undo, prefabs, and references survive destroy/recreate.
5. **Play-in-Editor (PIE)**: Play duplicates the edit `World` (serialize to
   memory, then deserialize into a play world), runs systems and scripts on
   the copy, and Stop throws it away. That makes play non-destructive, which
   is what Unity and Unreal users expect. Also add Pause, Step one frame,
   **Simulate** (physics only, no scripts), and "Keep simulation changes"
   (copy selected entities back).
6. **Transform gizmo v2**: translate, rotate, and scale (W/E/R),
   local/world toggle, grid and angle snapping, multi-selection pivot. Look
   at ImGuizmo before writing a custom one.
7. **Multi-select** (Ctrl/Shift-click, box select in viewport), copy/paste,
   duplicate (Ctrl+D), focus (F).
8. **Project system**: `MyGame.aproject` (JSON: name, engine version,
   startup scene, module list, input settings). File > New Project / Open
   Project / recent projects list. Assets are resolved relative to the
   project root instead of the repo's `assets/`.
9. **Editor settings and preferences**: theme, keybindings, and camera speed
   saved per user; layout saved per project (`imgui.ini` goes into
   `ProjectRoot/Saved/`).

### Editor UI: main layout (default dock arrangement)

```
┌─────────────────────────────────────────────────────────────────────────────────────┐
│ File  Edit  View  Project  Build  Tools  Window  Help               [project name]  │
├─────────────────────────────────────────────────────────────────────────────────────┤
│ [💾][↶][↷] │ [W][E][R] Local▾  Snap 0.5▾ 15°▾ │   ▶ Play  ⏸  ⏭  ■  │  Simulate  │ ⚙   │
├──────────────┬──────────────────────────────────────────────────┬───────────────────┤
│ HIERARCHY    │  VIEWPORT                     [Lit▾][Persp▾][⚙]  │ INSPECTOR         │
│ 🔍 search    │                                                  │ ▣ Player  [✓]     │
│ ▾ Level_01   │                                                  │ Tag: Player ▾     │
│   ▾ Player   │                 (3D scene, gizmo)                │ ▾ Transform       │
│     Camera   │                                                  │   Pos  0  1  0    │
│     Mesh     │                                                  │   Rot  0  0  0    │
│   ▸ Enemies  │                                                  │   Scl  1  1  1    │
│   Sun        │                                                  │ ▾ Health          │
│   Floor      │                                                  │   Current ━━●━ 80 │
│              │   fps 144 · 3.2k tris · 12 draws                 │ ▾ BP_Player (script)│
│ + Create ▾   │                                                  │ [ + Add Component]│
├──────────────┴──────────────────────────────┬───────────────────┴───────────────────┤
│ CONTENT BROWSER                             │ CONSOLE / OUTPUT / MESSAGES           │
│ ◂ ▸ Content / Characters /   🔍 filter ▾    │ [Info][Warn][Err]  🔍                 │
│ 📁 Maps  📁 Blueprints  📁 Materials         │ [12:01:02] Compiled BP_Player OK      │
│ [thumb][thumb][thumb][thumb][thumb]         │ [12:01:05] ⚠ Missing texture ...      │
└─────────────────────────────────────────────┴───────────────────────────────────────┘
│ status: Ready · 3 selected · autosaved 12:00                          ⏱ 6.9 ms      │
└─────────────────────────────────────────────────────────────────────────────────────┘
```

### UI design system (applies to every panel from here on)

- **Theme:** dark by default (`#1E1F22` background, `#2B2D31` panels,
  accent `#4C8DFF`), with a light theme too. Colors are defined once as
  tokens in `editor/src/ui/Theme.h`; panels don't hard-code colors.
- **Font:** Inter 15 px for UI, JetBrains Mono 14 px for code and console;
  icon font (Font Awesome or Lucide) merged into the atlas. High-DPI aware
  (scale fonts by monitor DPI).
- **Spacing scale:** 4 / 8 / 12 / 16 px. Panel padding 8, item spacing 6.
- **Consistent widgets:** `ui::PropertyRow(label, widget)` two-column
  layout; `ui::AssetField<T>` with drag-and-drop target plus a picker
  button; `ui::SearchBar`; `ui::Toolbar`; `ui::ConfirmDialog` (reuse the
  existing delete confirmation).
- **Keyboard first:** command palette (Ctrl+P: every editor action is a
  registered `EditorAction` with a name and default shortcut, rebindable).
- **Feedback:** toast notifications (bottom-right) for saves/compiles;
  unsaved asterisk on tabs; a progress bar in the status bar for imports
  and builds.

### Done when

- Every inspector edit, gizmo drag, create, delete, and reparent can be
  undone and redone, with a test driving 1,000 random commands and checking
  that undo-all restores a byte-identical scene.
- Play → move things → Stop restores the scene exactly (checked by
  serializing both and comparing).
- Layout persists across restarts; panels dock, undock, and tear out.

---

## Phase 8: Asset System v2 (asset database, import pipeline, hot reload)

**Why:** Blueprints, materials, prefabs, and animations are all assets that
reference each other. File-path references break as soon as someone renames
a folder.

### Engine / runtime work

- **`AssetGuid`** (128-bit) for every asset, stored in a sidecar
  `Foo.png.ameta` (JSON: guid, importer, import settings, dependencies).
- **`AssetDatabase`** (editor): scans `Content/`, maintains
  guid ↔ path ↔ type, tracks dependency graph, and handles renames/moves
  (updates the path while the GUID stays the same, so references don't
  break).
- **`AssetRef<T>`** / **`SoftAssetRef<T>`**: reflected handle types; hard
  refs load with their owner, soft refs load on demand (async).
- **Importers** (`IAssetImporter` with `CanImport(ext)` and
  `Import(src, settings) → ImportedAsset`): texture (PNG/JPG/TGA/HDR/EXR →
  BC1–BC7/BC6H compressed with mips), glTF/GLB (split into Mesh, Material,
  Skeleton, AnimationClip sub-assets), FBX (optional later, via ufbx),
  audio (WAV/OGG/MP3), fonts (TTF → SDF atlas).
- **Derived data cache** (`Intermediate/DDC/`): imported/processed output
  keyed by source hash + settings hash + importer version.
- **Async loading:** `AssetManager::LoadAsync<T>(guid) → AssetHandle<T>` on
  the job system, with ref counting and unload.
- **Hot reload:** a file watcher (ReadDirectoryChangesW / inotify)
  re-imports changed sources, bumps the asset's generation, and live
  handles pick up the new data (textures, meshes, materials, shaders,
  scripts).
- **Shader hot reload** falls out of this: `.hlsl` files are assets too.

### Editor UI: Content Browser

```
┌ CONTENT BROWSER ──────────────────────────────────────────────────────────────┐
│ [+ Add ▾] [⤓ Import] [💾 Save All]   ◂ ▸  Content › Characters › Hero        │
│ ┌─────────────┐ ┌───────────────────────────────────────────────────────────┐ │
│ │ ▾ Content   │ │ 🔍 search     Type: All ▾   Sort: Name ▾   ▦ ☰  size ━●━   │ │
│ │   ▸ Maps    │ │ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐               │ │
│ │   ▾ Chars   │ │ │ 🧍   │ │ 🎨   │ │ 🔷   │ │ 🏃   │ │ 🖼   │               │ │
│ │     Hero    │ │ │Hero  │ │M_Skin│ │BP_Hero│ │Run   │ │T_Skin│              │ │
│ │   ▸ Props   │ │ │Mesh  │ │Mat   │ │BP    │ │Anim  │ │Tex   │               │ │
│ │ ★ Favorites │ │ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘               │ │
│ └─────────────┘ └───────────────────────────────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────────────────┘
```

- Rendered thumbnails for meshes, materials, and prefabs (offscreen render
  pass through the renderer, cached in DDC); a color bar under each tile
  shows the asset type.
- Drag an asset into the viewport to spawn it; drag onto an Inspector
  `AssetField` to assign it.
- Right-click: Rename, Duplicate, Delete (with reference check: "Used by
  3 assets"), Show in Explorer, Reimport, Reference Viewer, Size Map.
- **Asset editors** open in their own dockable tabs by type: Texture viewer
  (channels, mips, compression settings), Mesh viewer (LODs, collision,
  sockets), Material editor (Phase 15), Blueprint editor (Phase 12), and so
  on.

### Done when

- Renaming or moving an asset folder keeps every scene and prefab reference
  working (test).
- Editing a PNG in an external program updates the viewport within
  about 1 second, without restarting.
- A 500-asset project reopens with no reimport (DDC hit).

---

## Phase 9: Scenes, Prefabs, and the Gameplay Framework

**Why:** Unity's prefabs, Unreal's Actors/Blueprint classes, and Godot's
scenes-as-nodes all solve the same problem: reusable, nested, overridable
object templates. Blueprints in Phase 12 attach to these.

### Engine / runtime work

- **Prefab asset** (`.aprefab`): a serialized entity subtree with local
  GUIDs.
  - `PrefabInstance` component: `{ AssetRef<Prefab>, overrides: list<PropertyOverride> }`,
    where an override is `{ entity local id, component TypeId, field path, value }`.
  - Nested prefabs; **prefab variants** (a prefab based on another prefab).
  - Apply overrides to the source prefab, or revert them; changes to the
    source propagate to all instances.
- **Scene asset** (`.ascene`) with the Phase 6 JSON archive; multiple
  scenes loaded additively (for streaming later); a `SceneManager` with
  `LoadScene(guid, mode)` and `UnloadScene`.
- **Gameplay framework** (thin, ECS-friendly versions of Unreal concepts):

  | Concept | Aether form |
  |---|---|
  | `GameMode` / game rules | `GameMode` asset + singleton component, chosen per scene |
  | `PlayerController` | `PlayerController` component; owns input context and possessed pawn |
  | `Pawn` / `Character` | Prefab with `CharacterMovement` + `Controller` target |
  | `Camera` | `Camera` component (perspective/ortho, FOV, near/far, priority) and `SpringArm` |
  | `Tag` / layers | `Tags` component (fixed string hashes) + 32 named layers in project settings |
  | Lifecycle events | `OnCreate`, `OnStart`, `OnUpdate(dt)`, `OnFixedUpdate(dt)`, `OnLateUpdate`, `OnDestroy`, `OnEnable`, `OnDisable` |
  | Timers | `TimerManager::SetTimer(entity, seconds, looping, callback)` |
  | Global events | `EventBus::Publish<T>(evt)` / `Subscribe<T>(handler)` |

- **System scheduling:** a `SystemGraph` where systems declare component
  read/write sets, and the scheduler runs non-conflicting systems in
  parallel on the job system. This is the "task graph" the README says is
  still to be built. Fixed-timestep loop for physics and gameplay (60 Hz
  default, configurable) with render interpolation.

### Editor UI

- Prefab edit mode: double-click a prefab and it opens isolated in the
  viewport, with a breadcrumb bar `Level_01 › Enemy (Prefab)`.
- Overridden fields are shown **bold with a blue left bar** in the
  Inspector; right-click a field for Revert / Apply to Prefab.
- Hierarchy shows prefab instances with a cube icon (blue for a prefab,
  lighter blue for a variant); a small "+" overlay marks added children.
- "Create Prefab" by dragging an entity from the Hierarchy into the Content
  Browser.

### Done when

- Change a field in a prefab and all 100 instances in a scene update, while
  instances that override that field keep their value (test).
- Nested prefab 3 levels deep round-trips save/load.

**🎯 M1 complete:** a designer can build and save a real level with no code.

---

# M2: Scriptable

## Phase 10: Input System

**Why:** scripts and Blueprints need `OnJump`, not `if (GetAsyncKeyState('W'))`.

### Engine / runtime work

- Raw device layer: keyboard, mouse (including raw delta and cursor lock),
  gamepad (XInput on Windows, SDL gamecontroller DB elsewhere), touch
  (later).
- **Action mapping** (Unreal Enhanced Input / Godot InputMap style):
  - `InputAction` asset: `{ name, value type: Bool | Axis1D | Axis2D | Axis3D }`
  - `InputMappingContext` asset: bindings `action ← key/button/axis` with
    **modifiers** (dead zone, negate, swizzle, scale) and **triggers**
    (pressed, released, hold ≥ t, tap, chord, double-tap).
  - Context stack with priorities (for example, "OnFoot" over "Vehicle",
    and "UI" blocks gameplay).
- API: `Input::GetAction(name)`, `Input::GetAxis2D(name)`, and events
  `OnActionStarted/Triggered/Completed` exposed to reflection, and therefore
  to Blueprints and scripts.
- Runtime rebinding with save to user config.

### Editor UI

- Input Mapping Context editor: a table of actions × bindings, a "Press a
  key…" capture button, and modifier/trigger stacks as small cards.
- Project Settings > Input shows the default contexts.

### Done when

- A test injects synthetic device events and checks action states,
  including hold and chord triggers.

---

## Phase 11: Scripting Runtime (text scripting + native gameplay modules)

**Why:** Blueprints are for designers, but programmers want text code. This
phase also builds the **VM infrastructure and binding layer that Blueprints
compile into** in Phase 12.

### Decision: two scripting paths

| Path | Choice | Why |
|---|---|---|
| **Native C++ gameplay modules** | Game code builds as a DLL (`MyGame.dll`) loaded by editor and player; **hot reload** by serializing live component state, unloading, reloading, and restoring | Full speed and full engine access; the same model as Unreal C++ |
| **Text scripting** | **Luau** (Roblox's typed, sandboxed, fast Lua; MIT license) | Tiny, embeddable, gradual types, great debugger story, no GC pauses of note, easy to bind via reflection. (C# via .NET hosting is a larger alternative, left for later.) |

### Engine / runtime work

- `engine/include/aether/script/`:
  - `ScriptComponent { AssetRef<Script> script; PropertyBag exposed_vars; }`
  - `ScriptHost` interface: `Compile`, `CreateInstance(entity)`,
    `CallEvent(instance, "OnUpdate", args)`, `HotReload(asset)`.
  - `LuauHost`: **auto-binds every reflected type/function/event** (Phase 6),
    so there's no hand-written glue. Example script:

    ```lua
    --!strict
    local Player = {}
    Player.speed = 6.0            -- exposed in Inspector automatically

    function Player:OnStart()
        self.health = self.entity:Get(Health)
        self.health.OnDied:Connect(function() self:Respawn() end)
    end

    function Player:OnUpdate(dt: number)
        local move = Input.GetAxis2D("Move")
        self.entity:Get(CharacterMovement):AddInput(Vec3.new(move.x, 0, move.y) * self.speed)
        if Input.WasPressed("Jump") then self.entity:Get(CharacterMovement):Jump() end
    end

    return Player
    ```
  - Sandbox: no raw `io`/`os`; a per-instance execution time budget in debug
    builds.
- **Native module hot reload:** `AETHER_GAME_MODULE(MyGame)` entry point
  registers its reflected types; the editor watches the DLL, and on rebuild
  snapshots all instances of that module's components via reflection, swaps
  the DLL, and restores them.
- **Debugging:** Luau debugger via Debug Adapter Protocol (DAP) so VS Code
  can set breakpoints; in-editor error overlay with a clickable stack trace.

### Editor UI

- Script asset opens in an embedded **code editor panel** (ImGuiColorTextEdit)
  with syntax highlighting, error squiggles from the compiler, and
  auto-complete fed by reflection. Also offer "Open in external editor
  (VS Code)".
- The Inspector shows a script's exposed variables like normal fields.
- A Build menu item compiles the game module (invokes CMake) and shows
  output in the Output panel with clickable errors.

### Done when

- A sample project where a Luau script moves the player with input, a
  second script listens to `Health.OnDied`, and editing the script while in
  PIE applies the change without stopping.
- Recompiling the C++ game DLL while the editor runs keeps component values.

---

## Phase 12: ⭐ Aether Blueprints (visual scripting)

**Why:** the headline feature. Designers create gameplay logic as node
graphs, like Unreal Blueprints, Unity Visual Scripting, or Godot's (former)
VisualScript and its community successors.

### 12.1 Concepts

| Concept | Description |
|---|---|
| **Blueprint Class** (`.abp`) | An asset that defines a new *entity template*: a component list (like a prefab), variables, functions, macros, event graphs. It can **derive from** another Blueprint or a native C++ "base" (for example, `Character`). Placing one in a scene creates an entity with a `BlueprintInstance` component. |
| **Blueprint Function Library** | Static functions usable from any graph |
| **Blueprint Interface** | A set of function signatures that any Blueprint can implement (`Interactable::Interact(instigator)`); calling it on an entity that doesn't implement it does nothing and doesn't error |
| **Level Script** | One graph per scene for level-specific events (triggers, cutscenes) |
| **Macro** | Reusable node subgraph inlined at compile time, which can have multiple exec outputs |
| **Struct / Enum assets** | User-defined data types usable in graphs and exposed to reflection |
| **Data Table** (later) | CSV-like rows of a struct type, editable in a table editor |

### 12.2 Graph model (data)

```cpp
enum class PinKind { Exec, Data };
enum class PinDir  { In, Out };

struct PinType {
    PinKind kind;
    TypeId  type;            // float, int, bool, Vec3, Entity, AssetRef<T>, struct...
    Container container;     // None | Array | Set | Map
    bool    by_ref = false;
};

struct Pin      { PinId id; std::string name; PinDir dir; PinType type; Any default_value; };
struct Node     { NodeId id; NodeTypeId type; Vec2 pos; std::vector<Pin> pins; PropertyBag config; std::string comment; };
struct Link     { PinId from; PinId to; };
struct Graph    { GraphId id; std::string name; GraphKind kind; // EventGraph | Function | Macro
                  std::vector<Node> nodes; std::vector<Link> links; std::vector<Variable> locals; };
struct Blueprint{ AssetGuid parent_class; std::vector<ComponentTemplate> components;
                  std::vector<Variable> variables; std::vector<Graph> graphs;
                  std::vector<AssetGuid> interfaces; };
```

Serialized as JSON (diffable). Node positions are stored, but the compiled
bytecode is a derived artifact kept in the DDC.

### 12.3 Node library

Nodes are **generated from reflection** (Phase 6): every
`BlueprintCallable` function becomes a node, every `BlueprintReadWrite`
field becomes Get/Set nodes, and every event becomes an event node.
Hand-written nodes cover control flow.

| Category | Nodes |
|---|---|
| **Events** | Event BeginPlay, Event Tick(dt), Event EndPlay, OnActionTriggered(InputAction), OnCollisionBegin/End, OnTriggerEnter/Exit, OnDamaged, Custom Event (with params), Interface Event |
| **Flow control** | Branch, Sequence, Switch on Int/String/Enum, For Loop, For Each (Array), While, Do Once, Do N, Gate, Flip-Flop, MultiGate, Select |
| **Latent (async)** | Delay, Retriggerable Delay, Move Component To, Timeline, AI MoveTo, Async Load Asset, Wait for Event |
| **Variables** | Get/Set (self, other entity), Make/Break struct, local variables, Promote to Variable |
| **Math** | + − × ÷ %, comparisons, clamp, lerp, abs/min/max, random ranges; vector ops (dot, cross, normalize, length), rotator ops, **math expression node** (`a*b + sin(c)` typed as text) |
| **Entity/World** | Spawn Prefab/Blueprint, Destroy, Get/Add/Remove Component, Find Entities with Tag, Get Transform/Set Transform, Attach/Detach, Get Player Pawn |
| **Physics** | Line/Sphere Trace (single/multi), Add Force/Impulse, Set Velocity, Overlap query |
| **Casting & types** | Cast To (Blueprint class), Is Valid, Does Implement Interface, conversions (auto-inserted when linking float to int, and so on) |
| **Strings & arrays** | Format Text, Append, Contains, Array Add/Remove/Find/Length/Get/Shuffle, Map Find/Add |
| **Debug** | Print String (on screen + console), Draw Debug Line/Sphere/Box, Breakpoint |
| **Audio / UI / Anim** | Play Sound at Location, Create Widget, Add to Viewport, Set Text, Play Montage (arrive with their phases) |
| **Utility** | Reroute node, Comment box, Local function call, Call Parent function |

### 12.4 Compilation and execution (the Blueprint VM)

The design is **graph → typed IR → register-based bytecode**, executed by
`BlueprintVM`, rather than interpreting the graph on every tick.

1. **Validate:** type-check links, find unconnected required inputs, detect
   invalid cycles in pure (data) nodes, and report errors *on the node*.
2. **Lower to IR:** walk exec links from each event/function entry. Pure
   nodes are evaluated lazily (topologically sorted into the exec node that
   consumes them, cached per exec step as Unreal does). Macros are inlined.
3. **Latent nodes:** compiled into resumable states. Each latent node splits
   the function; `Delay` stores `{instance, resume_pc, locals}` in the
   `LatentActionManager` and resumes when the timer fires. It's a coroutine
   without OS threads.
4. **Bytecode:** register VM ops like `LOAD_CONST r, k`,
   `GET_FIELD r, obj, field`, `CALL_NATIVE fn, argbase, ret`,
   `CALL_BP fn`, `JMP`, `JMP_IF_FALSE`, `SWITCH`, `LATENT_WAIT`, `RET`.
   Native calls go through the `FunctionInfo::Invoke` thunks; hot paths get
   typed fast thunks (no `Any` boxing for float/int/Vec3).
5. **Execution:** events are dispatched per `BlueprintInstance`; Tick runs
   through the Phase 9 system scheduler (Blueprint Tick systems run
   single-threaded at first, with parallelism for "thread-safe" graphs
   later).
6. **Safety:** infinite-loop guard (instruction budget per event, 1M in
   editor by default), with an error pointing at the node; null-entity
   access produces a warning, not a crash.
7. **Optional later:** "nativize" a Blueprint to generated C++ for shipping
   builds.

API:

```cpp
BlueprintCompileResult BlueprintCompiler::Compile(const Blueprint&, CompileOptions);
void BlueprintVM::Dispatch(Entity, EventId, std::span<const Any> args);
void BlueprintVM::Tick(World&, f32 dt);                  // latent actions + Tick events
DebugState& BlueprintVM::Debugger();                     // breakpoints, step, watches
```

### 12.5 Debugging

- Breakpoints on nodes (F9); PIE pauses there and the graph shows the
  **current node highlighted**.
- Step Over / Into (into function graphs) / Out; call stack panel.
- **Wire flow animation:** exec wires glow as they fire, so you can see
  execution live (Unreal-style); data pins show their last value on hover.
- Watch values: right-click a pin > Watch.
- Debug filter: pick which instance to debug when many exist ("BP_Enemy_3").

### 12.6 Editor UI: the Blueprint Editor

Uses `imgui-node-editor` (thedmd), which has smooth zoom and pan, curved
links, groups, and navigation. Wrapped in `editor/src/graph/` so the
Material, Animation, and VFX editors reuse the same widget.

```
┌ BP_Door ───────────────────────────────────────────────────────────────────────────────────┐
│ [Compile ✓] [💾 Save] [🔍 Find] [⚙ Class Settings] [▶ Debug: BP_Door_2 ▾]    Parent: Entity  │
├──────────────────┬───────────────────────────────────────────────────────┬─────────────────┤
│ MY BLUEPRINT     │  Viewport │ Construction │ ▣ Event Graph │ fn Open     │ DETAILS         │
│ ▾ Graphs      +  │ ┌──────────────────┐                                  │ Variable: IsOpen│
│   Event Graph    │ │◆ Event BeginPlay │──▶┌──────────────┐               │ Type: [bool ▾]  │
│ ▾ Functions   +  │ └──────────────────┘   │ Set IsOpen   │               │ Default: ☐      │
│   Open           │                        │ ▶        ▶   │               │ Instance Editable☑│
│   Close          │ ┌──────────────────┐   │ ● IsOpen ☐   │               │ Tooltip: ...    │
│ ▾ Macros      +  │ │◆ OnTriggerEnter  │   └──────────────┘               │ Category: Door  │
│ ▾ Variables   +  │ │ ▶          Other ●│──▶┌────────────┐  ┌───────────┐  │ Replicated: ☐   │
│   ● IsOpen  bool │ └──────────────────┘   │ ◇ Branch   │─▶│ fn Open   │  │                 │
│   ● Speed  float │                        │ ● Cond     │  └───────────┘  │                 │
│ ▾ Event Dispatch.│                        └────────────┘                  │                 │
│   ⚡ OnOpened    │  (right-click / Tab: searchable node palette,          │                 │
│ ▾ Components     │   context-sensitive to the dragged pin's type)         │                 │
│   Transform      │                                                       │                 │
│   Mesh           │                                                       │                 │
├──────────────────┴───────────────────────────────────────────────────────┴─────────────────┤
│ COMPILER RESULTS: ✓ 0 errors, 1 warning — "Branch: Condition not connected (defaults to false)" [→ node] │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

UI rules:

- **Pin colors by type**, consistent everywhere: exec white ▶,
  bool red-maroon, int teal, float green, string magenta, Vec3 gold,
  rotator/quaternion light blue, entity/object blue, asset ref purple,
  struct dark blue, enum dark green. Arrays use a grid-shaped pin icon.
- **Node header colors by category:** event red, function blue, pure
  function green, flow control gray, latent (clock icon) dark orange,
  macro gray-purple.
- **Palette:** Tab or right-click opens a fuzzy-search list; dragging off a
  pin filters to compatible nodes and auto-connects; auto-conversion nodes
  are inserted when needed.
- Shortcuts: Ctrl+C/V/X/D, Del, C (comment box around selection), Q
  (straighten links), Alt-click (break link), Ctrl-drag (move links),
  double-click wire (reroute node), F (frame selection), Home (fit graph),
  B+click (Branch), S+click (Sequence), D+click (Delay).
- Collapse selection into a function or macro (right-click).
- Minimap in the corner; bookmarks (Ctrl+1–9).
- **Diff view:** compare two revisions of a Blueprint side by side, with
  added nodes in green, removed in red, and changed pins in yellow.
- Class Defaults tab: edit default values of variables and inherited
  components.
- Components tab: a mini viewport and hierarchy, like the prefab editor,
  to build the template's component tree.

### Done when

- Sample: `BP_Door` opens when the player enters a trigger (event →
  branch → timeline → set rotation), `BP_Coin` adds score and destroys
  itself, and `BP_GameMode` shows a "You win" print when the score reaches
  10. All of it is made only in the editor, with no C++.
- Breakpoint pauses PIE and shows values; exec wire animation visible in
  screenshot tests.
- Performance: 10,000 BP instances ticking a 20-node graph costs under 2 ms
  on the target machine (benchmark test).
- Compiler unit tests: every node type, latent resume, cycle detection,
  and type conversions.

---

## Phase 13: Physics v2 and gameplay collision

(Done here because Blueprints immediately want traces, triggers, and
character movement.)

- **Colliders as components:** `BoxCollider`, `SphereCollider`,
  `CapsuleCollider`, `MeshCollider` (static triangle mesh, or convex hull
  for dynamic), `HeightfieldCollider` (Phase 21). `RigidBody` becomes
  motion-only (dynamic/kinematic/static, mass, damping, gravity scale,
  constraints/locks, CCD).
- **Physics materials** (friction, restitution, combine modes) as assets.
- **Collision layers/matrix** in Project Settings (32 layers × 32).
- **Triggers** (sensor bodies) → `OnTriggerEnter/Exit` events;
  **contacts** → `OnCollisionBegin/Stay/End` with impulse and normal. These
  are delivered on the main thread after the step (Jolt contact listener →
  queue → dispatch).
- **Queries:** `Raycast`, `SphereCast`, `ShapeCast`, `Overlap*`, with
  layer masks and ignore lists; exposed to Blueprints.
- **Character controller:** Jolt `CharacterVirtual` → `CharacterMovement`
  component (walk, run, jump, crouch, slopes, steps, moving platforms).
- **Joints/constraints:** fixed, hinge, slider, distance, cone, 6-DOF;
  ragdolls later (Phase 16).
- **Vehicles** (Jolt `VehicleConstraint`) as an optional component.
- **Editor:** collider gizmos (wireframe, editable handles), a physics debug
  draw toggle in the viewport menu, and a "Generate collision" button in the
  mesh editor.

**🎯 M2 complete:** someone can make a small but real game (a collect-coins
platformer) entirely in the editor with Blueprints or Luau.

---

# M3: Good-looking and alive

## Phase 14: Unified Renderer in the editor and game

**Why:** the PBR, IBL, shadows, glTF skinning, and GPU culling work exists
in demos. It needs to become **one renderer library** that the editor
viewport and the player both use, running on the backend-agnostic RHI.

### Work

- New `renderer/` module: `Renderer::Render(const RenderScene&, const View&, RenderTarget&)`.
  - `RenderScene` is extracted from ECS each frame (`MeshRenderer`, `Light`,
    `Camera`, `ReflectionProbe`, `SkyAtmosphere`, `Decal` components) into
    flat arrays, which keeps rendering decoupled from gameplay threads.
  - Built on `RenderGraph`. Move the render graph onto the RHI (today it's
    D3D12-state based) so both D3D12 and Vulkan run the full feature set.
- **Pipeline (clustered forward+)**, chosen for MSAA and material
  flexibility and to reuse the existing forward PBR shaders:
  1. GPU culling (existing compute pass) → indirect draws
  2. Depth pre-pass (+ Hi-Z)
  3. Light clustering (compute, 16×9×24 froxels) for **hundreds of lights**
  4. Shadow maps: **cascaded shadow maps** for the sun (4 cascades),
     atlas-based spot/point shadows, PCF/PCSS
  5. Forward PBR opaque (existing Cook-Torrance/IBL shaders)
  6. SSAO (GTAO), screen-space reflections (optional)
  7. Sky (physically based atmosphere) + fog (height + volumetric later)
  8. Transparent pass (sorted), then particles (Phase 19)
  9. Post: TAA, bloom, auto exposure, tone mapping (ACES/AgX), color
     grading LUT, vignette, DOF, motion blur, FXAA as a fallback
  10. UI (Phase 18), then editor overlays (gizmos, selection outline via
      stencil/jump flood, grid)
- **Components:** `MeshRenderer {AssetRef<Mesh>, materials[]}`,
  `SkinnedMeshRenderer`, `DirectionalLight`, `PointLight`, `SpotLight`,
  `AreaLight` (later), `SkyLight`, `ReflectionProbe` (baked cubemap
  capture), `PostProcessVolume` (global or bounded, blended),
  `Decal`.
- **LOD:** mesh LOD chain generated at import (meshoptimizer) with
  screen-size selection.
- **Instancing:** automatic batching of identical mesh+material.
- **Editor view modes:** Lit, Unlit, Wireframe, Normals, Albedo,
  Roughness/Metal, Overdraw, Light complexity, Shadow cascades, LOD
  coloring.
- **Editor rendering on the RHI:** the ImGui backend moves to the RHI too,
  so the editor runs on Vulkan (a prerequisite for Linux in Phase 24).

### Done when

- The editor viewport renders the `pbr_demo` scene (IBL + shadows + normal
  maps) from a saved `.ascene`, with pixel parity against `pbr_demo`'s
  screenshot (reuse the existing screenshot tooling).
- 1,000 dynamic point lights at 60 FPS at 1080p on the dev GPU.
- The same scene renders identically on D3D12 and Vulkan (pixel diff within
  tolerance).

---

## Phase 15: Material System and Material Graph Editor

### Work

- **Material asset:** shading model (Default Lit, Unlit, Subsurface,
  Clear Coat later), blend mode (Opaque/Masked/Translucent/Additive),
  two-sided, a parameter set.
- **Material graph** (reuses the Phase 12 graph widget): nodes compile to
  HLSL (**graph → HLSL codegen → DXC → DXIL/SPIR-V**), then cached shader
  permutations in the DDC.
  - Nodes: Texture Sample, TexCoord, Constant/Vector params, Math
    (add/mul/lerp/pow/dot/…), Fresnel, Panner, Time, World Position,
    Vertex Normal, Normal Map unpack, Noise, Triplanar, Custom HLSL node,
    **Material Functions** (reusable subgraphs).
  - Output node pins: Base Color, Metallic, Roughness, Normal, Emissive,
    AO, Opacity, Opacity Mask, World Position Offset.
- **Material Instances:** override parameters without recompiling; editable
  at runtime from Blueprints (`SetScalarParameter`, `SetVectorParameter`,
  `SetTextureParameter`).
- Hot reload with live preview while you edit.

### Editor UI

```
┌ M_Rock ──────────────────────────────────────────────────────────────────┐
│ [Apply ✓] [💾] Preview: ● Sphere ▢ Cube ▣ Plane ⬡ Mesh…    Stats: 142 ins │
├───────────────┬────────────────────────────────────────┬─────────────────┤
│  PREVIEW      │   [T] Albedo ─────────▶ Base Color     │ DETAILS         │
│  (rotating    │   [T] Normal ─▶ Normal ─▶ Normal       │ Blend: Opaque ▾ │
│   sphere)     │   [S] Rough 0.7 ──────▶ Roughness      │ Shading: Lit ▾  │
│               │   ┌──────────────┐                     │ Two sided ☐     │
│ PARAMETERS    │   │  ◉ MATERIAL  │                     │                 │
│ Rough ━━●━ 0.7│   │ Base Color ● │                     │                 │
│ Tint  ■       │   │ Metallic   ● │                     │                 │
└───────────────┴────────────────────────────────────────┴─────────────────┘
```

Each node shows a small preview thumbnail of its output.

---

## Phase 16: Animation System

### Work

- Assets: `Skeleton`, `AnimationClip` (from glTF, compressed with
  quantized curves and keyframe reduction), `AnimationGraph`, `BlendSpace`
  (1D/2D), `Montage` (sections, slots, notifies).
- **Animator component** evaluated in a parallel job per character
  (sampling → blending → local-to-model), then GPU skinning (the existing
  `gltf_demo` skinning path moved into the renderer).
- **Animation graph editor** (graph widget again):
  - **State machine** with states, transitions (conditions from graph
    variables, blend time, curves), conduits, and sub-state-machines
  - Blend nodes: Blend by bool/int/enum, Layered blend per bone (upper/lower
    body), Additive, Blend Space player
- **Root motion** (applied to `CharacterMovement`).
- **Anim notifies** (footsteps, hit frames) → events in Blueprints.
- **IK:** two-bone IK (feet, hands), look-at, FABRIK; foot placement on
  terrain.
- **Retargeting** between skeletons (bone mapping + scale).
- **Ragdolls** (Jolt `Ragdoll`) with blend to and from animation.
- Timeline/**Sequencer (cinematics)** later: keyframe any reflected
  property, camera cuts, events, audio tracks.

### Editor UI

- Anim asset viewer: playback bar, bone hierarchy, notify track, curves.
- Blend space editor: a 2D grid with sample points and a live preview dot
  you drag.
- State machine graph: states as rounded boxes, transitions as arrows with a
  rule icon; the active state is highlighted during PIE.

---

## Phase 17: Audio

### Work

- Backend: **miniaudio** (single-header, cross-platform) at first, with a
  wrapper so FMOD or Wwise integration can be added later.
- Assets: `SoundWave` (imported WAV/OGG/FLAC, streamed if long),
  `SoundCue` (small graph: random, sequence, modulate pitch/volume,
  concatenate), `AudioMixer` (buses: Master → Music/SFX/UI/Voice; effects:
  low-pass, reverb, compressor), `Attenuation` settings.
- Components: `AudioSource` (2D/3D, loop, spatial blend, doppler, priority),
  `AudioListener` (follows the camera by default), `ReverbZone`.
- Voice limiting and virtual voices; occlusion via physics raycasts (later).
- Blueprint nodes: Play Sound 2D/at Location, Spawn Sound Attached, Fade
  In/Out, Set Bus Volume.
- Editor: waveform preview, sound cue graph, mixer panel with live VU
  meters.

---

## Phase 18: Runtime Game UI (UMG / Godot Control equivalent)

**Why:** ImGui is for tools, not for shipped game menus and HUDs.

### Work

- New `ui/` module: a retained-mode **widget tree** rendered by the renderer
  (SDF text via the Phase 8 font importer, 9-slice images, clipping).
- **Layout:** anchors + offsets (Unity/UMG-style) and containers:
  Canvas, HBox, VBox, Grid, Overlay, ScrollBox, SizeBox, Spacer.
- **Widgets:** Text, Image, Button, Toggle, Slider, ProgressBar,
  TextInput, Dropdown, ListView (virtualized), Tooltip.
- **Styles/themes** as assets; resolution scaling (DPI curve or reference
  resolution); safe areas.
- Input: mouse, **gamepad focus navigation**, touch; UI input context
  (Phase 10) consumes input before gameplay.
- **Data binding:** bind a widget property to a reflected field or a
  Blueprint function.
- **Widget Blueprints:** a UI layout plus an event graph (OnClicked → Open
  Level) using the Phase 12 VM.
- UI animations (tween position/opacity/scale; keyframe timeline).
- World-space UI (health bars over enemies) via render-to-texture or
  direct 3D quads.

### Editor UI: UI Designer

```
┌ WBP_MainMenu ──────────────────────────────────────────────────────────────┐
│ [Designer | Graph]   Screen: 1920×1080 ▾  Zoom 50%  DPI preview 1.0 ▾      │
├──────────────┬───────────────────────────────────────────┬─────────────────┤
│ PALETTE      │   ┌───────────────────────────────────┐   │ DETAILS         │
│ 🔍           │   │         MY AWESOME GAME           │   │ Button "Play"   │
│ ▾ Common     │   │                                   │   │ Anchors [⊞ ▾]   │
│  Text Button │   │          [   PLAY   ]  ◄selected  │   │ Pos X 0 Y -40   │
│  Image Slider│   │          [ OPTIONS  ]             │   │ Size 320 × 64   │
│ ▾ Layout     │   │          [   QUIT   ]             │   │ Style: Primary▾ │
│  VBox Grid   │   └───────────────────────────────────┘   │ ▾ Events        │
├──────────────┤                                           │  OnClicked [+]  │
│ HIERARCHY    │                                           │                 │
│ ▾ Canvas     │   ANIMATIONS: [FadeIn] [+]  ▶ ━━━●━━━━━   │                 │
│   ▾ VBox     │                                           │                 │
│     Play     │                                           │                 │
└──────────────┴───────────────────────────────────────────┴─────────────────┘
```

---

## Phase 19: VFX / Particle System

- GPU particle simulation (compute) with a CPU fallback for small emitters.
- **Emitter graph** (module stack like Unreal Niagara / Unity VFX Graph):
  Spawn (rate, burst), Initialize (lifetime, velocity cone, size, color),
  Update (gravity, drag, curl noise, vortex, attractors, collision against
  depth buffer), Render (sprite/billboard, mesh, ribbon/trail, light).
- Curves and gradients over lifetime; sub-emitters (on death / on
  collision); soft particles; flipbook animation; sorting.
- `ParticleSystem` component; Blueprint nodes Spawn Emitter at
  Location/Attached, Set Parameter.
- Editor: live preview viewport with a timeline scrubber, and per-emitter
  stats (count, GPU time).

**🎯 M3 complete:** games look good, animate, sound right, and have real
menus and HUDs.

---

# M4: Complete games

## Phase 20: AI and Navigation

- **NavMesh** generation with **Recast/Detour** (zlib license): per-agent
  settings (radius, height, step, slope), baked per scene with runtime tile
  rebuild for dynamic obstacles, off-mesh links (jumps, ladders), area
  costs.
- `NavAgent` component: pathfinding, steering, local avoidance (Detour
  crowd).
- **Behavior Trees** (asset + graph editor): Selector, Sequence, Parallel,
  Decorators (Blackboard condition, Cooldown, Loop, TimeLimit), Services,
  Tasks (MoveTo, Wait, Play Anim, Run Blueprint Task). A **Blackboard**
  asset holds typed keys. Tasks can be written as Blueprints or Luau.
- **State Trees / utility AI** as an alternative later.
- **Perception:** sight (cone + raycast), hearing (noise events), damage;
  `AIPerception` component with events to Blueprints.
- Editor: NavMesh visualization overlay; BT debugger showing the active path
  during PIE.

---

## Phase 21: World Building (terrain, foliage, large worlds)

- **Terrain:** heightmap-based with GPU tessellation or CDLOD clipmaps; a
  splat-map material layer system; Heightfield collider.
- **Sculpt and paint tools:** raise/lower, smooth, flatten, noise, erosion
  (later), layer painting; brush size/falloff/strength in a tool panel with
  a viewport brush cursor.
- **Foliage:** instanced painting (GPU culled, LOD, impostors), with density
  and random scale/rotation/alignment.
- **Spline tools:** roads, rivers, fences placed along splines.
- **Large worlds:** world partition / scene streaming by grid cells, using
  additive scenes from Phase 9; floating-origin or double-precision world
  positions.
- **Lighting bake** (later): lightmaps or probe-based GI (DDGI on hardware
  ray tracing where available); baked reflection probes.

---

## Phase 22: Networking and Multiplayer

- Transport: **GameNetworkingSockets** (Valve, BSD license) or ENet;
  reliable/unreliable channels.
- **Server-authoritative replication** built on reflection (Phase 6):
  `Replicated` field flag, `NetIdentity` component, relevancy by distance,
  delta compression, per-connection priority.
- **RPCs:** `Server`, `Client`, `Multicast` function flags, callable from
  Blueprints and scripts.
- Client-side prediction and reconciliation for `CharacterMovement`;
  snapshot interpolation for others.
- Sessions/lobby abstraction (LAN discovery first; Steam/EOS plugins
  later).
- **Editor:** PIE with N clients plus a listen or dedicated server (each in
  its own world, and optionally its own window); network profiler
  (bandwidth per entity/field); simulated latency and packet loss.

---

## Phase 23: Profiling, Debugging, and Developer Tools

- **Tracy** integration (CPU zones in the job system and systems, GPU zones
  in the render graph, memory tracking via the existing allocators).
- In-editor **profiler panel**: frame-time graph, per-system times, render
  pass GPU times, draw call and triangle counts, memory by category.
- **Console** with CVars (`r.shadows.cascades 4`, `physics.debug 1`) and
  commands; available in game builds via the tilde key.
- **Debug draw API** (lines, boxes, spheres, text in world) from C++,
  Luau, and Blueprints, with a duration option.
- Stat overlays (`stat fps`, `stat gpu`, `stat memory`).
- **Crash handler:** minidump + log capture + a crash reporter dialog.
- Automated tests: a functional test framework (for example, "load
  scene, simulate 5 s, assert entity X reached trigger Y") runnable
  headless in CI.

**🎯 M4 complete:** feature coverage comparable to a mid-size commercial
engine.

---

# M5: Shippable

## Phase 24: Cross-platform

- **Platform layer:** abstract `Window`/input/filesystem/threads. Add
  **Linux** (X11 + Wayland via SDL3 or GLFW, Vulkan) and **macOS** (Cocoa +
  **MoltenVK** at first; a native Metal RHI backend later).
- The editor runs on Vulkan (it already needs the RHI-hosted ImGui from
  Phase 14).
- CI matrix: Windows (MSVC, D3D12 + Vulkan), Linux (Clang/GCC, Vulkan
  with lavapipe for headless screenshot tests).
- Later: Android (Vulkan) and consoles through a private-SDK plugin
  structure. Consoles need NDAs, so the code structure only has to allow for
  them.

---

## Phase 25: Build, Cook, and Package

- **Cooker** (`tools/aether-cook`): walk dependencies from the startup
  scene and configured "always cook" assets, convert to platform-optimal
  formats (BC7/ASTC textures, compiled shaders for DXIL/SPIR-V, binary
  archives, precompiled Blueprint bytecode), strip editor-only data, and
  write **`.apak` archives** (with compression via zstd/LZ4 and optional
  encryption).
- **Player** (`player/`): a minimal executable that loads the game module
  plus the pak files, runs the startup scene, and has no editor code.
- Build configurations: Debug, Development (console and profiling),
  Shipping (stripped, optimized).
- **Editor UI: Build/Package window:** target platform, configuration,
  scenes to include, icon, splash screen, version, output folder;
  "Package" runs the cook and build in the background with progress and
  log output; "Launch" runs the packaged game.
- Patching/DLC via extra pak files (later).
- Project Settings: startup scene, window mode/resolution defaults,
  quality presets (Low/Med/High/Epic mapping CVars).

---

## Phase 26: Ecosystem (templates, plugins, docs)

- **Project templates:** Blank, First-Person Shooter, Third-Person, 2D
  Platformer (needs the 2D features below), Top-Down, Vehicle. Each has a
  small Blueprint-based starter game.
- **Plugin system:** `.aplugin` descriptor, a runtime and/or editor module,
  and content. The engine's own optional modules (physics, audio, AI,
  networking) use the same system.
- **Editor extensibility API:** custom panels, inspectors
  (`RegisterPropertyDrawer<T>`), asset types, and menu items, from C++ and
  from Luau editor scripts (like Unity's `EditorWindow` / Godot's
  `@tool`).
- **2D toolkit:** sprites, sprite atlases, tilemaps + tileset editor,
  2D physics (Box2D v3), 2D lights, and a pixel-perfect camera. This gives
  a Godot-like 2D workflow.
- **Documentation:** API reference generated from reflection plus Doxygen,
  a manual (Getting Started, Blueprints guide, Luau guide), and sample
  projects.
- Version control integration in the Content Browser: Git LFS status
  badges and checkout/lock for binary assets.

---

# M6: Advanced and specialized systems

These phases come after the engine can ship a game. They're the features
large productions and specific genres expect. Most of them are independent,
so after M5 they can be built in any order, depending on which games are
being made with Aether.

## Phase 27: Cinematics (Sequencer / Timeline)

**Why:** cutscenes, trailers, scripted events, and in-game camera moves.
Phase 16 only mentions this briefly.

### Engine / runtime work

- `LevelSequence` asset: a set of **tracks** bound to entities by
  `EntityGuid` (or "spawnable" entities the sequence creates itself).
- Track types:

  | Track | Keys |
  |---|---|
  | Transform | position/rotation/scale curves (Bezier, linear, constant) |
  | Property | any reflected field (Phase 6): float, color, bool, enum |
  | Animation | clips with blend in/out, play rate, slot |
  | Camera Cut | which `CineCamera` is active, with blends |
  | Audio | sound, start offset, volume curve |
  | Event | fires a Blueprint custom event at a time |
  | Visibility / Spawn | enable/disable or spawn/destroy an entity |
  | Fade / Post-process | screen fade, post-process volume weight |
  | Subsequence | another sequence, time-offset (shots inside a scene) |

- `CineCamera` component: focal length, sensor size, aperture and focus
  distance (drives DOF), rack focus, and a camera rig (rail, crane).
- `SequencePlayer` component and Blueprint nodes: Play, Pause, Stop, Set
  Time, Set Play Rate, OnFinished.
- **Movie render:** render a sequence offline to a PNG/EXR sequence at a
  fixed frame rate, with high-quality anti-aliasing (accumulate N TAA
  samples per frame).

### Editor UI

```
┌ SEQ_Intro ───────────────────────────────────────────────────────────────────────┐
│ [⏮][◀][▶][⏭]  00:04:12 / 00:30:00   24 fps ▾   🔒 Lock camera   [🎬 Render]      │
├────────────────────────┬─────────────────────────────────────────────────────────┤
│ + Track ▾   🔍         │ 0s      2s      4s ▼    6s      8s      10s             │
│ ▾ 🎥 Camera Cuts       │ [ Cam_Wide ──────][ Cam_Close ─────────][ Cam_Wide ]     │
│ ▾ 🧍 Hero              │                                                         │
│    Transform           │  ◆───────◆──────────◆                                   │
│    Animation           │ [ Idle ][ Walk ──────────][ Wave ]                       │
│ ▾ 💡 Sun               │                                                         │
│    Intensity           │  ◆──────────────────────────◆                           │
│ ⚡ Events              │            ▲ OpenDoor                                    │
│ 🔊 Audio               │ [ music_intro.ogg ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ ]     │
└────────────────────────┴─────────────────────────────────────────────────────────┘
```

A curve editor tab opens for any selected track. Pressing **Record** turns
edits made in the viewport or Inspector into keys automatically.

### Done when

- A 30-second cutscene with 3 camera cuts, character animation, an event
  that opens a door, and music plays back in PIE and renders to a PNG
  sequence.

---

## Phase 28: Save Game and Persistence

**Status:** done except the cloud-save plugins and the player applying volumes and fullscreen (see `docs/design/PHASE_SPECS.md` §28.7).

- `SaveGame` object: a reflected struct the game defines
  (`AETHER_REFLECT(MySave, ...)`) and saves to a slot using the Phase 6
  archive; versioned, with migration hooks.
- `SaveSystem::Save(slot, obj)`, `Load(slot)`, `ListSlots()`,
  `DeleteSlot(slot)`; async so saving doesn't hitch; atomic write
  (write to temp, then rename) so a crash never corrupts a save.
- **World state persistence:** a `SaveableEntity` component marks entities
  whose flagged fields are captured (doors opened, pickups collected,
  enemy killed), keyed by `EntityGuid`.
- Settings (graphics quality, audio volumes, keybindings) stored
  separately from game saves.
- Platform backends: local file first; cloud saves (Steam/EOS) as plugins.
- Blueprint nodes: Save Game to Slot, Load Game from Slot, Does Save Game
  Exist, Create Save Game Object.
- Editor: a **Save Inspector** panel that opens a `.asav` and shows its
  contents with the generic Inspector (useful for debugging).

---

## Phase 29: Localization and Text

**Status:** done except the optional shaping step (HarfBuzz joining for Arabic, gender selectors); see `docs/design/PHASE_SPECS.md` §29.

- `LocText` type: a string key plus source text, used for every
  player-visible string; the runtime UI (Phase 18) and Blueprint text
  pins take `LocText`, not raw strings.
- **String tables** (CSV/PO import and export) per language; the gather
  step scans scenes, prefabs, Blueprints and widgets for `LocText` and
  produces a translation file.
- Runtime language switch without a restart; fallback chain
  (`pt-BR → pt → en`).
- Text shaping for complex scripts via **HarfBuzz**; right-to-left layout
  (Arabic, Hebrew); font fallback (Latin → CJK → emoji); plurals and
  gender through ICU MessageFormat-style arguments (`{count} coin{count|s}`).
- Localized assets (a different texture or voice line per language).
- Editor: a **Localization Dashboard** (languages, completion percentage
  per language, missing keys) and a pseudo-localization preview mode that
  makes strings longer and accented to catch clipped UI.

---

## Phase 30: Gameplay Ability System and RPG Toolkit

**Status:** in progress (gameplay tags, attributes, effect system, ability system, Luau face, editor debugger, inventory, interaction and quests kits; see `docs/design/PHASE_SPECS.md` §30).

**Why:** most action, RPG and MOBA games rebuild the same systems: stats,
buffs, cooldowns and abilities. Unreal's Gameplay Ability System (GAS) is
the model to follow.

- **Attributes:** an `AttributeSet` component (Health, Mana, Stamina,
  AttackPower, …) with base and current values; changes are events.
- **Gameplay Effects** (asset): instant, duration, or infinite modifiers
  (`add`/`multiply`/`override`) on attributes, with stacking rules,
  periodic ticks (poison: −5 HP per second), and conditions.
- **Gameplay Tags:** hierarchical tags (`State.Stunned`,
  `Damage.Fire`) used by abilities to block and cancel each other.
- **Abilities** (Blueprint subclass of `GameplayAbility`): activation
  requirements (cost, cooldown, required/blocked tags), `ActivateAbility`
  graph with latent tasks (Play Montage and Wait, Wait for Input Release,
  Wait for Target Data), commit and end.
- Networked (prediction keys) once Phase 22 exists.
- **Genre kits on top**, each as an optional plugin:
  - Inventory and items (item definitions as assets, stacks, equipment
    slots, a drag-and-drop inventory widget template)
  - **Dialogue graph** (nodes: line with speaker, portrait and voice;
    choice; condition; set variable; event), with a dialogue editor using
    the Phase 12 graph widget
  - **Quest system** (objectives, prerequisites, rewards, a quest log
    widget template)
  - Interaction system (look-at or proximity prompts, "Press E to open")
- Editor: an **Attribute debugger** overlay during PIE (stats, active
  effects with remaining time, tags on the selected entity).

---

## Phase 31: Advanced Rendering

Phase 14's clustered forward+ pipeline covers most games. This phase adds
the features that high-end productions compare against Unreal 5.

- **Hardware ray tracing** (DXR 1.1 / `VK_KHR_ray_query`): ray-traced
  shadows, reflections and ambient occlusion, with rasterized fallbacks.
  The RHI gains acceleration structures (BLAS/TLAS) and ray query
  support.
- **Dynamic global illumination:** probe-based DDGI (ray traced) first;
  screen-space GI as a fallback on hardware without ray tracing.
- **Virtualized geometry** (Nanite-like): meshlet clustering at import
  (meshoptimizer), GPU cluster culling, mesh shaders where supported,
  hierarchical LOD. This is a research-sized project; do it only when
  scenes need millions of instances.
- **Virtual shadow maps** to pair with virtualized geometry.
- **Upscalers:** FSR 3 (open source) built in; DLSS and XeSS as plugins
  (their SDK licenses don't allow them in the core repo).
- **Volumetrics:** volumetric fog and lighting (froxel-based, reusing the
  Phase 14 clusters), volumetric clouds, god rays.
- **Water:** FFT ocean, shoreline foam, underwater post-process, and
  buoyancy (with physics).
- **Hair and fur** (strand-based later; cards first), **skin** with
  subsurface scattering (separable SSS).
- **Decals v2:** deferred decals that affect normal and roughness.
- **Scalability:** every feature has quality levels tied to the Phase 25
  presets.

### Done when

- A benchmark scene reaches 60 FPS at 1440p with ray-traced shadows and
  reflections on a mid-range RT GPU, and falls back automatically with the
  same look (within tolerance) on a GPU without ray tracing.

---

## Phase 32: XR (VR and AR)

- **OpenXR** runtime integration (Meta Quest via Link/Air Link, SteamVR,
  Windows Mixed Reality, Pico).
- Stereo rendering: single-pass instanced (both eyes in one draw);
  foveated rendering where the runtime supports it.
- `XRRig` prefab: head camera, hand controllers with input actions
  (Phase 10 gains XR device bindings), hand tracking poses.
- Interaction toolkit: grab (direct and distance), teleport locomotion,
  smooth locomotion with comfort vignette, snap turn, UI pointer for
  world-space widgets (Phase 18).
- Editor: **VR preview** in PIE, and a "VR template" project.
- AR (later): passthrough on Quest, plane detection, anchors.

---

## Phase 33: Mobile and Web

- **Android** (Vulkan, NativeActivity, Gradle packaging from the Phase 25
  Package window) and **iOS** (Metal backend, Xcode project generation).
- Mobile renderer path: tile-friendly forward pipeline, fewer passes,
  ASTC textures, half-resolution post, dynamic resolution.
- Touch input: gestures (tap, swipe, pinch, rotate), a virtual joystick
  widget template.
- Power and thermal: frame-rate caps, pause when backgrounded.
- **Web** (later): WebGPU backend + Emscripten for playable browser
  builds and demos.

---

## Phase 34: Modding and User-Generated Content

- Mod packages: a `.aplugin` (Phase 26) plus cooked content in a `.apak`,
  loaded at startup from a `Mods/` folder in a defined order.
- **Safe scripting for mods:** Luau is already sandboxed (Phase 11); mods
  get a restricted API surface with no filesystem or network access by
  default.
- A stripped-down "mod editor" build of the editor that only works with
  the game's content and exposed types.
- Asset overrides (replace a texture or mesh by GUID) and additive content
  (new levels, items).
- Steam Workshop / mod.io integration as plugins.

---

## Phase 35: Accessibility

Accessibility is cheaper to build in than to add later, so the runtime
UI and input phases should follow these rules from the start. This phase
is where the full set lands.

- **Input:** full remapping (Phase 10), hold-to-toggle options, adjustable
  dead zones and sensitivity, one-handed presets.
- **Visual:** subtitle system (size, background, speaker names and colors,
  directional indicators for off-screen sounds), colorblind filters
  (protanopia/deuteranopia/tritanopia simulation and correction), UI
  scale, high contrast mode, reduced motion (disables camera shake and
  head bob).
- **Audio:** mono audio, per-bus volume, visual cues for important sounds.
- **Screen reader** support for menus (UI Automation on Windows, AT-SPI on
  Linux) using the widget tree's names and roles.
- **Editor accessibility too:** keyboard navigation of every panel, UI
  scale, and colorblind-safe pin and node colors (Phase 12's pin colors
  get shapes as well, so color is never the only signal).

---

## Phase 36: AI-assisted tools (optional)

These are editor conveniences, never runtime requirements. The engine must
work fully without them.

- **Blueprint assistant:** describe logic in text ("when the player enters
  this trigger, open the door over 1 second") and the assistant proposes a
  node graph the user reviews and accepts. It uses the same Phase 12 graph
  model, so the output is ordinary, editable nodes.
- **Explain this graph / this error:** a plain-language summary of a
  selected graph or compiler error.
- **Asset search by description** ("red brick wall texture") using
  embeddings of names, tags and thumbnails.
- Implemented as an editor plugin with a pluggable model provider, off by
  default, and clearly labeled when content was generated.

---

# M7: The Aether Upgrade (from feature-complete to production-grade)

Phases 1-36 make Aether a **feature-complete** engine. They do not make it something other
people can build and ship commercial games on. M7 is a separate upgrade roadmap that starts
**when Phase 36 is finished** and changes the question from "what features are missing?" to "is
this dependable?". The pattern is the usual one for mature engines: after feature expansion,
the work shifts to optimization, quality assurance, tooling, compatibility and release stability.

**Rules for M7:**

- **No more open-ended feature phases.** Every M7 phase ends on measurable acceptance (benchmarks,
  budgets, soak tests, a sample game that ships), not on "the feature exists".
- **It revisits earlier phases, it doesn't replace them.** "Graphics 2.0" hardens and extends
  Phase 31; "Multiplayer 2.0" does the same for Phase 22. Each M7 phase names what it builds on.
- **Everything ends at the production gate** (below). Phase 48 is the release; nothing is "done"
  until the gate says so.
- Keep the cross-cutting rules (section 3), and keep the honest-documentation habit: record what
  was measured and how, including what didn't work.

## Phase 37: Engine 2.0 Core (API cleanup, architecture, stability)

- **Why:** a decade of phases leaves seams: duplicated helpers, inconsistent naming, modules that
  reach into each other, optional kits wired by hand into the player.
- **Work:** an API audit module by module (naming, ownership, error handling, `const`-ness, what is
  public); one module/kit registration mechanism for the player, the editor and scripting (today
  each kit adds CMake options, defines and stage code by hand); dependency-graph cleanup with a CI
  check that forbids new cycles; a deprecation policy (`AETHER_DEPRECATED` with a version and a
  replacement); sanitizer and static-analysis clean builds as a gate.
- **Done when:** the module graph is acyclic and checked in CI; a kit can be added without editing
  `game.cpp`; the public headers have documented stability levels (stable / experimental / internal).

## Phase 38: Performance

- **Why:** the performance budgets in section 3 are targets, not measurements.
- **Work:** a benchmark suite with checked-in baselines and a CI regression gate (ECS iteration and
  structural changes, job system, scene load, physics step, render extraction, Blueprint VM, Luau
  calls, save/load, cook); ECS (the component-type cap, now 256, made dynamic or proven sufficient; chunk layout; query caching); renderer
  (draw submission, culling, GPU-driven paths where the RHI supports them, pipeline and shader
  caches); memory (per-system budgets, allocation tracking, per-frame allocation audit);
  multithreading (a task graph with inferred dependencies, a render thread, async asset jobs); GPU
  profiling and optimization passes with Tracy zones everywhere.
- **Done when:** the budgets in section 3 are met on the reference machines and enforced in CI; a
  10k-entity scene loads in under 2 s; frame-time histograms and memory per system are in every
  benchmark report.

## Phase 39: Graphics 2.0

- **Builds on:** Phases 14, 15, 31.
- **Work:** one rendering path across D3D12, Vulkan and MoltenVK with feature-level fallbacks;
  advanced PBR (clear coat, sheen, anisotropy, subsurface, hair, cloth, decals); virtual geometry
  (meshlet clusters with hierarchical LOD and streaming, in the spirit of Nanite, with a fallback
  for hardware without mesh shaders); virtual texturing; a mature GI/shadow stack and upscaling;
  render-graph validation and capture tooling.
- **Done when:** the same scene renders within tolerance on all three backends, a million-triangle
  asset is usable with virtual geometry, and the render-graph validator runs in CI.

## Phase 40: AI Engine

- **Builds on:** Phase 20.
- **Work:** navigation at scale (tiled navmesh streaming, dynamic obstacles, hierarchical paths,
  crowds in the thousands); behavior tooling (utility AI, GOAP, state trees next to behavior trees,
  debugging and visualization); perception 2.0; an optional ML integration layer (ONNX runtime
  inference as a runtime feature for NPC behavior, never required); a clean boundary for AI *agents*
  that drive the editor (Phase 44) kept separate from gameplay AI.
- **Done when:** a 1000-agent crowd scene meets its budget; a behavior can be authored, debugged
  and profiled entirely in the editor; an ONNX model runs in a sample with deterministic fallback.

## Phase 41: Multiplayer 2.0

- **Builds on:** Phase 22, Phase 30 (prediction for abilities).
- **Work:** dedicated server builds and headless operation; matchmaking and sessions behind a
  backend-agnostic service interface; replication optimization (priority, relevancy, bandwidth
  budgets, interest management, delta compression measurements); prediction and rollback for the
  gameplay ability system; cheat-resistance basics (server authority audit, input validation);
  network soak and loss/latency simulation in CI.
- **Done when:** a 64-player dedicated-server sample runs a soak test within bandwidth and tick
  budgets under simulated packet loss.

## Phase 42: World Streaming 2.0

- **Builds on:** Phase 21.
- **Work:** worlds of hundreds of kilometres (large-world coordinates end to end: physics,
  rendering, audio, networking); asynchronous cell streaming with priorities and hitch-free budgets;
  LOD and HLOD generation; world-partition authoring tools (data layers, level instances at scale);
  streaming of audio, navmesh and gameplay data with the cells.
- **Done when:** a very large generated world is traversed at speed with no frame hitch above budget
  and bounded memory, in an automated flythrough test.

## Phase 43: Editor 2.0

- **Work:** the pending editor items finished (docking branch, `editor/main.cpp` split, gizmos,
  multi-select, copy/paste, Simulate mode, the New/Open Project UI); layout and workspace
  presets; a command palette and global search; asset management at scale (thumbnails, dependency
  and reference viewer, bulk rename and move, validation, source control integration); UX polish
  driven by recorded task walkthroughs (make a level, make a character, make a UI).
- **Done when:** the "first game" tutorial and two larger walkthroughs are completed in the editor
  without leaving it, and an editor soak test (open, edit, undo, play, stop, for hours) is stable.

## Phase 44: AI-Native Editor

- **Builds on:** Phase 36 (the optional assistant) and the editor's command stack (Phase 7).
- **Idea:** the editor and the running game are exposed as a **tool surface** that AI agents can use
  through the Model Context Protocol (MCP), with the human always in the loop.
- **Work:** an MCP server in the editor exposing typed, versioned tools: query and edit the scene
  and prefabs, create and edit assets (materials, Blueprints, quests, items, effects, dialogue),
  import and cook, build and package, run the test suite and the functional tests, play-in-editor
  with a bounded step and capture (screenshots, logs, profiler summaries), read diagnostics. Every
  edit goes through the undo stack and is attributed to the agent; destructive tools need
  confirmation; a project-level permission file says what agents may touch; agent sessions are
  logged. Reference agents on top: a code agent (scripts, native modules), an asset agent, and a
  level/game-design agent that composes gameplay-kit data (abilities, quests, items, NPCs) and
  proposes a scene for review. Everything stays optional and off by default, like Phase 36.
- **Done when:** an agent can, from a written brief, build a small playable level (terrain, enemies,
  abilities, a quest, NPCs), compile and run the tests, and report failures, with every change
  reviewable and undoable, and the engine works fully without any of it.
- **Note:** the repository already has an MCP server (`mcp/`, option `AETHER_BUILD_MCP`): scene
  inspection and editing, undo/redo, scene save/load and Play-in-Editor over stdio, with every change
  going through the editor's command stack. This phase hardens and extends it (versioned tool schemas,
  agent attribution, permissions, confirmation, kit tools, build/test tools, bounded capture) rather
  than starting it.

## Phase 45: Mobile and Console

- **Builds on:** Phases 24, 33.
- **Work:** Android and iOS production builds (lifecycle, permissions, thermal and memory pressure,
  store packaging); ARM everywhere; controller and touch input abstraction with remapping UI and
  haptics; console readiness through the platform-plugin interface (certification checklists,
  save/storage, suspend/resume, performance-mode profiles); device farms in CI for at least one
  phone class.
- **Done when:** a sample game ships as a signed Android and iOS build that passes lifecycle tests
  and holds its frame budget on a reference device.

## Phase 46: Developer Ecosystem

- **Builds on:** Phase 26.
- **Work:** a stable SDK (headers, libraries, CMake package config, versioned ABI for native
  modules); plugin packaging, versioning and dependency resolution; project templates and sample
  games kept current by CI; a plugin/asset marketplace specification (metadata, signing, licensing,
  review) and a local registry; documentation site generated from the code and the manual.
- **Done when:** a third party can build a plugin and a game against the SDK from a clean machine
  using only the published instructions, and the sample games build in CI against the SDK.

## Phase 47: Production QA

- **Work:** fuzzing (asset importers, scene/Blueprint/save loaders, the network protocol, the
  cooker and pak reader) in CI; crash recovery (editor autosave and restore, crash dumps with
  symbolication, a safe mode); a soak/endurance suite; profiling and memory-leak gates on every
  release candidate; automated functional and screenshot test farms per platform and backend;
  accessibility and localization regression checks; security review of the plugin and MCP surfaces.
- **Done when:** the fuzzers run clean for a fixed budget on every release candidate, the editor
  recovers from an injected crash with no data loss, and the QA matrix is green on every supported
  platform.

## Phase 48: Aether 1.0

- **Work:** the production gate below, executed; API freeze for everything marked stable; migration
  tooling for every versioned format; release notes, upgrade guide and long-term-support policy;
  signed release builds of the editor, player and SDK for each platform; sample games and a
  documented first-game path.
- **Done when:** the production gate passes on a tagged release candidate and a release is cut.

## The Aether 1.0 production gate

Aether is "1.0" only when **all** of these hold, each checked by something automated where possible:

| Area | Requirement |
|---|---|
| **API stability** | Public APIs are marked stable / experimental / internal; stable ones are frozen with a deprecation policy |
| **Backwards compatibility** | Every serialized format is versioned with a migration path; a corpus of old projects keeps loading |
| **Performance benchmarks** | Baselines for the hot paths are checked in; regressions fail CI |
| **Memory budgets** | Per-system budgets are tracked and enforced; no per-frame allocation in hot paths |
| **Crash reporting** | Crash dumps are captured, symbolicated and triaged; the editor recovers without data loss |
| **Automated testing** | Unit, functional, screenshot, soak, network and fuzz suites run per platform and backend |
| **Documentation** | The manual, the generated API reference and the specs match the code (the manual and API-docs tests already enforce part of this) |
| **Examples and sample games** | At least three complete sample games build, run and ship from the SDK |
| **Packaged editor** | Signed, installable editor builds for each supported desktop platform |
| **Player/runtime** | Signed player builds for every supported platform, with the cook/pak pipeline |
| **SDK** | A published SDK a third party can build against from a clean machine |
| **Plugin system** | Versioned plugin ABI, packaging and a documented authoring path |
| **Release builds** | Reproducible release pipeline with checksums and release notes |

---

## 3. Cross-cutting concerns (every phase)

| Concern | Rule |
|---|---|
| **Testing** | Unit tests in `tests/` for all runtime code; headless editor/screenshot tests (existing `AETHER_EDITOR_SCREENSHOT`) for UI and rendering; benchmarks for hot paths |
| **Performance budgets** | 60 FPS at 1080p on a mid-range GPU for template projects; editor idle < 5% CPU (don't redraw an unchanged UI); scene load < 2 s for 10k entities |
| **Threading** | Game logic on the main thread plus scheduled parallel systems; rendering extraction then a render thread (introduced in Phase 14); asset loading on jobs |
| **Backwards compatibility** | Every serialized type is versioned (Phase 6); never break old projects without a migration |
| **Licensing** | Only permissive third-party code (MIT/BSD/zlib/Apache): Jolt, ImGui, imgui-node-editor, ImGuizmo, Luau, miniaudio, Recast/Detour, meshoptimizer, Tracy, ufbx, GameNetworkingSockets, Box2D |
| **Honest documentation** | Keep the README's current practice: record what was verified and how, including attempts that were reverted |

---

## 4. Step-by-step execution order (the short version)

Work in this order. Each line is roughly one PR-sized chunk, or a few.

1. ✅ Reflection core (`TypeInfo`, fields, `Any`) → port existing components.
2. ✅ JSON archive + versioned serialization → scenes saved as JSON.
3. ✅ Generic reflected Inspector + "Add Component".
4. Split `editor/main.cpp` into panels; ImGui docking branch; theme tokens. *(Deferred until the editor can be built and checked on Windows.)*
5. ✅ Undo/redo command stack + `EntityGuid`s.
6. ✅ Play-in-Editor (snapshot and restore); Pause/Step. Simulate mode is still to do.
7. Rotate/scale gizmos, snapping, multi-select, duplicate, copy/paste.
8. ✅ Project files (`.aproject`), project folder layout, recent projects. The editor's New/Open Project UI is still to do.
9. Asset GUIDs + `.ameta` + `AssetDatabase` + rename-safe references.
10. Importers → DDC → async loading → hot reload; Content Browser v2 with
    thumbnails.
11. Prefabs (instances, overrides, nesting, variants).
12. Gameplay framework: lifecycle events, `Camera`, tags/layers, timers,
    event bus, `SystemGraph` scheduler, fixed timestep.
13. Input actions and mapping contexts.
14. Luau host with auto-binding from reflection; script component; code
    editor panel; DAP debugger.
15. Native game module DLL + hot reload.
16. Graph widget (imgui-node-editor wrapper) shared by all graph editors.
17. Blueprint asset + data model + editor UI (no execution yet).
18. Blueprint compiler → bytecode → VM; events, functions, variables.
19. Latent nodes (Delay, Timeline), macros, interfaces, casting.
20. Blueprint debugger (breakpoints, wire flow, watches).
21. Physics v2: colliders, triggers, contacts → events, queries, character
    controller.
22. **Sample game #1 (coin platformer) built with Blueprints only.** This
    checks everything above.
23. Renderer extracted from demos; render graph on the RHI; light
    components; clustered lighting; CSM; post-processing.
24. Material graph → HLSL codegen; material instances.
25. Animation: clips, animator, state machine editor, blend spaces, IK,
    root motion.
26. Audio: miniaudio, sources, mixer, sound cues.
27. Runtime UI: widgets, layout, UI designer, widget Blueprints.
28. Particles: GPU sim, emitter modules, editor.
29. **Sample game #2 (third-person action demo).**
30. AI: navmesh, behavior trees, perception.
31. Terrain, foliage, splines, world streaming.
32. Networking: replication, RPCs, prediction, multi-client PIE.
33. Tracy profiling, console/CVars, debug draw, crash handler.
34. Linux + macOS (MoltenVK); CI matrix.
35. Cooker, pak files, standalone player, Package window.
36. Templates, plugin system, editor extensibility, 2D toolkit, docs.
37. Sequencer: tracks, CineCamera, movie render.
38. Save game system and world state persistence.
39. Localization: `LocText`, string tables, HarfBuzz shaping, dashboard.
40. Ability system (attributes, effects, tags, abilities), then inventory,
    dialogue and quest kits.
41. Accessibility pass across runtime UI, input, audio and the editor.
42. Advanced rendering: ray tracing, DDGI, volumetrics, upscalers, water.
43. XR (OpenXR), mobile (Android/iOS) and web (WebGPU).
44. Modding support, then optional AI-assisted editor tools.
45. **M7, after Phase 36 is finished: the Aether Upgrade.** Engine 2.0 core and API cleanup (37),
    performance with CI benchmark gates (38), Graphics 2.0 (39), the AI engine (40), Multiplayer
    2.0 (41), World Streaming 2.0 (42), Editor 2.0 (43), the AI-native editor over MCP (44),
    mobile and console (45), the developer ecosystem and SDK (46), production QA (47), then Aether 1.0
    behind the production gate (48). No further open-ended feature phases until the gate is passed.

---

## 5. Feature parity checklist vs. Unreal / Unity / Godot

| Feature | Unreal | Unity | Godot | Aether phase |
|---|---|---|---|---|
| Visual scripting | Blueprints | Visual Scripting | (removed in 4; plugins) | **12** |
| Text scripting | C++ (+ Verse) | C# | GDScript / C# | **11** (C++ + Luau) |
| Hot reload | Live Coding | Domain reload | Yes | **8, 11** |
| Prefabs / templates | BP classes, Level Instances | Prefabs + variants | Scenes | **9** |
| Docking editor | ✓ | ✓ | ✓ | **7** |
| Undo/redo | ✓ | ✓ | ✓ | **7** |
| Play in editor | ✓ | ✓ | ✓ (separate process) | **7** |
| Content browser + GUIDs | ✓ | ✓ (.meta) | ✓ (.import/uid) | **8** |
| Material graph | ✓ | Shader Graph | Visual Shader | **15** |
| Anim state machines | Anim BP | Animator | AnimationTree | **16** |
| Runtime UI designer | UMG | UI Toolkit / uGUI | Control nodes | **18** |
| Particles | Niagara | VFX Graph | GPUParticles | **19** |
| NavMesh + BT | ✓ | NavMesh (+ packages) | NavigationServer | **20** |
| Terrain | Landscape | Terrain | (plugins) | **21** |
| Networking | Replication | Netcode for GO | MultiplayerAPI | **22** |
| Profiler | Unreal Insights | Profiler | Profiler | **23** |
| Multi-platform packaging | ✓ | ✓ | ✓ | **24, 25** |
| 2D toolkit | Paper2D | 2D tools | ✓ (first-class) | **26** |
| Plugins / editor scripting | ✓ | ✓ | ✓ (`@tool`) | **26** |
| Cinematics | Sequencer | Timeline | AnimationPlayer | **27** |
| Save games | SaveGame | (custom / PlayerPrefs) | (custom / ConfigFile) | **28** |
| Localization | Localization Dashboard | Localization package | TranslationServer | **29** |
| Ability system | GAS | (packages) | (plugins) | **30** |
| Ray tracing / GI | Lumen, HW RT | HDRP RT | SDFGI | **31** |
| XR | OpenXR | XR Interaction Toolkit | OpenXR | **32** |
| Mobile / Web | ✓ / (Pixel Streaming) | ✓ / WebGL | ✓ / Web export | **33** |
| Modding | (per-game) | (per-game) | PCK loading | **34** |
| Accessibility | (partial) | (partial) | (partial) | **35** |
