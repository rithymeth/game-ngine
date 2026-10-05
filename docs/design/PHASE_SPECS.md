# Build specs for the critical-path phases

[ROADMAP_DETAILS.md §B](../ROADMAP_DETAILS.md) specifies Phase 6
(reflection). This document does the same for the next phases on the
critical path, at the level of algorithms, data structures, edge cases and
tests:

- **Phase 7:** undo/redo command stack, entity GUIDs, Play-in-Editor
- **Phase 8:** asset database, GUID resolution, hot reload
- **Phase 9:** prefab override resolution, system scheduling
- **Phase 11:** Luau binding and hot reload
- **Phase 13:** physics events and character movement

Each section ends with a PR breakdown. PRs are meant to be small enough to
review in one sitting.

---

## Phase 7: editor foundation

### 7.1 Entity GUIDs

Today an `Entity` (`engine/include/aether/ecs/entity.h`) is an index plus
a generation. That's right for runtime lookups, but it changes when an
entity is destroyed and recreated, which breaks undo, references in saved
files, and prefab links.

```cpp
struct EntityGuid { u64 hi, lo; };            // random v4 UUID, created once, saved with the entity

struct IdComponent { EntityGuid guid; };     // added to every editor-created entity

class GuidIndex {                            // lives next to World
public:
    Entity  Find(EntityGuid) const;          // O(1), unordered_map
    void    OnCreated(Entity, EntityGuid);
    void    OnDestroyed(Entity);
};
```

- References between entities in saved data (Parent, Blueprint variables,
  prefab overrides) store `EntityGuid`, and are resolved to `Entity` after
  load.
- Duplicating an entity gives it a new GUID. Undoing a delete restores the
  **same** GUID, so anything referring to it keeps working.

### 7.2 Command stack

```cpp
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Do(EditorContext&) = 0;
    virtual void Undo(EditorContext&) = 0;
    virtual const char* Label() const = 0;
    // Called when a new command arrives within the merge window of this one.
    virtual bool TryMerge(const ICommand& next) { return false; }
    virtual usize MemoryBytes() const { return sizeof(*this); }
};

class CommandStack {
public:
    void Execute(std::unique_ptr<ICommand> cmd);   // Do() then push; clears redo
    void Undo();
    void Redo();
    void BeginTransaction(const char* label);      // nested allowed; outermost wins the label
    void EndTransaction();                          // bundles everything since Begin into one entry
    void MarkSaved();                               // for the ● unsaved indicator
    bool IsDirty() const;
    void SetMemoryBudget(usize bytes);              // default 256 MB, drops oldest entries
};
```

Rules:

- **Merging:** consecutive `SetFieldCommand`s on the same entity, component
  and field within one drag (between `ImGui::IsItemActivated` and
  `IsItemDeactivatedAfterEdit`) merge into one entry. Gizmo drags work the
  same way.
- **Transactions:** actions made of several steps (Duplicate with
  children, Create Prefab, Paste) are wrapped in a transaction so one Ctrl+Z
  undoes all of it.
- **Selection** is saved in each entry (before and after), so undo
  restores it.
- **PIE:** the command stack is frozen during PIE. Edits during play go to
  the play world and aren't undoable (same as Unity and Unreal); a
  "Keep simulation changes" action creates normal commands afterwards.
- **Dirty tracking:** each entry records which scene or asset it touched,
  so only those get the unsaved `●`.

Built-in commands (all reflection-based, so they work for any component):

| Command | Stores |
|---|---|
| `SetFieldCommand` | entity GUID, component TypeId, field path, old/new value as serialized bytes |
| `CreateEntityCommand` | the serialized entity (all components), parent GUID, sibling index |
| `DestroyEntityCommand` | same, captured before destroying; recursive for children |
| `AddComponentCommand` / `RemoveComponentCommand` | entity GUID, TypeId, serialized component |
| `ReparentCommand` | entity GUID, old/new parent GUID, old/new sibling index, whether world transform was kept |
| `SetSelectionCommand` | only used inside transactions |

Test (from the roadmap): run 1,000 random commands from this table, then
undo all of them, and check the scene serializes byte-for-byte to the
original. Then redo all and compare with the snapshot taken after the
random run.

### 7.3 Play-in-Editor

```
          Edit World ──serialize──▶ memory buffer ──deserialize──▶ Play World
              ▲                                                     │
              │                  (edit world is untouched)          │ systems + scripts run
              └──────────── on Stop: destroy Play World ◀───────────┘
```

Steps when **Play** is pressed:

1. Finish any in-progress drag; freeze the command stack.
2. Serialize every loaded scene of the edit world into memory (the Phase 6
   binary archive, which is fast).
3. Create a new `World` + `PhysicsWorld`, deserialize into it, and rebuild
   runtime-only state (Jolt bodies from `RigidBody` settings, as
   `LoadScene` already does today; audio voices; script instances).
4. Map GUIDs: selection in the edit world is carried over to the same
   GUIDs in the play world, so the Inspector keeps showing the same object.
5. Fire `BeginPlay` on every entity, in hierarchy order (parents first).
6. The viewport switches to the game camera (or keeps the editor camera
   with **Simulate**).

When **Stop** is pressed: fire `EndPlay`, destroy the play world, restore
editor camera and selection, unfreeze the command stack.

Budgets: Play-start under 500 ms for a 10,000-entity scene; Stop under
100 ms. Measure both in a benchmark test.

Edge cases:

- Scripts that save to disk during PIE write to `Saved/PIE/`, not real
  save slots.
- Assets changed during PIE (hot reload) apply to both worlds.
- If the play world crashes a script, PIE stops with an error toast and
  the edit world is safe.

### 7.4 PR breakdown

1. ✅ **Done.** `EntityGuid`, `IdComponent`, `GuidIndex` with tests, plus
   custom JSON converters so GUIDs save as strings. The index is kept up to
   date explicitly (`Add`/`Remove`/`Rebuild`, and `EnsureGuid`/
   `RegenerateGuid` update it), not by hooks in `World`, and `Find` validates
   every entry against the live world.
2. ✅ **Done.** `CommandStack` with transactions and merging, plus the
   random-command test using test-only commands.
   - The editor calls `BreakMergeChain()` when an edit is committed, rather
     than the stack watching ImGui.
   - The random test compares a GUID-keyed snapshot, not raw scene bytes,
     because recreated entities get new handles and a new storage order.
3. ✅ **Done.** The built-in commands, with Inspector edits, spawn and delete
   wired through them, plus Ctrl+Z / Ctrl+Y and Edit menu entries. Physics
   side effects go through `EditorHooks`, so they also happen on undo and
   redo.
   - `ReparentCommand` and the `Parent` switch to `EntityGuid` move to
     step 4.
   - The legacy body-list sliders are also still direct edits, to be
     routed through commands in step 4.
4. ✅ **Done** (except selection in history). Gizmo drags recorded as one
   undo step, `ReparentCommand`, `Parent` switched to `EntityGuid`, and the
   hierarchy moved to `scene/hierarchy.h`. Selection isn't part of the
   history yet; it's cleared when undo removes the selected entity. That
   comes with a proper `EditorContext` selection model.
5. ✅ **Done** (except BeginPlay/EndPlay). Play/Stop with a byte-identical
   round-trip test, plus Pause/Step (originally planned for step 6).
   - Implemented as snapshot, then play in place, then restore on Stop,
     rather than a duplicated play world. The result is the same, and
     nothing holding a `World&` has to switch.
   - BeginPlay/EndPlay need scripting, so they wait for Phase 11.
6. Simulate, Pause, Step, Keep Simulation Changes.

---

## Phase 8: asset database and hot reload

### 8.1 Data structures

```cpp
struct AssetRecord {
    AssetGuid     guid;
    std::string   path;            // relative to Content/, forward slashes
    AssetTypeId   type;
    u64           source_hash;     // xxh3 of the source file
    u64           settings_hash;
    u32           importer_version;
    std::vector<AssetGuid> dependencies;   // what this asset references
    std::vector<AssetGuid> sub_assets;     // meshes/materials split out of a .gltf
};

class AssetDatabase {                       // editor only
public:
    void  Scan();                                    // at project open
    const AssetRecord* Find(AssetGuid) const;
    const AssetRecord* FindByPath(std::string_view) const;
    std::vector<AssetGuid> Referencers(AssetGuid) const;   // reverse dependency lookup
    bool  Move(AssetGuid, std::string_view new_path);      // also moves the .ameta
    bool  Delete(AssetGuid, bool force);                    // refuses if referenced unless forced
    void  Reimport(AssetGuid);
};
```

The runtime (player) doesn't use `AssetDatabase`; it reads a GUID → pak
offset table built by the cooker (Phase 25).

### 8.2 Scan at project open

1. Walk `Content/` recursively.
2. For each source file without an `.ameta`, create one with a new GUID and
   default importer settings, and queue an import.
3. For each `.ameta` whose source is missing: mark the asset **missing**
   (shown with a red icon), don't delete the `.ameta` (the file may come
   back, for example on a git branch switch).
4. For each asset, compute the DDC key; import only on a DDC miss.
5. Detect **duplicate GUIDs** (usually from copying files in Explorer
   with their `.ameta`): keep the older one, give the newer one a fresh GUID,
   and log a warning.

### 8.3 File watcher and hot reload

- Windows: `ReadDirectoryChangesW` on `Content/` with overlapped I/O on a
  background thread. Linux: `inotify`. Events are **debounced for 200 ms**
  per file, because editors often save in several steps (write temp,
  rename).
- On change: re-hash the source; if the hash changed, reimport on a job;
  when it finishes, swap the data on the main thread at the start of the
  next frame.
- **Handles:** `AssetHandle<T>` points to a slot with `{T* data, u32
  generation}`. Hot reload replaces `data` and bumps the generation.
  Systems that cache GPU objects compare the generation and rebuild.
- Dependency cascade: when a texture reloads, materials using it rebuild
  their bindless indices; when a shader reloads, pipelines using it
  recompile.
- Renames and moves made **outside** the editor show up as a delete plus a
  create. The watcher pairs them by matching the `.ameta` GUID so the move
  is treated as a move, not a new asset.

### 8.4 Migrating today's path-based references

`ModelRenderer` stores an `asset_path` string today. A one-time migration
runs when a project is opened for the first time with the new engine:

1. Scan and create `.ameta` files (as above).
2. Load each scene with the legacy loader, and replace every
   `asset_path` with the matching `AssetRef` GUID.
3. Save scenes in the new format, and keep a backup of the old files in
   `Saved/MigrationBackup/`.

### 8.5 PR breakdown

1. ✅ **Done.** `AssetGuid`, `.ameta` read/write, `AssetDatabase::Scan` with
   tests, plus `Move` and `MarkImported`. One rule was added: an unreadable
   `.ameta` is never overwritten; its asset is skipped with a warning.
   Referencers and delete-with-reference-check come with `AssetRef` in
   step 2.
2. ✅ **Done.** `AssetRef<T>` reflected type, resolution, and migration of
   `ModelRenderer`, plus dependency tracking (`Referencers`) and
   delete-with-reference-check.
   - The migration keeps the path alongside the GUID. The Windows-only
     renderer still loads by path, and `ResolveModelAssets` keeps the path
     current.
   - Dependencies come from scanning scene and prefab files for asset GUID
     strings, not from importer output.
3. ✅ **Done.** Importer interface and the texture importer with DDC.
   - Output is uncompressed RGBA8 with gamma-correct mips. BC7 compression
     needs an encoder, so it's deferred.
   - Importers run synchronously for now; job-system async loading comes
     with hot reload in step 5.
4. ✅ **Done.** glTF importer split into sub-assets (meshes, materials,
   animations).
   - Sub-asset GUIDs live in the source's `.ameta` (`sub_assets`), keyed by
     `mesh:<i>` etc., so they're stable across reimports.
   - Material texture references are content-relative paths for now.
     Turning them into `AssetRef<Texture>` needs the database at import
     time, which comes with the Content Browser (step 6).
   - Meshes use a raw binary format (`AMSH`); the rest use reflection.
5. ✅ **Done** (engine side). File watcher, debounce, and hot reload for
   textures, then meshes, then shaders.
   - The watcher polls (mtime + size) instead of using `ReadDirectoryChangesW`
     or `inotify`; native backends can replace the walk behind the same
     interface.
   - `HotReloader` reimports synchronously in `Update()`; worker jobs come
     with the job system.
   - `AssetHandle<T>`/`AssetStore<T>` implement the slot + generation
     design above. Swapping renderer resources (textures, meshes, shaders)
     on reload is editor work pending the Windows build.
6. ✅ **Done** (engine side). Content Browser v2: thumbnails, rename/move with
   reference safety, Reference Viewer.
   - `assets/content_browser.h` holds the listing, search, rename/move,
     reference graph and thumbnails; the ImGui panel is pending the Windows
     build.
   - Reference safety: GUID references need nothing; the relative URIs
     inside `.gltf` files are rewritten on move, and a `.gltf` now depends on
     the textures it names.
   - Thumbnails cover textures and materials. Model and mesh thumbnails need
     an offscreen render, so they come with the renderer-side editor work.

---

## Phase 9: prefabs and scheduling

### 9.1 Prefab data

A prefab asset is a small scene: a list of entities, each with a
**local ID** (unique only inside that prefab) and components.

```cpp
struct PrefabInstance {                      // component on the instance's root entity
    AssetRef<Prefab> source;
    std::vector<PropertyOverride> overrides;
    std::vector<LocalId> removed_entities;   // children deleted in this instance
    std::vector<AddedEntity> added_entities; // children added in this instance
};

struct PropertyOverride {
    LocalId     entity;      // which entity inside the prefab
    TypeId      component;
    std::string field_path;  // "position", "materials[2].tint"
    Bytes       value;       // serialized with the Phase 6 archive
};
```

Each spawned entity from a prefab gets a `PrefabLink { instance root,
LocalId }` component so the editor knows where it came from.

### 9.2 Resolving an instance (at load and after prefab edits)

```
Resolve(instance):
  1. data = deep copy of source prefab's entities
     (if source is itself a variant or contains nested prefabs,
      resolve those first, recursively, with cycle detection)
  2. remove entities listed in removed_entities
  3. apply each PropertyOverride to data[entity][component][field_path]
     - if the field no longer exists: keep the override, mark it "orphaned",
       show a warning in Messages (don't delete user data silently)
  4. append added_entities
  5. create/update real entities in the World, reusing existing Entities
     matched by (instance root, LocalId) so references stay valid
```

Order of precedence: nested prefab defaults → outer prefab's overrides of
the nested prefab → variant overrides → instance overrides. The innermost
default loses; the most specific override wins.

### 9.3 Recording overrides

When a field on a prefab-linked entity is edited, the `SetFieldCommand`
also adds or updates a `PropertyOverride`. If the new value equals the
prefab's value, the override is removed instead (so reverting by hand
removes the bold marking).

**Apply to Prefab** writes the override's value into the source prefab
asset and removes the override from this instance. Other instances that
override the same field keep their own values. **Revert** removes the
override and resolves again.

### 9.4 Propagation

When a prefab asset is saved, every loaded instance of it is re-resolved
(step 5 above reuses entities, so selection and references survive). A
change that breaks instances (for example, deleting an entity that
instances override) produces warnings listing the affected scenes.

### 9.5 Tests

- Override one field on 1 of 100 instances, change that field in the
  prefab: 99 update, 1 keeps its value.
- Nested prefab 3 deep: override at each level, check precedence.
- Variant of a variant: check precedence and cycle detection
  (prefab A containing prefab B that contains A is rejected with an error).
- Rename a field in C++ with a migration hook: overrides migrate too.
- Removed and added children survive save/load and prefab edits.

### 9.6 System scheduling

```cpp
struct SystemDesc {
    const char* name;
    Phase phase;                        // PreUpdate, FixedUpdate, Update, LateUpdate, PreRender
    ComponentMask reads, writes;        // declared access
    std::vector<const char*> after;     // explicit ordering, by name
    void (*run)(World&, const FrameContext&);
    bool main_thread_only = false;      // e.g. anything touching ImGui or the D3D12 device
};
```

- Within a phase, build a DAG: an edge A → B if A is listed in B's `after`,
  or if both touch the same component and at least one writes it (ordered
  by registration to break ties, deterministically).
- Run the DAG on the job system: a system starts when all its
  predecessors finish. Systems with non-overlapping access run in
  parallel.
- Blueprint and Luau Tick run as `main_thread_only` systems in phase 2 of
  this plan; making them parallel is future work.
- Fixed timestep: an accumulator runs `FixedUpdate` 0–N times per frame
  (capped at 5 to avoid a spiral of death), and render interpolates
  transforms with `alpha = accumulator / fixed_dt`.

### 9.7 PR breakdown

1. ✅ **Done.** Prefab asset save/load and `PrefabInstance` resolve (no nesting).
   - Override values are JSON (with JSON-structure field paths such as
     `items[2]` and `position[1]`), not archive bytes: prefab data is
     already reflected JSON, and JSON diffs and merges well.
   - Added children need no `added_entities` list: they're ordinary scene
     entities parented under the instance, and resolving leaves unlinked
     entities alone.
   - The instance root keeps its own Transform (its placement); its other
     components follow the prefab.
2. ✅ **Done** (except the Apply button). Override recording, bold UI, Apply/Revert.
   - Recording diffs the edited component against the prefab (per leaf
     field), so it happens on undo and redo too and reverting by hand
     clears the override.
   - "Bold" is an accent bar plus accent-coloured label: ImGui's default
     font has no bold face.
   - `ApplyOverridesToPrefab` is done; its editor button waits for the
     Windows editor's asset saving.
3. ✅ **Done.** Nesting, variants, cycle detection.
   - `FlattenPrefab` turns nesting and variants into flat data, then the
     step 1 resolve runs unchanged. Nested entities get path-derived stable
     IDs (`NestedLocalId`), so overrides can address them.
   - Apply to Prefab writes into a variant's overrides. For entities
     inside nested prefabs it's still to do.
   - The migration-hook test (§9.5) comes with propagation in step 4.
4. ✅ **Done.** Propagation on prefab save and orphaned override warnings.
   - `PrefabLibrary`, `PropagatePrefabChange` and `CheckScenesUsingPrefab`
     (the affected-scenes warning list); showing it in the Messages panel
     is Windows editor work.
   - Overrides carry their component's schema version and are migrated
     with it (the §9.5 migration test), by running the type's migration
     hook on the single overridden field.
5. ✅ **Done.** Gameplay framework components (Camera, Tags, Layers) and lifecycle
   events.
   - Also an `Active` component, since OnEnable/OnDisable need something
     to enable and disable.
   - `Lifecycle` dispatches per registered component type (scripts and
     Blueprints register theirs in Phases 11-12). Changes made in callbacks
     are deferred to the end of the pass.
   - Tags are compared by name. Hashed tags, SpringArm, timers and the
     event bus come with the gameplay work that needs them.
6. ✅ **Done.** `SystemDesc` scheduler and fixed timestep with interpolation.
   - Phases run as levels of the DAG (each level in parallel on the job
     system). Starting each system as soon as its own predecessors finish
     can replace the level barrier later without changing the interface.
   - `ComponentMask` access and `std::function` run callbacks (the spec's
     function pointer is too narrow for systems with state).
   - Moving the existing editor and physics updates onto the scheduler is
     editor work pending the Windows build.

---

## Phase 10: input

### 10.1 Model

Devices feed a raw `InputState` (every `Key` across keyboard, mouse and
gamepad, as a value). `InputSystem::Update` then evaluates the context
stack against it each frame:

1. Contexts are evaluated from highest priority down. A context that
   `consume_input`s hides the keys it's using from lower ones, and
   `block_lower_contexts` hides everything from them. A binding that
   loses its input this way resets quietly, without firing Released or
   Completed.
2. Per binding: the raw value goes through the **modifiers** (dead zone,
   negate, swizzle, scale) into a Vec3, then the **triggers** (Down,
   Pressed, Released, Hold, Tap, DoubleTap, Chord). A binding with no
   triggers counts whenever its value is non-zero.
3. An action's phase is the strongest of its bindings' (None < Ongoing <
   Triggered). Its value is the sum of its actuated bindings' values, which
   is how WASD becomes one 2D axis.
4. Phase changes become events: Started, Ongoing, Triggered (every frame
   it's triggered), Completed, Canceled.

Chords see their chord action as evaluated so far this frame, or last
frame's state for an action in a lower context.

### 10.2 PR breakdown

1. ✅ **Done.** Keys and raw state, actions, mapping contexts, modifiers,
   triggers, the context stack, events and the polling API, all testable
   with synthetic events (the phase's "done when").
2. ✅ **Done.** Input assets (`InputAction` and `InputMappingContext` files through the
   asset database), runtime rebinding, "press a key" capture, and user
   overrides saved to config.
   - `.aaction` / `.amapping` files; bindings refer to actions by name.
   - Rebinds address "the Nth binding of an action in a context", and are
     saved in `Saved/Config/Input.json`, separate from the defaults.
3. Platform backends: Win32 keyboard and mouse (raw delta, cursor lock),
   XInput gamepads (Windows), and the SDL controller database elsewhere.
4. Editor: the Input Mapping Context editor, and Project Settings > Input.

---

## Phase 11: Luau binding and script hot reload

### 11.1 Binding generation from reflection

At startup, `LuauHost` walks `TypeRegistry` and creates, for each
reflected component type:

- a Luau **userdata metatable** named after the type (`Health`),
- `__index` / `__newindex` handlers that look up the field by name in a
  precomputed hash map and read/write through `FieldInfo` (with typed fast
  paths for float, int, bool and Vec3),
- methods for every `BlueprintCallable` `FunctionInfo`, called through the
  `invoke` thunk,
- event objects with `:Connect(fn)` returning a connection with
  `:Disconnect()`.

Values like `Vec3` and `Quat` are Luau **vector** or small userdata types
with math operators, so `a + b * 2` works in scripts.

Component references are **handles** (entity + TypeId + world
generation), never raw pointers. Using one after its entity is destroyed
raises a Luau error with a clear message instead of reading freed memory.

### 11.2 Script lifecycle

```
ScriptComponent added / PIE start
   → compile (cached bytecode by source hash)
   → run the module chunk → it returns a table (the "class")
   → create an instance table with the class as metatable
   → copy exposed variables from the component's PropertyBag
   → call :OnStart()
Every frame → :OnUpdate(dt) (only if the class defines it)
Destroy → :OnDestroy() → disconnect all event connections the instance made
```

Exposed variables: any top-level field of the class table with a
default value of a supported type (number, boolean, string, Vec3, an
asset or entity reference) appears in the Inspector. A `--@range 0 100`
comment above it adds metadata.

### 11.3 Hot reload during PIE

1. The file watcher reports a changed `.luau` file.
2. Compile it. If it fails: keep running the old version and show the
   error in the code editor and as a toast.
3. If it succeeds, for each live instance: copy fields of the old
   instance table that exist in the new class (by name), swap the
   metatable to the new class, and call `:OnReload()` if defined.
4. Event connections made by the old code stay connected (they refer to
   the instance table, which is kept). Scripts that need to reconnect can
   do it in `OnReload`.

### 11.4 Sandbox and limits

- Standard libraries allowed: `math`, `string`, `table`, `bit32`,
  `utf8`, `buffer`. Removed: `io`, `os` (except `os.clock`), `debug`
  (except in editor builds, for the debugger), `loadstring`.
- Luau's interrupt callback enforces a per-call instruction budget in
  editor builds, with the same error style as Blueprints (BP202).
- Memory: a per-VM allocator that tracks usage and reports it in the
  profiler.

### 11.5 PR breakdown

1. ✅ **Done.** Fetch Luau (MIT) via `FetchContent`, a `LuauHost` that runs a script
   file, tests.
   - The sandbox and limits of §11.4 came with it. The instruction budget
     counts interrupt checks, and the memory cap is enforced by the VM's
     allocator.
   - Values crossing C++/Luau are nil, bool, number and string for now.
     Engine types come with step 2.
2. ✅ **Done.** Reflection-driven bindings for fields and functions, with the typed
   fast paths and handle safety.
   - Component handles are (binding generation, entity, component id).
     Every access re-checks the entity is alive and still has the
     component. Re-binding the world invalidates all handles.
   - Functions are exposed whatever their flags. Filtering to
     `BlueprintCallable` can come with the exposure rules of step 3.
   - `Quaternion` and other structs are tables for now. Only `Vec3` is a
     native vector.
3. ✅ **Done.** `ScriptComponent`, lifecycle, exposed variables in the Inspector.
   - The "PropertyBag" is the component's list of overridden variables
     (JSON values). Variables the entity doesn't override follow the
     script's current default.
   - Classes are cached per script asset; recompiling on change is step 5
     (hot reload).
   - Inspector edits change the component in place. Recording them as
     undoable commands comes with the Windows editor work.
4. ✅ **Done** (physics queries with Phase 13). Events (`:Connect`) and Input/Physics/Timer APIs.
   - Physics reaches scripts through `SendEvent` (OnCollisionBegin and
     friends call script methods). Raycasts and overlaps come with Phase
     13's character and query work.
   - Script-made events (`Event.new`) live as long as the script system.
5. ✅ **Done.** Hot reload and the error overlay.
   - Instance state lives on the instance table, so swapping its metatable
     to the new class keeps it. No field copying is needed.
   - Entities whose script failed to load during play start when a reload
     succeeds.
   - The overlay is a portable ImGui widget; hooking it (and the toast)
     into the editor window is Windows editor work.
6. ✅ **Done** (portable side). Code editor panel with completion from reflection; DAP debugger.
   - ✅ **Part 1 done:** the engines behind them.
     - `ScriptDebugger` handles breakpoints, which survive reload and move
       to the next line with code, plus Into/Over/Out and Pause. Its stops
       report frames and locals, printed without running script code.
     - `CompleteScript` does text-based completion from the script API and
       reflection.
   - ✅ **Part 2 done:** the DAP server (`DapFraming`, `DapSession`).
     - It doesn't depend on a transport. The socket listener comes with the
       Windows editor hookup.
     - `evaluate` covers locals only.
   - ✅ **Part 3 done:** the code editor panel.
     - `CodeDocument` is the model: auto-indent, undo, find and Luau
       tokens.
     - `DrawCodeEditor` is the portable ImGui widget, with a breakpoint
       gutter, execution and error lines, and a completion popup from a
       callback.
     - Docking it into the editor window and wiring completion, the
       debugger and a DAP socket are Windows editor work.

---

## Phase 12: Blueprints (visual scripting)

The concepts, VM design and UI are in [ROADMAP.md Phase 12](../ROADMAP.md),
[ROADMAP_DETAILS.md §A.4, §C-D](../ROADMAP_DETAILS.md), and the node
reference in [BLUEPRINT_NODES.md](BLUEPRINT_NODES.md). This section is the
build plan.

### 12.1 Library layout

`blueprint/` builds `Aether::Blueprint`. It depends only on the engine, so
it builds and tests headless everywhere. The editor's graph widget lives in
`editor/src/graph/` (portable ImGui, like the other editor UI).

### 12.2 Graph data (step 1)

```cpp
struct PinType  { PinKind kind; ValueType type; const reflect::TypeInfo* reflected; bool is_array; };
struct PinDesc  { std::string name; PinDir dir; PinType type; Value default_value; u32 flags; };
struct Node     { NodeId id; std::string type; float x, y; json config; json defaults; std::string comment; };
struct Link     { PinRef from; PinRef to; };          // PinRef = {node id, pin name}
struct Graph    { std::string name; GraphKind kind; std::vector<Node> nodes; std::vector<Link> links; };
struct Blueprint{ std::string parent; std::vector<Variable> variables; std::vector<Graph> graphs; };
```

- **Pins aren't saved.** A node stores its type ID, its config and the
  defaults of any inputs the user changed. Its pins come from
  `ResolveNode(blueprint, node)`, which asks the node type (given the
  config and the Blueprint's variables and functions). So renaming a
  variable's type, or changing the number of `Sequence` outputs, never
  leaves stale pins in the file.
- **Node type IDs are stable strings**, as in §A.4:
  - hand-written: `Flow.Branch`
  - one per variable: `Var.Get:IsOpen`
  - one per reflected function: `Call.Native:Health.Heal`
  - one per operand type: `Math.Add:float`

  The registry resolves the families by prefix.
- **`.abp` files** are JSON in the §A.4 shape. Links are written as
  `{"from": [1, "then"], "to": [2, "exec"]}`.
- **Validation** covers what can be checked on the graph alone.
  - Errors:
    - unknown node types and pins
    - wrong direction
    - BP001 type mismatch (implicit int→float, anything→string for
      Print, and Wildcard are allowed)
    - more than one link into a data input, or out of an exec output
    - BP003 pure loops
    - BP005 deleted variables
    - BP006 duplicate events
  - Warnings:
    - BP101 unconnected inputs that ask for it (Branch's condition)
  - Each diagnostic names its node and pin.

### 12.3 PR breakdown

1. ✅ **Done.** Graph model, node signatures (core library: events, flow,
   variables, math, literals, print, reflected native calls), `.abp`
   save/load, validation, and a `GraphBuilder` for tests.
   - Validation added five catalog codes: BP007 (unknown type or bad
     config), BP008 (pins and defaults), BP009 (too many links), BP010
     (an event outside the Event Graph) and BP011 (a function's entry
     count).
   - Custom node types register through `RegisterNodeType` and
     `RegisterNodeFamily`, the §D mechanism, with the same factories the
     built-ins use.
2. ✅ **Done.** Compiler and VM: lower to the §C.2 register bytecode, typed
   math opcodes, pure nodes cached per exec step, native calls through
   `FunctionInfo`, dispatch of BeginPlay, Tick and custom events, the BP202
   instruction budget, and a golden bytecode test for BP_Door.
   - **Registers**: two banks per frame, 16-byte value registers and
     string registers. Frames are kept per call depth and reused, so a
     dispatch allocates nothing (except native calls that box arguments
     in `Any`).
   - **Exec flow**:
     - An exec wire back into a node already on the path compiles to a
       jump, so loops are guarded by the budget.
     - A node reached by two paths is emitted once per path. This is
       correct because what runs after it can differ between paths.
   - **Calls**: functions and custom events are called with a new frame,
     and BP203 limits the depth.
   - **Unsupported types**: structs and arrays in compiled code are
     refused with BP012 until step 4.
   - **Instances**: they're attached to entities through
     `BlueprintVM::Attach`. The `BlueprintInstance` component and play
     start come with step 3's lifecycle integration.
3. ✅ **Done.** Latent actions (Delay, Retriggerable Delay, Delay Until
   Next Tick), the stateful flow nodes (Do Once, Gate, Do N, Flip Flop),
   the latent action manager, the `BlueprintInstance` component, and
   lifecycle integration.
   - **Node state**: each stateful or latent node gets a per-instance
     state slot, shared by the event functions of its graph. That's how
     a Gate opened by one event affects another.
   - **Latent resume**: a latent node compiles to `LATENT`, and
     execution carries straight on (a Sequence runs its next output).
     The node's "completed" code is compiled once per node, after the
     function's body, and the saved frame resumes there.
   - **Where it lives**: the latent action manager is part of
     `BlueprintVM`. `Tick` advances game time, resumes due actions in
     wake order, then runs Event Tick.
   - **Lifecycle**: `BlueprintSystem` maps create, start,
     enable/disable and destroy to attach, BeginPlay, pause/resume and
     EndPlay. Ticking stays one `Update(dt)` per frame, not per-entity
     lifecycle updates, so latent timers advance once per frame.
4. The rest of the v1 node library, in parts:
   - ✅ **Part 1 done:** loops, switches, select, strings, Format Text,
     conversions, and more math.
     - The nodes: For Loop and For Loop with Break, While Loop, Switch on
       Int and on String, and Select (per type, 2 to 16 options).
     - Strings and Format Text: Append (+pins), Length, Is Empty,
       Contains (with ignore case), To Upper/Lower, Trim, String to
       Int/Float (with success), and Format Text with `{name}`
       placeholders and `{{ }}` escapes.
     - Conversions: `Conv.ToString:<type>`.
     - More math: trig, sqrt/exp/log/pow, Floor/Ceil/Round/Truncate to
       int, int variants, Nearly Equal, Map Range Clamped, and seeded
       random numbers.
     - Loops compile to plain jumps, not calls. The loop body's
       registers stay live, so the index pin reads directly.
   - ✅ **Part 2 done:** arrays and For Each.
     - Storage: a third register bank of arrays, and array variables per
       instance.
     - Pure nodes: Make, Length, Last Index, Get, Is Valid Index, Find and
       Contains.
     - Modifying nodes, which edit an array variable in place: Add, Add
       Unique, Insert, Remove Index, Remove Item, Clear, Set Array Elem
       and Reverse. Their array pin must come from a Get node (BP013,
       Unreal's by-reference pins).
     - For Each iterates a copy.
     - An out-of-range Get returns the default (with BP205), and an
       out-of-range remove does nothing.
     - Sort, Shuffle and Filter need comparator functions, so they wait
       for part 4. So do arrays in native calls and Dispatch arguments.
   - ✅ **Part 3 done:** entity and world nodes.
     - Transforms: Get/Set Location and Rotation, Add Offset and Get World
       Location. Rotation helpers: Rotate Vector, Rotation from Axis and
       Angle, and Combine Rotations.
     - Tags: Has/Add/Remove Tag and Find Entities with Tag.
     - Hierarchy: Get Parent, Attach To (refusing cycles) and Detach.
     - Lifetime: Destroy Entity and Spawn Blueprint.
     - Time: Game Time and Delta Seconds.
     - **Destroy** is deferred until the running event finishes. Then
       EndPlay runs, and the entity is destroyed through the lifecycle
       when a `BlueprintSystem` is set up.
     - **Spawn** goes through a spawner hook. `BlueprintSystem` creates the
       entity with an ID, a Transform and a `BlueprintInstance`, and
       attaches it straight away. BeginPlay comes at the lifecycle's next
       sync.
     - **Not yet**: Transform has no scale, so there are no scale nodes.
       Expose-on-spawn pins come with part 4.
   - ✅ **Part 4 done:** event dispatchers and interfaces.
     - **Dispatchers** are declared on the Blueprint (`dispatchers` in
       the .abp). Call is on this Blueprint's own dispatchers.
       Bind/Unbind/Unbind All use a Custom Event handler (config `event`),
       and go by dispatcher name, so a listener can bind to another
       class's dispatcher. When the listener declares a dispatcher of that
       name, the handler's parameters must match (BP014).
     - **Bindings** live in the VM, per (target, dispatcher), and go away
       with either side.
     - **Interfaces** come from a registry (`RegisterBlueprintInterface`).
       A Blueprint lists the ones it implements (`interfaces`) and
       handles `Event.Interface:I.F` events (BP015 if unlisted).
     - `Interface.Call` does nothing on non-implementers, and
       `Interface.Implements` answers Does Implement.
     - Interface functions have no return values yet.
   - ✅ **Part 5 done:** macros.
     - **Signatures**: macro graphs have `Macro.Inputs`/`Macro.Outputs`
       tunnels, and their signatures may have exec pins (several in and
       several out).
     - **Inlining**: `CompileBlueprint` inlines every `Macro:Name`
       instance, nested ones too, before compiling.
       - Each copy has fresh node IDs, so stateful and latent nodes keep
         separate state per instance.
       - An unconnected macro input feeds its value (the instance's
         default, else the macro's) through a literal node, so
         conversions still apply.
     - **Checks**:
       - The expanded graphs are validated again, and problems there
         (a latent node inlined into a function, say) are reported on the
         instance node.
       - Macros containing themselves are refused with BP016.
   - ✅ **Part 6 done:** Sort/Filter with comparator functions, and
     expose-on-spawn pins. Step 4 is complete.
     - **Sort** sorts an array variable in place. It uses the natural order
       for ints, floats and strings, or a function of the Blueprint
       (config `by`, (a, b) -> bool "a comes before b").
       - The merge sort is written by hand, so an inconsistent Blueprint
         comparator can't hit `std::sort`'s undefined behaviour. It is
         stable.
     - **Filter** is pure and keeps the items a predicate (item) -> bool
       accepts.
     - **Errors**: a function with the wrong signature is refused with
       BP017, a missing one with BP004, and sorting a type with no natural
       order without a function with BP007.
     - **Expose on Spawn**: the Spawn node's config `expose` lists the
       spawned Blueprint's variables (the editor fills it in). Each gets an
       input pin. The values reach the spawner as JSON and are applied at
       attach, before BeginPlay. Only variables marked `ExposeOnSpawn` (or
       instance-editable) take them.
5. ✅ **Done.** Debugger: node breakpoints, stepping and call stack, exec
   trace for wire animation, watched pin values, and an instance filter.
   - **Debug info from the compiler**: each function records where each
     node's code starts (`entries`; several nodes can share a pc, such as
     a Sequence with no code of its own, or an impure node and its pure
     inputs). It also records the register holding each pin's value
     (`pin_values`).
   - **The hook**: while a handler or the trace is on, the VM checks one
     flag per instruction and runs the hook at node starts. A loop
     re-entering a node counts as a new start.
   - **Stepping**: Step Over/Into/Out compare call depths, as the Luau
     debugger does. A stop reports the call stack, innermost first, with
     every pin's current value per frame.
   - **Wire animation**: the exec trace records `{entity, graph, node,
     frame}` in a capped buffer the editor takes each frame.
   - **Watches**: a watch is a pin value read at a stop. Continuous
     watches (last value on hover while running) come with the editor.
6. Graph editor widget and Blueprint editor panels (portable ImGui):
   palette, pin colors, links, comments and compile results.
   - ✅ **Part 1 done:** the graph widget and its Blueprint adapter.
     - **Widget** (`editor/src/graph/graph_view.h`): generic, knows
       nothing about Blueprints. It draws a view model (nodes, pins,
       wires) and reports edits for the owner to apply. Pan, zoom about
       the cursor, node drag, wire drag, box select, Alt+click to break a
       wire, the palette (right-click, or a wire dropped on empty
       canvas), Delete, Home (fit) and Ctrl+A. Hit testing is public
       for tests.
     - **Blueprint adapter** (`blueprint_graph.h`): pin colors from
       BLUEPRINT_NODES.md, header colors by category, compile errors on
       their nodes, the debugger's current node, breakpoints and glowing
       wires from the exec trace.
     - **Edits**: links are checked with `CanConnect`. Linking a data
       input or an exec output replaces its old link. A node placed from
       a dragged wire links its first fitting pin.
     - **Palette search** is fuzzy, prefix matches first, and filtered to
       nodes with a pin fitting the dragged one.
   - ✅ **Part 2 done:** the Blueprint editor panels. Step 6 is complete.
     - **Document** (`blueprint_document.h`, no ImGui): the open
       Blueprint, its file, undo/redo as whole-Blueprint snapshots (edits
       with the same merge key, like typing in one field, share a step),
       and the compile status (not compiled, OK, warnings, errors, stale).
     - **My Blueprint edits**: add, rename and remove variables,
       functions, macros and event dispatchers. Renames update what refers
       to them (Get/Set, calls, macro instances, Sort/Filter `by`,
       dispatcher nodes). Removals take those nodes with them. A variable
       type change breaks links that no longer fit.
     - **Clipboard**: copy, cut, paste at the mouse and duplicate, as JSON
       with the links between copied nodes.
     - **Comment boxes** are part of the graph (`comments` in the .abp).
       C boxes the selection; dragging the title moves the box and the
       nodes inside it, and the corner resizes it.
     - **Editor** (`blueprint_editor.h`): toolbar (Compile with its status,
       Save, Undo/Redo, parent class), My Blueprint, graph tabs, Details
       (variables, function and macro signatures, dispatcher parameters,
       node comments and pin defaults, comment text and color), Compiler
       Results (a row focuses its node) and the palette popup. Keys:
       Ctrl+Z/Y/S, F7, and in the graph Tab, F, C and Ctrl+C/X/V/D.
     - **Not yet**: the Class Defaults and Components tabs, the diff view,
       the minimap and bookmarks, reroute nodes, and collapse to function.
7. ✅ **Done.** Samples (BP_Door, BP_Coin, BP_GameMode) and the
   10,000-instance benchmark. Phase 12 is complete on the portable side.
   - **Samples** in `assets/blueprints/`, in the editor's saved form (a
     load and save gives the same JSON), each with comment boxes:
     - `BP_Door` opens while an entity tagged Player is in its trigger,
       swinging at `Speed` degrees per second up to `OpenAngle`, and
       closes when it leaves. There's no Timeline node yet, so Tick moves
       the angle.
     - `BP_Coin` checks for the player, then (Do Once) calls its
       `OnCollected(value)` dispatcher and destroys itself.
     - `BP_GameMode` binds `AddScore` to every entity tagged Coin at
       BeginPlay, prints the score, and prints "You win" once at `Goal`.
     - `BP_Spinner` is the benchmark's 20-node Tick: spin, bob, count laps.
   - **Trigger events** are dispatched by the tests for now; Phase 13's
     contact events will send them.
   - **Benchmark**: `aether_bp_bench [--count N] [--frames N] [--check MS]`
     times `BlueprintVM::Tick`. 10,000 spinners take about 1.4 ms a frame
     on the development container (target: under 2 ms).
     - Getting there fixed a quadratic cost: after every dispatch the VM
       scanned all instances for ones detached while running. It now
       keeps a list of those.
     - Tick also calls the Tick function directly, with no string
       lookup per instance.

---

## Phase 13: physics events and character movement

### 13.1 Contact and trigger events

Jolt calls its `ContactListener` from **worker threads** during
`PhysicsSystem::Update` (which already runs on `aether::JobSystem` through
`JoltJobSystemAdapter`). Gameplay code must not run there.

```
during Step (worker threads):
   OnContactAdded / Persisted / Removed
      → push {body A, body B, type, point, normal, impulse} into a
        per-thread queue (lock-free, no allocation: fixed-capacity ring,
        overflow counted and logged)
after Step (main thread):
   merge queues, sort by (body A, body B, type) for deterministic order,
   map BodyID → Entity, and dispatch:
      OnCollisionBegin / Stay / End  (solid bodies)
      OnTriggerEnter / Exit          (if either body is a sensor)
```

- **Stay** events are off by default (they fire every step); enable them
  per collider.
- Events for an entity destroyed earlier in the same dispatch are skipped.
- Removal events for bodies that were destroyed are still delivered as
  End/Exit, so scripts can clean up.

### 13.2 Queries

`PhysicsWorld::RayCast(ray, max_distance, LayerMask, ignore)`,
`ShapeCast(shape, from, to, ...)`, and `Overlap(shape, transform, ...)`
wrap Jolt's `NarrowPhaseQuery` with `BroadPhaseLayerFilter` and
`ObjectLayerFilter` built from the project's 32-layer collision matrix.
Queries are safe to call from gameplay code at any time outside `Step`.

### 13.3 Character movement

`CharacterMovement` wraps Jolt's `CharacterVirtual`:

| Setting | Default |
|---|---|
| capsule radius / height | 0.35 m / 1.8 m |
| walk / run speed | 4 / 7 m/s |
| acceleration / braking | 20 / 25 m/s² on the ground, 5 m/s² in the air (air control 0.3) |
| jump velocity | 5 m/s; coyote time 0.1 s; jump buffer 0.1 s |
| max slope | 45° |
| step height | 0.35 m |
| gravity scale | 1.0 |

Each fixed step: read queued input (`AddInput(vector)`, `Jump()`), update
velocity, call `CharacterVirtual::ExtendedUpdate` (which handles stairs and
sticking to the floor), then write the result to `Transform`. Moving
platforms are handled by inheriting the ground body's velocity. Events:
`OnLanded`, `OnJumped`, `OnMovementModeChanged` (walking, falling,
flying, swimming).

### 13.4 PR breakdown

1. ✅ **Done.** Collider components (box, sphere, capsule, convex, mesh)
   and the `RigidBody` split, with the scene migration.
   - **Colliders** each have a center offset, a trigger flag (the body
     becomes a Jolt sensor), friction and restitution. Several on one
     entity make a static compound shape. A mesh collider keeps its
     triangles only on static bodies; moving ones get the convex hull of
     its vertices, as in Unity and Unreal.
   - **`RigidBody`** is motion only (schema version 2): Static, Kinematic
     or Dynamic, mass, damping, gravity scale, CCD and rotation locks. An
     entity with colliders and no RigidBody is static.
   - **Migration**:
     - Old binary payloads (radius, mass, is_static: 9 bytes) still load;
       new ones start with a magic tag and hold the reflected binary
       form.
     - Version-1 JSON migrates through the reflection hook (is_static
       becomes motion; radius becomes `legacy_radius`).
     - `MigrateLegacyRigidBodies(World&)` then gives each such entity a
       SphereCollider. The editor calls it after loading a scene.
   - **`PhysicsScene`** keeps a PhysicsWorld in step with the ECS:
     - creates, rebuilds (when collider or RigidBody settings change)
       and destroys bodies;
     - pushes gameplay's Transform changes in (kinematic bodies are moved
       over the step, others teleported), and writes dynamic bodies
       back;
     - records why a collider couldn't make a body, such as a degenerate
       hull;
     - stores each body's entity in its user data, for step 3's events.
   - **Editor**: `editor/main.cpp` now makes its spheres from RigidBody +
     SphereCollider and migrates old scenes. It hasn't been compiled on
     Windows yet; moving it onto PhysicsScene waits with the other window
     hookups.
2. ✅ **Done.** Layers and the collision matrix.
   - **Settings**: `ProjectSettings::collision_matrix` stores, per layer,
     the mask of layers it collides with. A missing row means "all", so
     older projects and untouched layers stay empty in the file.
   - **API**: `SetLayersCollide(settings, a, b, collide)` edits both
     directions, and `MakeCollisionMatrix` reads it. A lopsided file
     collides only where both rows agree.
   - **Jolt object layers** are `layer * 2 + moving`: 64 of them over two
     broadphase trees (non-moving and moving). The pair filter reads the
     matrix, and two non-moving bodies never collide.
     `PhysicsWorld::SetCollisionMatrix` swaps the matrix at runtime; pairs
     the broadphase finds from then on follow it.
   - **Bodies**: `PhysicsScene` puts each body on its entity's `Layer`,
     and rebuilds the body when the layer changes.
   - **Debug fix**: the broadphase interface now implements
     `GetBroadPhaseLayerName`, which Jolt's profiling builds need. Debug
     builds with physics failed to compile without it.
3. ✅ **Done.** Contact listener queue and event dispatch, with a
   determinism test (same scene, same events in the same order across 10
   runs).
   - **Queue**: Jolt's callbacks (worker threads) claim slots in a
     16,384-entry buffer with one atomic counter. There's no lock and no
     allocation; overflow is counted (`DroppedContacts`) and logged.
     Persisted callbacks are kept only for bodies that asked for Stay.
   - **After the step**, `PhysicsWorld` sorts the entries by (body pair,
     sub-shapes, kind) and counts sub-shape contacts per body pair, so a
     compound touching with two parts begins once. The result is
     `ContactEvent`s: Begin with point, normal and approach speed; Stay
     at most once per pair per step; End.
   - **Sleep**: Jolt reports a sleeping pair's contacts as removed and
     re-adds them on waking. A pair whose bodies are all asleep turns
     dormant instead of ending. It ends only if a body wakes without
     touching, or goes away, so a resting pile doesn't flicker
     Begin/End.
   - **`PhysicsScene`** maps bodies to entities and delivers
     `PhysicsEvent{self, other, type, point, normal, approach_speed}` to
     both sides through `SetEventHandler`. Types: CollisionBegin/Stay/End,
     or TriggerEnter/Exit when either body is a sensor; triggers never
     Stay. The normal points at `self`.
     - A destroyed body's entity is remembered until its End goes out,
       so the survivor hears it, with the gone entity as `other`.
     - An event whose `self` was destroyed earlier in the same dispatch
       is skipped.
   - **Stay** is opt-in per collider (`report_stay`). `PhysicsEventName`
     gives the Blueprint event, and the Event OnCollisionStay node was
     added.
   - **Fix** found by the determinism test's repeated worlds:
     `JoltJobSystemAdapter`'s destructor now waits for the wrappers it
     scheduled. A worker releases its job after marking it done, so a
     world destroyed right after `Update` could free the job pool under
     it. That tripped a Jolt assertion in Debug builds. Jolt's asserts
     now log the failed expression.
   - **Not yet**: the impulse Jolt computes during solving isn't
     reported (approach speed stands in, as Jolt suggests for impact
     sounds). Events go to Blueprints through a handler the game sets up
     (the test shows one line); the player loop wires it with the other
     hookups.
4. ✅ **Done.** Queries with Blueprint nodes.
   - **`PhysicsWorld`**: `RayCast`, `ShapeCast` (any Jolt shape, with a
     rotation), `SphereCast`, `Overlap` and `OverlapSphere`. All take a
     `QueryFilter`: a layer mask, bodies to ignore, and whether to
     include triggers (off by default).
     - Built on Jolt's `NarrowPhaseQuery`, with an object-layer filter
       from the mask and a body filter for the ignore list and sensors.
     - Results give the body, point, surface normal (pointing back at
       the query) and distance.
     - A shape cast that starts inside something hits at distance 0.
     - Overlaps come back sorted by body.
   - **`PhysicsScene`** gives the same in entity terms, and ignores one
     entity (usually the caller).
   - **Blueprint nodes**: Line Trace, Sphere Trace and Overlap Sphere.
     They're impure, with `layers` as an int mask and `ignore self` on by
     default. They compile to one `TRACE` instruction with a table entry,
     and the VM asks `SetPhysicsQueries` hooks the game connects.
     Without hooks they find nothing and warn BP207. HitResult structs,
     multi-hit traces, box and capsule traces, and the force and velocity
     nodes come later.
5. ✅ **Done.** `CharacterMovement` with tests for slopes, steps, and jump
   buffering.
   - **The component** has the §13.3 settings (saved), plus per-step input
     (`AddInput`, `Jump`, `run`) and read-only state: velocity, mode,
     grounded, ground normal, and the coyote and buffer timers.
   - **`CharacterSystem`** keeps one Jolt `CharacterVirtual` per entity.
     The shape is a capsule offset so the Transform's position is the
     feet. It's rebuilt when the size, mass, slope limit or layer
     changes. Each fixed step:
     - The timers update. A jump happens if one was pressed within the
       buffer and the character is grounded or within coyote time.
     - On the ground it accelerates toward the input or brakes. In the
       air it moves toward only `air_control` of the input, at
       `air_acceleration`.
     - On the ground it takes the ground's velocity (moving platforms).
       Gravity is added only in the air: on walkable ground it would
       creep the character down slopes.
     - `ExtendedUpdate` walks stairs up to `step_height` and sticks to
       the floor while walking.
     - The feet go back to the Transform. A Transform set by gameplay
       teleports the character.
     - Events: Landed (with impact speed), Jumped, MovementModeChanged
       (Walking, Falling or Flying; Swimming comes with water).
   - **Triggers and queries**: each character has Jolt's inner kinematic
     body, which `PhysicsScene::AdoptBody` maps to its entity. Triggers
     now notice kinematic bodies (`mCollideKinematicVsNonDynamic` on
     sensors).
   - **Blueprints**: the events OnLanded(impact_speed), OnJumped and
     OnMovementModeChanged(new_mode). `CharacterEventName` gives the
     names to dispatch. Movement input nodes wait for the input system's
     Blueprint hookup.
6. ✅ **Done** (portable side). Collider gizmos and the physics debug draw
   toggle. Phase 13 is complete on the portable side.
   - **`DrawPhysicsDebug(world, scene, options, lines)`** produces
     world-space `DebugLine`s for everything the options ask for:
     colliders, triggers, characters, and the last step's contacts
     (points and normals).
     - `PhysicsDebugOptions` is reflected, for the viewport menu's toggle
       and the editor settings.
     - Colors: static gray, kinematic blue, dynamic green (darker
       asleep), triggers orange, characters cyan, contacts red.
   - **Wireframes**: boxes, spheres and capsules are drawn exactly,
     following the entity's Transform and the collider's center. Convex
     and mesh colliders use their body's triangles (what actually
     collides), or a cross per point when there's no body yet.
     `DrawColliderWireframe` draws one entity's colliders, for the
     selection gizmo.
   - **Handles**: `ColliderHandles` gives a box's six faces, a sphere's
     six radius points, and a capsule's four radius points and two
     ends, with world positions and outward directions.
     `DragColliderHandle` applies a drag. A box face or capsule end moves
     while the opposite one stays put, as in Unity. Sizes stay at least
     1 cm, and a capsule's radius at most half its height.
   - **Waiting on Windows**: the viewport draws the lines and handles and
     records drags as undoable field edits, with the other editor window
     hookups.

---

## Phase 14: the unified renderer

The concept is in [ROADMAP.md Phase 14](../ROADMAP.md): one `renderer/`
library that both the editor viewport and the player use, on the RHI.
Everything that doesn't need a GPU is plain C++ in `Aether::Renderer`, and
tested headless. The CPU versions of the GPU passes (clustering, cascades,
culling) are also the reference the GPU passes are checked against.

### 14.1 Frame flow

```
game thread:  ExtractRenderScene(world)  -> RenderScene (flat arrays, a copy)
              MakeView(camera)           -> View (matrices, frustum, jitter)
render:       Cull(scene, view)          -> visible objects
              BuildDrawList(...)         -> sorted, batched draws (instancing)
              BuildLightClusters(...)    -> 16x9x24 froxels -> light lists
              ComputeCascades(...)       -> 4 sun shadow cascades
              RenderGraph (on the RHI)   -> passes, barriers, transient memory
              post (exposure, tone map)  -> the swap chain / editor texture
```

The RenderScene is a copy, so the game can move on while the frame
renders.

### 14.2 PR breakdown

1. ✅ **Done.** `renderer/` library: light and post-process components,
   RenderScene extraction, views, CPU frustum culling, and sorted,
   instanced draw lists.
   - **Components**: `DirectionalLight`, `PointLight` (range), `SpotLight`
     (inner and outer cone angles), `SkyLight` and `PostProcessVolume`
     (global or a bounded box with a blend distance, a weight and a
     priority: exposure compensation, bloom, vignette, saturation, and
     the tone mapper). Lights point along their entity's -Z. Meshes are
     today's `ModelRenderer`, so `MeshRenderer` with material slots waits
     for material assets (Phase 15).
   - **`ExtractRenderScene`** copies what's visible in the ECS into flat
     arrays:
     - active entities only (`IsActiveInHierarchy`), with world
       transforms through the hierarchy, in entity order;
     - world bounding boxes from a `mesh_bounds` provider (a unit cube
       without one), transformed with Arvo's method;
     - a `mesh_key` per mesh (the model GUID, or the path for older
       data);
     - light radiance (color × intensity), spot cone cosines with inner
       clamped to outer, and the sky's sum.
   - **Views**: `MakeView(camera, world transform, aspect)` gives the view,
     projection and view-projection matrices, position, forward and
     frustum. `MakeViewFromActiveCamera` uses the scene's active camera.
     The frustum's six planes come from the view-projection matrix
     (Gribb–Hartmann, for [0, 1] depth). It tests points, spheres and
     boxes; boxes use the positive vertex, so the test is conservative.
   - **`Cull` and `BuildDrawList`**: visible objects are grouped by mesh
     into instanced batches. Instances are sorted front to back by the
     nearest point of their box, and batches by their nearest instance,
     for the depth pre-pass.
   - **`BlendPostProcess(scene, position)`** applies volumes in priority
     order. Numbers blend by weight × falloff outside bounded boxes; the
     tone mapper switches at half weight.
2. ✅ **Done.** Light clustering (CPU reference for the compute pass) and
   cascaded shadow splits (stable, texel-snapped).
   - **`BuildLightClusters(scene, view)`** divides the view into
     16 × 9 × 24 froxels.
     - Depth slices are exponential for perspective (thin near the
       camera) and linear for orthographic views.
     - Each froxel's view-space box is tested against the lights'
       spheres: a point light's range, or the tightest sphere around a
       spot light's cone (`SpotBoundingSphere`).
     - Lists are packed: each cluster has an offset, then its point
       indices, then its spot indices.
     - A per-cluster cap (256) drops the extras and counts them.
     - `ClusterOf(view, point)` and `SliceOf(depth)` find a point's
       cluster, as the shader will.
   - **`CascadeSplits`** uses the practical scheme: `lambda` blends
     logarithmic and uniform splits.
   - **`ComputeCascades(view, sun, settings)`**: 4 cascades up to
     `shadow_distance` (100 m by default).
     - Each covers its frustum slice with a bounding sphere, with the
       radius rounded up to 1/16 m, so turning the camera never resizes
       it.
     - Its center is snapped to whole texels in light space, with two
       texels of margin, so moving the camera shifts the map by whole
       texels.
     - Its orthographic light view reaches `depth_padding` further toward
       the sun, for casters outside the view.
3. ✅ **Done** (planning). The render graph on the RHI: pass
   declarations, dependency order, culling unused passes, barriers, and
   transient resource aliasing, with the planning tested headless.
   - **`FrameGraph`** (`renderer/include/aether/renderer/frame_graph.h`)
     is the backend-neutral plan. Each pass declares its queue (graphics
     or compute) and its reads and writes, each with an `Access`
     (ColorTarget, DepthWrite/Read, ShaderRead/Write, Copy, IndirectArgs,
     Present). Resources are transient textures and buffers, or imported
     ones with an initial and a final access.
   - **`Compile()`**:
     - **Order**: passes run in declaration order. A pass can only
       depend on earlier ones, so that order is always valid, and it's
       the order the author wrote.
     - **Culling**: starting from side-effect passes and writers of
       imported resources (frame outputs), it keeps what they need.
       Only read-after-write and write-after-write edges carry data;
       write-after-read edges only order passes, so they can't keep a
       useless reader alive.
     - **Barriers**: each pass moves its resources to the access it
       declared. ShaderWrite after ShaderWrite gets a UAV barrier. A
       transient's first use in memory another resource used earlier
       this frame gets an aliasing barrier. Imported resources get final
       barriers back to their final access.
     - **Queues**: each pass lists the other queue's passes it depends
       on, including write-after-read, so the backend can place
       fences.
     - **Transient memory**: each resource's lifetime is its first to
       last use in the order. Resources are placed largest first, each
       at the lowest 64 KB-aligned offset free of anything alive at the
       same time.
     - **Errors**: a transient read before anything writes it, and one
       pass using a resource two ways.
   - **On the RHI**: executing the plan (resource creation in one heap,
     barriers, queue fences, pass callbacks) is the GPU work of step 5.
     The D3D12-only `gfx::RenderGraph` stays until then.
4. ✅ **Done.** Post-processing math: auto exposure, ACES/AgX tone
   mapping, bloom chain sizes, TAA jitter, with CPU references
   (`renderer/include/aether/renderer/post.h`).
   - **Auto exposure**:
     - A 256-bin log2 luminance histogram over [-10, 10] EV, with black
       in bin 0.
     - The average takes the pixels between the 50th and 95th
       percentiles, so the sun and the dark half don't pull it; black
       pixels never count.
     - EV100 from the average (K = 12.5), then exposure =
       2^compensation / (1.2 × 2^EV100).
     - Adaptation moves exponentially toward the target, faster
       brightening (3/s) than darkening (1/s), clamped to [-4, 16] EV.
   - **Tone mapping**:
     - ACES uses Stephen Hill's RRT + ODT fit.
     - AgX uses Troy Sobotka's inset matrix, a log2 encoding over
       [-12.47, 4.03], a 6th-order base curve, the outset matrix, and a
       2.2 decode.
     - `None` clamps. sRGB encode and decode are included.
   - **Grading**: saturation around Rec. 709 luminance, and a vignette
     factor.
   - **Bloom chain**: half size per level, rounded up, from half the
     source, down to 8 px or 6 levels.
   - **TAA**: Halton(2, 3) jitter repeating every 8 frames, centered on
     the pixel. `JitterProjection` adds offset × w to clip x and y, so
     every point moves by exactly the offset at any depth, for
     perspective and orthographic projections.
5. GPU passes on D3D12 and Vulkan: depth pre-pass, clustered forward PBR,
   shadows, sky, post (Windows).
6. The editor viewport and the player on the renderer, view modes, and the
   screenshot parity test (Windows).

---

## Phase 15: materials and the material graph

The concept is in [ROADMAP.md Phase 15](../ROADMAP.md). Materials are node
graphs, like Blueprints, that compile to HLSL. The model, validation, code
generation and instances are plain C++ in `Aether::Renderer` and are tested
headless; compiling the HLSL (DXC) and the live preview need the GPU side.

### 15.1 The material asset (`.amat`)

```json
{
  "$type": "Material", "$version": 1,
  "shading": "DefaultLit", "blend": "Opaque", "two_sided": false,
  "parameters": [
    {"name": "Roughness", "type": "float", "default": [0.7, 0, 0, 0]},
    {"name": "Albedo", "type": "texture", "texture": "<asset guid>"}
  ],
  "nodes": [{"id": 1, "type": "Material.Output", "pos": [600, 0]}, ...],
  "links": [{"from": [2, "rgb"], "to": [1, "BaseColor"]}]
}
```

Pin types are float, float2, float3 and float4, plus texture. Math nodes are
generic: their result is their widest input, and a float input broadcasts
(so float3 × float works, float3 + float2 doesn't).

### 15.2 PR breakdown

1. ✅ **Done.** The material model: asset, graph, node library, type
   inference, validation (MT001–MT010) and `.amat` save/load
   (`aether/renderer/material.h`).
2. ✅ **Done.** HLSL generation: dead-node elimination, shared
   subexpressions, parameter buffer layout (16-byte rules) and texture
   slots, and a permutation key (`aether/renderer/material_codegen.h`,
   §15.3).
3. ✅ **Done.** Material instances: parameter overrides without
   recompiling, the packed parameter buffer, and the Blueprint parameter
   nodes (`aether/renderer/material_instance.h`, §15.4).
4. ✅ **Done.** Material functions (reusable subgraphs), a Custom HLSL
   node, Noise and Triplanar (§15.5).
5. ✅ **Done** (portable part). The material editor: the graph panel
   (Phase 12's widget), parameters, details and stats
   (`editor/src/graph/material_*`). The live preview sphere and the
   Windows window hookup come with step 6.
6. DXC compilation to DXIL and SPIR-V, the permutation cache in the DDC, and
   the live preview (GPU).

### 15.3 Generated shaders

`GenerateHlsl` produces an include that the renderer's passes call:

- `cbuffer MaterialParams : register(b0, space1)` holds every scalar and
  vector parameter, widest first, each with an explicit `packoffset` in the
  first gap that doesn't cross a 16-byte register. The layout depends only
  on the parameters, so instances share it.
- The textures the graph samples are `Texture2D T_<name> : register(tN,
  space1)`, in declaration order, with one `MaterialSampler` at `s0`.
- `MaterialOutputs EvaluateMaterial(MaterialInputs i)` is the pixel stage.
  It fills every output: pins the settings ignore get their defaults, and
  their nodes are not emitted.
- `float3 EvaluateWorldPositionOffset(MaterialInputs i)` is the vertex
  stage. It samples with `SampleLevel(..., 0)`.
- Defines select the pass's permutation: `MATERIAL_SHADING_*`,
  `MATERIAL_BLEND_*`, `MATERIAL_TWO_SIDED`, `MATERIAL_OPACITY_MASK_CLIP`,
  `MATERIAL_WORLD_POSITION_OFFSET`, `MATERIAL_TEXCOORDS=N` and
  `MATERIAL_USES_*`.
- The permutation key is a hash of the code and the defines. Moving nodes
  or changing parameter values doesn't change it.

### 15.4 Material instances (`.amati`)

```json
{
  "$type": "MaterialInstance", "$version": 1,
  "parent": "materials/M_Metal.amat",
  "parameters": { "Tint": [0.8, 0.4, 0.2, 0], "Roughness": 0.9,
                  "Albedo": {"texture": "<asset guid>"} }
}
```

- A parent is a material or another instance; chains resolve parent
  first, up to 16 deep. The chain errors are MI003 (a loop), MI004 (a
  missing or wrong-type parent) and MI005 (too deep).
- An override is skipped with a warning when the parameter was renamed
  away (MI001) or its kind changed (MI002), so instances survive edits to
  their parent.
- Instances share the parent's shader: only the MaterialParams bytes and
  the texture bindings differ.
- At runtime a `ParameterBlock` holds one set of values. It repacks
  lazily, and its revision changes only when a value does.
- Per entity, the `MaterialParameters` component names the material or
  instance to draw with and holds overrides. Its Blueprint-callable
  methods become the Blueprint parameter nodes (Set Scalar, Set Vector,
  Set Texture, Get Scalar, Get Vector, Clear).
- Extraction copies the overrides into the render scene, and draw
  batches split by material.

### 15.5 Functions and custom code

- A material function (`.amf`, `"$type": "MaterialFunction"`) is a graph
  with `Function.Input` nodes (`name`, `type`, `default`) and
  `Function.Output` nodes instead of a Material Output. A material calls
  one with `Function.Call:<name>`, resolved against its `FunctionLibrary`.
  - The call's pins are the function's inputs and outputs, in node order.
    Texture inputs are required.
  - Codegen inlines calls, recursively. Each input and output becomes a
    `Utility.Reroute` of its declared type, so a function's output type
    is exactly what it declares.
  - Errors: MT011 (the interface), MT012 (unknown or recursive calls) and
    MT013 (a called function has its own errors).
- `Custom.HLSL` takes typed inputs (unique identifiers, not `i` or
  `_t…`), an output type and a function body. Each distinct body and
  signature is emitted once, as a helper function.
- `Math.Noise` is 3D gradient noise over world position (by default)
  times a scale. With 1 to 8 octaves, it becomes fBm.
- `Texture.Triplanar` projects a texture along the three axes, blending
  by the normal raised to a sharpness. The vertex stage samples with
  `SampleLevel`.

---

## Phase 16: animation

The concept is in [ROADMAP.md Phase 16](../ROADMAP.md). The runtime is the
`Aether::Animation` library (`animation/`): skeletons, poses, clips,
blending, blend spaces, animation graphs, IK and retargeting. It is plain
C++ on the engine and is tested headless. GPU skinning in the renderer and
Jolt ragdolls come at the end of the phase.

### 16.1 Data

- A **Skeleton** is a list of bones, each with a parent that comes before
  it. A bone has a name, a rest pose (translation, rotation, scale) and an
  inverse bind matrix. `BuildSkeleton` makes one from an imported model's
  skin. Each bone's parent is its nearest ancestor that is also a joint,
  and the joints are reordered so that parents come first. Non-joint
  nodes between joints, and above the root joint, are folded into the
  rest poses, so model space matches what the inverse binds expect.
- A **Pose** is one local transform per bone. `LocalToModel` walks the
  bones in order, and skin matrices are `model * inverse_bind`.
- An **AnimationClip** has one track per bone (empty means the rest
  pose), each with translation, rotation and scale keys. `BuildClip` maps
  an imported animation's channels onto the skeleton.
  - Sampling uses a binary search for the key, then lerp for translation
    and scale and a shortest-path nlerp for rotation. Step interpolation
    is kept.
  - Keyframe reduction drops keys that interpolation between their
    neighbours reproduces within a tolerance (metres, radians, scale
    ratio).
  - Compression quantizes each track to 16 bits per component, within
    the track's range, and uses smallest-three for rotations (the
    largest component is dropped, and its index is kept in 2 bits). The
    result is a byte blob that decodes back into a clip.
- **Blending**: `BlendPoses` blends two poses by a weight, optionally per
  bone through a mask. `MakeAdditive` and `ApplyAdditive` make and apply
  deltas against a reference pose. `BoneMask` covers a bone and its
  descendants, which is how layered upper-body and lower-body blends
  work.

### 16.2 PR breakdown

1. ✅ **Done.** Skeletons, poses, clips (sampling, reduction,
   compression), blending, additive and bone masks
   (`aether/animation/*.h`).
2. ✅ **Done.** Blend spaces (1D, and 2D by triangulation) with synced
   playback, and root motion extraction (§16.3).
3. ✅ **Done.** The animation graph model and its runtime: a state
   machine (states, transitions with conditions on graph variables,
   blend times, conduits, sub-machines) and the blend nodes. Diagnostics
   AG001–AG011 (§16.4).
4. ✅ **Done.** The `Animator` component, evaluated in parallel jobs;
   montages (sections, slots); anim notifies as Blueprint events; root
   motion into `CharacterMovement` (§16.5).
5. ✅ **Done.** IK (two-bone, look-at, FABRIK, foot placement) and
   retargeting between skeletons (§16.6).
6. ✅ **Done.** Editors (portable): the animation graph and state
   machine editor, the blend space editor and the asset viewer
   (`editor/src/anim/`). The 3D preview needs the renderer (step 7).
7. GPU skinning in the renderer, and ragdolls with blending to and from
   animation (GPU / Windows).

### 16.3 Blend spaces and root motion (`.ablend`)

```json
{
  "$type": "BlendSpace", "$version": 1, "dimensions": 2,
  "x": {"name": "Right", "min": -300, "max": 300},
  "y": {"name": "Forward", "min": -300, "max": 300},
  "samples": [{"clip": "<asset>", "x": 0, "y": 300, "rate": 1.0}]
}
```

- **Weights**: 1D blends the two neighbouring samples. 2D uses a Delaunay
  triangulation (Bowyer-Watson, over positions normalized by the axis
  ranges) and barycentric weights. Outside the hull it uses the nearest
  point on it. Collinear 2D samples blend along their line. Parameters
  are clamped to the axes.
- **Diagnostics**: BS001 no samples; BS002 two samples in one place;
  BS003 collinear 2D samples; BS004 a bad axis or dimension count; BS005
  (warning) a sample outside its axis; BS006 no clip, or a rate of 0 or
  less.
- **Synced playback**: every clip plays at the same normalized phase. A
  cycle takes the weighted average of `duration / rate`, so changing the
  parameters changes speed without popping the feet.
- **Root motion**:
  - It is the root bone's movement over the ground, and its heading
    (yaw) about +Y. Vertical movement is included only on request.
  - The movement is expressed in the root's frame at the start of the
    step, and wrapping past the end of a loop is handled.
  - `StripRootMotion` plays the clip in place: the root stays at its
    start position and heading.
  - A blend space's motion is the weighted sum of its clips' motion.

### 16.4 Animation graphs (`.aanim`)

- **Variables** are bool, int, float or trigger. A trigger lasts until a
  transition uses it or the next Update ends.
- **Pose nodes** form a DAG ending at `output`:
  - Clip;
  - Blend Space, with its parameters from variables;
  - Blend, by a float alpha;
  - Blend by Bool and Blend by Int, which crossfade over `blend_time`;
  - Layered, where a layer applies from a bone down, with an optional
    soft ramp;
  - Additive, which applies the input's difference from the rest pose;
  - State Machine.

  A node shared by two parents is evaluated once per frame.
- **State machines**:
  - Each state plays a pose node. A State Machine node there makes it a
    sub-machine, which restarts at its entry whenever its state is
    entered.
  - Conduits have no pose: a transition into one is taken only when one
    of the conduit's exits holds too, and it goes straight through.
  - Transitions come from a state or from any state. Each has conditions
    that must all hold, and a priority (higher first, then list order).
  - A transition can also require that the state's animation is within
    `blend_time` of its end.
  - Transitions crossfade over `blend_time`, and the state being left
    keeps playing during the crossfade. A transition taken mid-blend
    blends from a snapshot of the current pose, so nothing pops.
  - The state changes of each Update are reported (for Blueprint events
    in step 4).
- **Diagnostics**:
  - AG001: no output node.
  - AG002: wrong inputs.
  - AG003: pose nodes that loop.
  - AG004: an unknown variable.
  - AG005: a variable of the wrong type for its use or comparison.
  - AG006: a machine problem (no states, a bad entry, a missing state,
    an unknown machine).
  - AG007: a conduit as the entry, or with no way out.
  - AG008: a machine inside itself.
  - AG009 (warning): an unreachable state.
  - AG010: a missing clip or blend space.
  - AG011: a duplicate name.

### 16.5 Animator, montages, notifies and root motion

- **Notifies**: named points in a clip, or windows when they have a
  duration.
  - A playhead reports those it crosses, after its start and up to and
    including its end; a loop wrap reports the end and then the start.
    Windows report a begin and an end.
  - In a graph, a notify comes with the weight of the path that played
    it. A blend space reports only its heaviest clip's notifies, so a
    blend of walk cycles steps once.
  - Compressed clips carry notifies from format version 2; version 1 data
    still loads.
- **Montages** (`.amontage`): a clip on a named slot, with blend in and
  out times and sections.
  - Each section runs to the next section's start and then follows
    `next`. A section can name itself as `next` to loop; an empty `next`
    ends the montage.
  - A montage blends out so that it is gone by the end of the last
    section in its chain.
  - `JumpToSection` moves between sections. `StopMontage` blends out
    early and ends the montage as interrupted. Playing a montage over
    another ends the old one as interrupted and takes over its weight,
    so nothing pops.
  - A graph's `Slot` node blends its slot's montage over its input.
  - Diagnostics MN001–MN004.
- **Events** (`AnimEvent`):
  - Notify, NotifyBegin, NotifyEnd.
  - StateChanged.
  - MontageStarted, MontageSectionChanged, MontageBlendingOut,
    MontageEnded (with `interrupted`).
  - Calls made between Updates report with the next Update.
  - Blueprint events: `Event.OnAnimNotify(name)`,
    `Event.OnAnimNotifyBegin/End(name)`,
    `Event.OnAnimStateChanged(machine, from, to)`,
    `Event.OnMontageStarted(montage)`,
    `Event.OnMontageSectionChanged(montage, section)`, and
    `Event.OnMontageBlendingOut/Ended(montage, interrupted)`.
- **Root motion in graphs**: with root motion on, clips, blend spaces and
  montages play in place. Their motion is summed, weighted like their
  poses, and a montage counts by how far it is blended in.
- **`Animator` component** (graph, skeleton, root_motion, speed, paused):
  - Its Blueprint-callable methods (Set Float/Bool/Int Parameter, Set
    Trigger, Play Montage, Stop Montage, Jump To Section) queue commands.
  - `AnimationSystem` applies them, then evaluates every Animator. With
    a `JobSystem` they are evaluated in parallel, one job per character,
    with results identical to serial.
  - It keeps each entity's pose, skin matrices, events and world-space
    root motion.
- **`CharacterMovement::AddRootMotion(displacement, yaw)`**: for one
  step, the displacement replaces the input's horizontal movement
  (gravity and jumps still apply), and the yaw turns the Transform.

### 16.6 IK and retargeting

- Solvers take model-space targets and change the pose's local
  rotations. Foot placement also moves the pelvis.
- **Two-bone IK**:
  - The middle joint bends to the distance law-of-cosines gives, then
    the root swings the end onto the target.
  - The root then twists about the root-target line so the middle joint
    faces the pole.
  - A straight limb bends in the plane through it and the pole.
  - Out of reach, the limb straightens toward the target. Too close, the
    joint closes as far as the bone lengths allow.
  - `weight` blends with the animated pose.
- **Look-at**: turns a bone so a local aim axis points at the target,
  limited to `max_angle` from the animated pose, and weighted.
- **FABRIK**: backward and forward passes over a chain until the tip is
  within `tolerance` of the target. It keeps the root and the bone
  lengths, and goes straight at targets out of reach.
- **Foot placement** (`FootPlacer`):
  - It traces the ground under each foot and measures it against the
    animation's ground (model y = 0), so lifted feet stay lifted.
  - The pelvis drops to the lowest foot's offset, within
    `max_step_down`. Each leg's two-bone IK uses its knee as the pole,
    keeping the bend plane.
  - Each foot tilts to its ground normal, up to `max_foot_angle`.
  - Offsets ease in over `smoothing` seconds; the first frame snaps.
- **Retargeting**:
  - Each target bone takes its source bone's model-space rotation away
    from rest, applied to its own model-space rest rotation. Limbs point
    the same way even when the rigs' bone axes differ.
  - Only the root translates, scaled by the ratio of the roots' rest
    heights. Unmapped bones keep their rest pose.
  - `AutoMapBones` pairs names ignoring case, `_`, `-`, spaces and a rig
    prefix ("mixamorig:").
  - `RetargetClip` resamples a whole clip and carries its notifies.


---

## Phase 17: audio

The concept is in [ROADMAP.md Phase 17](../ROADMAP.md). The engine side is
the `Aether::Audio` library (`audio/`): a software mixer (voices, buses,
effects, 3D) that renders into a buffer, so it runs and is tested headless.
An output backend (miniaudio first, behind an interface so FMOD or Wwise
can be added later) pulls from the mixer on its own thread.

### 17.1 Mixing

- **SoundWave**: interleaved float samples, a sample rate and 1 or 2
  channels. WAV files decode from 8-, 16-, 24- and 32-bit PCM and 32-bit
  float, including `WAVE_FORMAT_EXTENSIBLE`, and other chunks are
  skipped.
- **Voices** play a sound on a bus:
  - Sounds are resampled to the mixer's rate with cubic (Hermite)
    interpolation, times the pitch.
  - Volume changes are smoothed over a short ramp (no zipper noise), and
    fades in and out are linear in gain.
  - Mono sounds pan with equal power; stereo sounds pan by balance.
  - A voice can loop. One that doesn't frees itself at its end, as does
    one that finishes fading out.
- **Buses** form a tree under Master (Music, SFX, UI and Voice by
  default):
  - Each bus has a volume in dB, a mute, and effects that run in order.
  - Children mix into their parent after their own effects and volume.
  - Each bus measures its peak and RMS per channel for the meters.
- **Effects**: biquad filters (low-pass, high-pass, band-pass, notch,
  shelves, peak, from the RBJ cookbook), a feed-forward compressor
  (threshold, ratio, attack, release, makeup) and a Freeverb-style
  reverb (8 comb and 4 all-pass filters per channel; room size, damping,
  wet and dry, width).

### 17.2 3D audio

- **Attenuation** by distance between `min_distance` (full volume) and
  `max_distance` (no further falloff):
  - Inverse: `min / (min + rolloff * (d - min))`.
  - Linear: 1 to 0.
  - Logarithmic: `1 - ln(d / min) / ln(max / min)`.
  - Custom: a piecewise-linear curve over 0..1 of that range.
  - Air absorption, when set, is a one-pole low-pass whose cutoff moves
    from 20 kHz to `lowpass_at_max_hz`, interpolated in octaves.
- **Listener**: position, forward, up and velocity. Pan is the source
  direction along the listener's right (`forward × up`), fed into the
  equal-power panner.
- **Spatial blend** interpolates gain, pan, doppler and air absorption
  between the 2D voice (0) and full 3D (1).
- **Doppler**: `(c - f·v_listener) / (c - f·v_source)` along the line
  between them; speeds are clamped below `c / f` and the ratio to 1/4..4.
- **Occlusion**: `occlusion_query(listener, source)` returns 0..1 and is
  called by `UpdateOcclusion` (game thread) for voices that opt in; it
  can also be set per voice. Full occlusion is -12 dB and a 1.2 kHz
  low-pass, both scaled log-linearly; the amount follows its target with
  a 0.1 s time constant.
- **Voice limiting**:
  - Each Render, voices are ranked by priority, then audibility (voice
    gain × distance and occlusion gain × bus gains down to Master).
  - The first `max_voices` are real; the rest, and any voice below
    `virtual_threshold_db` (-80), are virtual.
  - Virtual behaviour per voice: Continue (keeps its position), Restart
    (resumes from the start) or Stop (ends).
  - A voice going virtual is mixed for one more block ramping down, and
    one coming back ramps up over its first block.

### 17.3 Decoding, streaming and sound cues

- **Formats**: WAV (our decoder), Ogg Vorbis (stb_vorbis) and FLAC
  (dr_flac), detected from the file's first bytes. Mono and stereo.
- **Streams** (`AudioStream`) decode as they play, from compressed bytes
  in memory (shared between voices) or from a file. `Mixer::PlayStream`
  keeps a window of decoded frames per voice, decoded 4096 frames ahead
  at a time:
  - Its position counts on through loops, and the stream seeks to 0 at
    its end.
  - Jumping elsewhere (a start time, back from virtual, a restart) seeks.
  - A streamed voice mixes exactly the same samples as the decoded sound
    would.
- **Delayed starts**: `PlayParams::delay` starts a voice that many
  seconds into the next Render, to the sample. The mixer counts the
  frames it has rendered as its clock.
- **Sound cues** (`.acue`, JSON): nodes with children, and the output's
  settings (volume, pitch, bus, priority, virtual mode, 3D).

  | Node | Plays |
  |---|---|
  | Wave | a sound, optionally looping |
  | Random | one child by weight; `no_repeat` never picks the last one again |
  | Sequence | one child in turn, remembered between plays |
  | Modulator | its child with a random volume and pitch from ranges |
  | Concatenator | its children back to back |
  | Loop | its child `count` times, or forever (0) |
  | Mix | all its children at once, each with an input volume |
  | Delay | its child after a random wait |

- **Evaluation** turns a cue into a plan: sounds with start offsets,
  volumes and pitches (pitch shortens durations), and its duration.
  - A Loop forever over a Wave is one looping voice.
  - A Loop forever over anything else leaves a tail: the body is
    evaluated again at the time it ends, so random choices vary each
    time round.
  - Unknown sounds play nothing.
  - Choices and random numbers come from a seeded `CueState` per cue.
- **Validation** (errors unless noted):

  | Code | Problem |
  |---|---|
  | CU001 | no output node |
  | CU002 | duplicate node id |
  | CU003 | a child that doesn't exist |
  | CU004 | a cycle |
  | CU005 | the wrong number of inputs |
  | CU006 | a Wave with no sound |
  | CU007 | a sound that doesn't exist |
  | CU008 | an inverted or invalid range |
  | CU009 | negative or all-zero weights |
  | CU010 | (warning) inputs after one that never ends |
  | CU011 | (warning) a node the output doesn't reach |

- **CuePlayer**: plays cues from a `SoundBank` (decoded or streamed
  sounds by name).
  - Every sound is placed as a delay from the cue's start frame, so
    concatenations and loops are seamless and never drift.
  - Each repeat of an endless loop is scheduled when it comes within
    `lookahead` (0.25 s) in `Update`.
  - Instances can be stopped (with a fade), moved and have their volume
    changed.

### 17.4 Components, the audio system and Blueprints

- **AudioSource**: a cue, auto-play, volume and pitch.
  - Blueprint methods: Play, Stop, Fade In, Fade Out, Set Volume, Set Cue
    and Is Playing (pure). They queue commands the system applies.
  - Its cue follows the entity's world position, with velocity from its
    movement (doppler).
  - When the cue ends by itself, the system reports it, dispatched as
    `Event.OnAudioFinished (cue)`. Stopping or fading out isn't ending.
  - Entities that go away fade their sound out over 50 ms.
- **AudioListener**: the first active one (lowest entity index) sets the
  mixer's listener from its world position and facing (-Z forward, +Y
  up), with velocity from its movement.
- **ReverbZone**: full strength within `radius`, fading linearly over
  `blend_distance`.
  - The listener's zone is the highest priority, then the strongest.
  - The system adds one reverb effect to `reverb_bus` (SFX) the first
    time any zone exists. It sets room size and damping from that zone,
    and wet = the zone's wet × its strength.
- **AudioSystem** (`Update(dt)`):
  - listener, sources, attached sounds, bus fades, reverb, occlusion
    (`Mixer::UpdateOcclusion`) and the cue player;
  - world positions follow the scene hierarchy when given a `GuidIndex`;
  - missing or invalid cues (checked against the sound bank) are reported
    once each.
- **The Audio library** (static Blueprint functions, category "Audio"),
  acting on the active system (the last one made):

  | Node | Does |
  |---|---|
  | PlaySound2D (cue, volume_db, pitch) | plays a cue without 3D, even a spatial one |
  | PlaySoundAtLocation (cue, location, volume_db, pitch) | plays a cue at a point |
  | SpawnSoundAttached (cue, target, offset, volume_db) | follows an entity (the offset turns with it) and stops if it's destroyed |
  | SetBusVolume (bus, volume_db, fade_seconds) | fades linearly in dB; a new fade replaces the old one |
  | StopAllSounds | stops everything the system plays |

- **Entity parameters**: `Entity` is now reflected, so native functions can
  take and return entities as Blueprint Entity pins. Static native
  functions are listed under their type's name rather than
  "Components|".

### 17.5 Output and the audio thread

- **Backends** (`AudioBackend`) own the thread that asks for audio. They
  call back for each period of interleaved stereo.
  - `NullBackend`: real time (a thread paced by the period) or manual
    (`Pump` renders on the caller's thread); it can capture what it's
    given.
  - `MiniaudioBackend`: the platform's device through miniaudio (WASAPI,
    Core Audio, ALSA, PulseAudio, ...), 32-bit float stereo at the
    mixer's rate. It can use miniaudio's null device for tests.
  - `CreateDefaultBackend` tries the device and falls back to real-time
    null.
- **AudioOutput** threads the mixer and renders it from the backend's
  callback; stopping puts it back on one thread.
- **Threaded mixer**:
  - Changes (play, stop, volume, position, buses, listener, effects,
    `Post`) become commands in a lock-free single-producer
    single-consumer queue (8192). The audio thread applies them in
    order at the start of each Render.
  - Used commands go back through a second queue, so the game thread
    frees them.
  - If the queue is full, the game thread waits for room; commands are
    never dropped.
  - Voice ids are handed out on the game thread, so `Play` returns at
    once.
  - After each Render the audio thread publishes a snapshot through a
    triple buffer: the last command applied, the frame clock, each
    voice's info and playback time, the bus meters and the real-voice
    count.
  - Queries read the snapshot plus the game thread's pending changes. A
    queued play counts as playing, a queued hard stop as stopped, and a
    queued StopAll as everything stopped. Bus settings and the listener
    come from a game-side copy.
  - A play remembers the game's clock when it was asked. The frames it
    waited in the queue come off its delay, so scheduled sounds (cue
    loops) stay sample-accurate when the delay covers the latency.
  - Occlusion is asked on the game thread, from the game's copy of each
    occluded voice's position.
  - Buses are made before threading. Fields such as `speed_of_sound` are
    set before, or through `Post`. The AudioSystem changes its reverb
    through `Post`.

### 17.6 Editors

- **Waveform preview** (`editor/src/audio/waveform`):
  - Min/max and RMS per pixel column, per channel, over the visible
    range; zoomed in, a column is a single sample.
  - Statistics: peak, RMS, DC offset and clipped samples.
  - The mouse wheel zooms about the cursor (never below 16 frames) and
    Shift+wheel scrolls. Click sets the playhead; dragging selects a
    region.
  - Preview plays from the playhead through a mixer. With a selection, it
    starts at the selection and loops it.
- **Sound cue editor** (`cue_document`, `cue_editor`):
  - The document owns the `.acue` and JSON undo (merged drags; no-op
    edits aren't steps).
  - Nodes are added (the first becomes the output's) and deleted (unhooked
    everywhere, and from the output).
  - Links replace an input, or append one on a multi-input node's "+"
    pin. Loops, inputs on a Wave, and a second input on a one-input node
    are refused with a reason. Disconnecting shifts later inputs with
    their weights and volumes.
  - The graph uses the Phase 12 widget. Nodes are coloured by type, with
    an Output node and error outlines from the diagnostics.
  - Details:
    - per node type: sound, looping, weights, no-repeat, volume and pitch
      ranges, loop count, input volumes and delay range;
    - for the output: name, volume, pitch, bus, priority, virtual mode,
      spatial blend, doppler and occlusion, and attenuation with its
      model, distances, rolloff, air absorption, custom curve points and a
      plot of the falloff.
  - A palette with search offers node types and a Wave per sound.
  - Diagnostics jump to their node. Preview through a CuePlayer (Space) is
    refused while there are errors.
- **Mixer panel** (`mixer_panel`):
  - **Buses**: a strip per bus with L/R meters from -60 to +6 dB (level,
    RMS and a held peak), a fader (double-click for unity) and mute.
    Meter ballistics: the level rises at once and falls 24 dB/s; the peak
    holds 1 s, then falls.
  - **Effects**: bypass and sliders for each effect's settings (filter,
    compressor, reverb), and gain reduction on a single-threaded mixer.
    On a threaded mixer the panel asks the audio thread for the settings
    through `Post` and writes them the same way. It keeps a bypass set
    before the answer arrives.
  - **Voices**: a table of every voice's time, real or virtual, distance,
    gain, pan and pitch.
  - The mixer gained `EffectCount`/`EffectAt` (a game-side list) and
    `Voices()` (from the snapshot when threaded).
- **Sync groups** (found by the TSan run of the threaded cue test): on a
  threaded mixer the game's clock can lag the audio thread by a period.
  A cue's first sound (delay 0) then started that late while its later
  sounds kept their schedule.
  - `Mixer::BeginSyncGroup` makes a time base that the audio thread
    starts when it reaches it. A voice with `PlayParams::sync_group` is
    delayed from that base, not from when its command arrived.
  - The CuePlayer gives each playing cue a group, so its sounds keep
    exact spacing however late their commands arrive (within the
    lookahead). On one thread it behaves as before.

### 17.7 PR breakdown

1. ✅ **Done.** Sounds (WAV), voices (resampling, pitch, loop, pan,
   fades), the bus mixer with meters, and the DSP effects.
2. ✅ **Done.** 3D: listener, attenuation curves (inverse, linear,
   logarithmic, custom) with air absorption, spatial blend, doppler,
   voice limiting by priority with virtual voices, and an occlusion hook.
3. ✅ **Done.** Sound cues (random, sequence, modulate, concatenate,
   loop, mix, delay) and `.acue` files; Ogg Vorbis and FLAC decoding;
   streaming long sounds; sample-accurate delayed starts.
4. ✅ **Done.** Components (`AudioSource`, `AudioListener`,
   `ReverbZone`), the `AudioSystem`, and the Blueprint nodes (Play Sound
   2D/at Location, Spawn Sound Attached, Fade In/Out, Set Bus Volume).
5. ✅ **Done.** The device backend (miniaudio) with a lock-free handoff to
   the audio thread, and a null backend.
6. ✅ **Done.** Editors: the waveform preview, the sound cue graph and the
   mixer panel with live meters.

---

## Phase 18: runtime game UI

The concept is in [ROADMAP.md Phase 18](../ROADMAP.md). The engine side is
the `Aether::UI` library (`ui/`): a retained widget tree with layout,
input and styles. It paints into a draw list of clipped, textured quads,
which the renderer draws (GPU side with the Windows renderer work).
Everything else runs and is tested headless.

### 18.1 Widgets, layout and drawing

- **Units**: layout is in layout units, the pixels of a reference
  resolution (1920×1080 by default); the viewport scales them to the
  screen. +x is right and +y is down.
- **Widget**: a name, visibility, opacity, clipping of children, and a
  slot its parent reads.
  - Visibility: Visible; Hidden (keeps its space); Collapsed (none);
    HitTestInvisible (drawn, not hit, nor are its children); and
    SelfHitTestInvisible (only its children are hit).
  - Layout is two passes: Measure (children first, desired sizes), then
    Arrange (parents first, rectangles).
  - Painting draws the widget, then its children (clipped if it clips),
    then any overlay such as a scroll bar. Opacity multiplies down the
    tree.
  - Hit testing tries children front to back, then the widget itself.
    Panels let clicks through; images, bordered backgrounds and scroll
    boxes catch them.
- **Panels**:

  | Panel | Places its children |
  |---|---|
  | Canvas | by anchors (below), in `z` order |
  | HorizontalBox / VerticalBox | in a row or column; `fill` 0 takes the desired size, `fill` > 0 shares what's left by weight; aligned across; `spacing` between |
  | Grid | in cells (row, column, spans); an auto track is its widest single-cell child, a weighted one (`column_fill`/`row_fill`) shares the rest |
  | Overlay | on top of each other, each aligned in the area |
  | SizeBox | one child, its size overridden or clamped |
  | Border | one child inside `padding`, over a background brush |
  | ScrollBox | one child longer than the box, scrolled, clipped, with a scroll bar; `ScrollIntoView` scrolls the least that shows a rectangle |
  | Spacer | nothing: fixed empty space |

- **Anchors** (Canvas): each axis attaches to a fraction of the canvas.
  - At a point (min = max), the child sits at the anchor plus
    `position`, sized `size` (or its desired size with `auto_size`),
    about its `alignment` pivot.
  - Stretched (min < max), its edges are the anchors inset by `margins`.
- **Text**: lines break at `\n` and, with `wrap_width`, greedily between
  words; a word longer than a line gets one to itself. It's justified left,
  centre or right. UTF-8, with invalid bytes read as U+FFFD. Fonts supply
  ascent, line height, glyphs (advance, quad, UV) and kerning. The
  built-in stand-in has fixed-width boxes; real SDF fonts come in step 5.
- **Brushes**: none, colour, image, box (9-slice: corners keep their
  size, times `slice_scale`, and shrink to fit small rectangles; edges
  stretch one way; the middle both) and frame (9-slice without the
  middle).
- **Draw list**: quads (rectangle, UV, colour, texture, clip index) and
  clip rectangles, each intersected with the one it's pushed inside.
  Empty, transparent or fully clipped quads are dropped. `Scale` turns
  layout units into pixels.
- **Viewport**: widget layers in z order (equal z in the order added),
  each laid out over the safe area.
  - Scale rules: none, shortest side, longest side, width, height, or a
    curve (the shortest side in pixels to a scale), clamped.
  - The safe area is insets in pixels (notches, overscan).
  - It paints to pixels, and hit tests the top layer first.

### 18.2 Controls and input

- **Controls** (a `ControlStyle` until themes arrive in step 3):

  | Control | Behaviour |
  |---|---|
  | Button | pressed on pointer down, clicked on release inside or on accept; one child in `padding`; `on_clicked`, `on_pressed`, `on_released`, `on_hovered` |
  | Toggle | flips on click or accept; `on_changed` only on a change |
  | Slider | press sets the value from the pointer and drag follows; clamped, snapped to `step`; left/right step it while focused (up/down leave) |
  | ProgressBar | filled to `percent` from any side |
  | TextInput | one line; typing inserts at the cursor (replacing a selection); the edit keys move by code point, select with Shift, delete, Home/End, select all; a length limit; passwords shown as `*`; click and drag place and select; control characters are dropped; Enter commits; the text scrolls to keep the whole caret in view |
  | Dropdown | opens a popup of option buttons below it (above when there's no room); focus starts on the selected option and stays in the popup; accept picks, cancel or a click outside closes |
  | ListView | virtualized: `make_row` builds a row and `bind_row` fills it, only for the rows in view, reused as it scrolls; click, the wheel and up/down select and scroll; accept activates |

- **UIInputRouter** (for one viewport; each call says whether the UI used
  the input):
  - **Pointers** (mouse and each touch): hover goes to the nearest
    interactive widget under the pointer. Press focuses it and captures
    the pointer if it takes it; release reports whether it ended inside.
    A captured control counts as hovered only while the pointer is over
    it. Disabled widgets and everything in them ignore input.
  - **Wheel**: offered from the widget under the pointer outwards. A
    scroll box with something to scroll takes it, even at an end.
  - **Focus and navigation**: the focused widget sees navigation first
    (a slider steps, a list moves its selection); otherwise focus moves.
    - Candidates are focusable, enabled, shown, and not entirely clipped
      away (a scroll box scrolled past them).
    - An explicit target by name wins.
    - Next/Previous go in tree order, wrapping.
    - Directions pick the nearest candidate whose centre lies that way,
      scoring distance + 2 × sideways offset.
    - Focusing a widget scrolls it into view in its scroll boxes.
  - **Accept / Cancel**: Accept goes to the focused widget. Cancel closes
    a popup (focus back to its owner), else offers itself from the focused
    widget outwards.
  - **Text and edit keys** go to a focused widget that wants text.
  - **Popups**: one at a time, over everything. A click outside only
    closes it. Closing from inside a handler (an option's click) waits
    until the event is done.
  - **Tooltips**: after `tooltip_delay` hovering the nearest widget with
    one, in a layer that doesn't catch clicks. Moving to another source
    or pressing hides it.
  - Widgets removed from the tree while hovered, focused or captured are
    forgotten.
- **UIInputBridge** (from the Phase 10 device state, each frame):
  - Arrows, the d-pad and the left stick (past `stick_threshold`)
    navigate, repeating after 0.4 s every 0.08 s. Tab and Shift+Tab step
    through.
  - Enter, A (and Space) accept; Escape and B cancel.
  - While a text box has focus, Space types and the arrow keys and edit
    keys (Backspace, Delete, Home, End, Ctrl+A) edit; the d-pad and stick
    still navigate.
  - `SetModal` adds a context that blocks lower-priority (gameplay)
    contexts while a menu is up.

### 18.3 Themes, layout files and data binding

- **Themes** (`.atheme`, JSON): a palette of named colours, control
  styles, and text styles.
  - Colours are `"@name"`, `"#RRGGBB"`/`"#RRGGBBAA"`, or `[r, g, b(, a)]`.
    A bare colour where a brush goes is a solid brush.
  - Brushes name their image by asset path; a resolver turns it into the
    renderer's texture id when the theme loads.
  - A style is a ControlStyle: the normal, hovered, pressed, disabled,
    accent and focus brushes, focus width, text and hint colours, and
    text size.
  - Styles are keyed `"Type"` or `"Type.class"` and can `extends`
    another in any order in the file. Loops and unknown bases are errors.
  - Lookup goes `Type.class`, then `Type`, then `Default`, then the
    built-in style.
  - `ApplyTheme` styles a tree:
    - controls take their style;
    - Text takes `text.<class>` (or `Default`);
    - progress bars take their style's normal and accent brushes;
    - Borders with a class take their style's normal brush.
- **Layout files** (`.aui`, JSON): `{version, root, bindings}`.
  - Each widget records its type, name, class, tooltip, enabled,
    visibility, opacity, clipping and navigation, only the slot fields
    that differ from the defaults, its own properties, and its children.
    A ListView's rows are code, not saved.
  - Types come from a registry: every built-in widget, plus the game's
    own through `RegisterWidgetType` (a maker, and save and load
    functions).
  - Errors name the path to the widget (`Menu/VerticalBox[0]/Buton[0]`):
    unknown types or names, too many children, a newer version, or
    malformed JSON.
- **Data binding** (`DataBinder`): named sources are reflected objects. A
  binding maps a widget (by name) and property to a path
  (`player.stats.mana`, through nested structs).
  - Properties: `text` (Text, TextInput), `visible` (Collapsed when
    false), `enabled`, `opacity`, `percent` (ProgressBar), `value`
    (Slider), `checked` (Toggle) and `selected` (Dropdown, ListView).
  - Options:
    - `divide_by` makes a ratio (health / max health);
    - `format` puts the value in text ("HP {} / 100");
    - `precision` sets digits (whole numbers show none by default);
    - `invert` flips booleans.
  - Strings and bools show as themselves; numbers of any reflected
    scalar type convert.
  - `Update` pushes only values that changed, so an edited widget is left
    alone until its source changes. It sets controls directly, so their
    `on_changed` doesn't fire. A focused text box keeps what's being
    typed until it loses focus.
  - `two_way` writes a slider's value, a toggle's check, a text box's text
    (a number field takes what parses) and a dropdown's choice back to the
    field, then calls the control's earlier `on_changed`.
  - Bad bindings are reported and skipped: a missing widget, source or
    field, a property the widget lacks, or two-way on something that
    can't write back.

### 18.4 Widget Blueprints and UI animations

- **Render transforms**: each widget has `render_offset`, `render_scale`
  and `render_pivot` (0..1 of its box, the centre by default).
  - They change how the widget and its children draw and hit-test, but
    not the layout, so a button can pop or slide without moving its
    neighbours.
  - The draw list keeps a transform stack that quads and clip rectangles
    pass through. Hit testing maps the point back into the widget's
    untransformed space.
- **UI animations** (`UIAnimation`): named timelines of tracks. A track
  animates one property of one widget (by name) through keys of
  `{time, value, ease}`.
  - Eases: Linear, Constant, EaseIn, EaseOut, EaseInOut and Back. A
    key's ease shapes the segment that ends at it.
  - Properties: `opacity`, `offset_x/y`, `scale`, `scale_x/y`,
    `tint_r/g/b/a` (Image and Text colour), `position_x/y` and
    `size_x/y` (the slot), `value` (Slider) and `percent` (ProgressBar).
  - Saved in the `.aui` file under `animations`, keys as `[t, v, "Ease"]`.
- **The animator** (`UIAnimator`) plays animations on a widget tree.
  - `Play` takes a speed, a loop count (0 is forever), reverse and a
    start time. It applies the first frame at once, so nothing flashes.
  - `Tween` animates one property from its current value to a target;
    a new tween of the same property replaces the old one.
  - `Validate` reports tracks naming missing widgets or properties.
  - Finished animations are listed after each `Tick`.
- **Widget components**: a `WidgetComponent` names the layout an entity
  shows, its z-order and whether it's visible.
  - `UISystem` loads the layout, applies the theme and adds it to the
    viewport. It re-points its bindings at the entity's reflected
    components each update, and removes it with the entity.
  - Control callbacks become `WidgetEvent`s: Clicked, ValueChanged,
    CheckChanged, TextCommitted, SelectionChanged and
    AnimationFinished. The game dispatches them to Blueprints as
    `Event.OnWidgetClicked` and so on, with the widget's name.
  - Missing layouts are reported once, not every frame.
- **Blueprint library** (static `UI`): CreateWidget, RemoveWidget,
  SetText (a button sets its first Text), GetText, SetVisible,
  SetEnabled, SetValue, GetValue, PlayAnimation, StopAnimation and
  SetFocus. All take the entity and a widget name.

### 18.5 Distance-field text and world-space UI

- **SDF fonts** (`SdfFont`, `aether/ui/font.h`): TrueType or OpenType
  fonts loaded through stb_truetype. Bad data is refused with a reason.
  - Glyphs are rasterized the first time they're drawn, as signed
    distance fields at one base size (48 px, 6 px of field around each
    glyph). They go into a one-channel shelf-packed atlas. When the atlas
    fills, it doubles in height up to a limit; glyphs that still don't
    fit are reported.
  - The renderer uploads the dirty rectangle, or the whole atlas when it
    has been resized. `Prewarm` rasterizes text ahead of time.
  - Metrics (ascent, line height, advances, kerning) scale from the base
    size. Code points the font lacks draw as U+FFFD, else '?'. Control
    characters take a space's room.
- **Drawing SDF quads**: a quad carries the field's span on screen
  (`sdf_range`, in pixels), its edge value and a softness. The shader's
  coverage is `saturate((d - edge) / (1/range + softness) + 0.5)`
  (`SdfCoverage`), so edges stay about a pixel wide at any size.
  - Render transforms and the viewport's scale grow `sdf_range` with the
    glyphs.
- **Text effects** (`TextEffects`): an outline (width and colour) and a
  drop shadow (offset, colour, softness), drawn in passes: shadow, then
  outline, then fill.
  - With SDF fonts the outline lowers the edge value, and the shadow
    adds softness.
  - With bitmap fonts the outline is four offset copies.
  - Text widgets and theme text styles both have effects, and both are
    saved.
- **Font libraries**: `FontLibrary` names fonts. A viewport's library
  lets a Text widget pick one by name (`font`); unknown or empty names
  use the default. Theme text styles can set the font too.
- **World widgets** (`WorldWidgetComponent`, `WorldUISystem`,
  `aether/ui/world_ui.h`): a layout shown at an entity's place.
  - Settings: an offset from the entity, a size in layout units and a
    pivot.
  - `Screen` widgets are projected and drawn flat in pixels, snapped to
    whole pixels. Past `reference_distance` they shrink down to
    `min_scale`, and they scale with the HUD's UI scale.
  - `World` widgets are quads in the scene, `world_width` wide. They face
    the camera or keep the entity's facing. Each comes with a transform
    from layout units to world space and its corners. The renderer can
    draw its list through the transform, depth-tested, or into a
    texture.
  - Widgets are hidden behind the camera, past `max_distance`, off
    screen, or when not visible.
  - Each widget has its own viewport and input router, so its controls
    (even dropdowns) work as usual.
  - Bindings read the entity's components. Control events come back as
    `WidgetEvent`s for Blueprints. The UI library (SetText, PlayAnimation
    and the rest) falls back to world widgets when the entity has no
    screen one.
  - Placement uses the entity's `Transform`, or its world transform when
    given a GuidIndex.
- **Picking**: `HitTest` finds the nearest widget with something hittable
  under a pixel.
  - Screen widgets use their rectangle; World widgets use a ray against
    the quad's plane.
  - Panels let clicks through to the game.
  - `interactive` widgets take pointer moves, presses and releases,
    capture a press until it's released, and unhover when the pointer
    leaves.

### 18.6 The UI Designer

The editor for `.aui` layouts (`editor/src/uidesign/`). Like the other
editors it is built on Dear ImGui alone and runs headless in tests.

- **The document** (`UILayoutDocument`): the widget tree, its bindings
  and animations, and its file.
  - Undo is whole-document JSON snapshots. Drags and slider edits merge
    into one step.
  - Widgets are addressed by paths of child indices.
  - Tree edits:
    - Add any registered type, named `<Type><n>`. It goes only where it
      fits (a Button takes one child, a Text none).
    - Delete, not the root. Nested selections delete once. Bindings and
      tracks of the widgets that go are dropped.
    - Duplicate, renaming the copy and its children and moving a Canvas
      child 16 units.
    - Move and reorder, never into the widget itself.
    - Rename, with unique names. Bindings and tracks follow the new name.
  - Properties are edited through the widget's saved form. Setting a key
    rebuilds the widget through its registered type, keeping its
    children, so custom widget types work too. Bad values are refused
    with the loader's reason; unchanged values add no undo step. Details
    also lists the settings files leave out at their defaults.
  - Canvas placement turns a rectangle into the slot under its anchors:
    position and size on point axes, margins on stretched ones.
    Resizing ends auto-size. Changing anchors (14 presets, or any
    values) keeps the widget where it is.
  - Bindings: add, edit and remove. Each widget type lists what it can
    bind.
  - Animations:
    - add, rename and remove animations;
    - one track per widget and property;
    - keys kept in time order: a key at an existing key's time replaces
      it, and moving a key onto another replaces that one.
  - Diagnostics (cached per revision):
    - UD001 duplicate names;
    - UD002 a binding's widget missing, UD003 a property it can't bind,
      UD004 no source;
    - UD005 a track's widget missing, UD006 a property it can't animate;
    - warnings: UD007 an empty animation, UD008 two tracks on one
      property.
- **The designer** (`UIDesigner`):
  - A **palette** of registered types: click to add into the selection,
    or drag onto the canvas or the hierarchy.
  - The **hierarchy**: select, Ctrl+click for more, and drag to
    reparent, or onto a leaf to put it before that leaf.
  - The **canvas**:
    - the layout at a resolution preset (desktop, 4K, tablet, handheld,
      phones with safe areas) or a custom size, with the safe area
      outlined;
    - pan and zoom about the cursor, and fit;
    - click to select the deepest widget, then drag to move Canvas
      children and use eight handles to resize, with grid snapping;
    - the anchors drawn in the parent;
    - new widgets go into the container under the pointer, or past a
      full one, sized for their type.
  - **Details**: name, every property as a field (combos for enums,
    colours, vectors, option lists, nested brushes), the slot for the
    parent's kind, and the widget's bindings.
  - The **timeline**:
    - pick or make an animation, and add tracks;
    - key the widget's current value at the playhead;
    - drag keys in time, right-click to delete;
    - scrub on the ruler, and play in a loop.
  - It previews on a copy of the tree, with a theme and the animation
    applied, so the document stays as it will be saved.
  - **Diagnostics**: click one to select its widget.
  - Keys: Delete, Ctrl+D, Ctrl+Z/Y, Ctrl+S, F to fit, and arrows to
    nudge (Shift: by the grid).

### 18.7 PR breakdown

1. ✅ **Done.** The widget tree, the layout panels (Canvas with anchors,
   boxes, Grid, Overlay, SizeBox, Border, ScrollBox, Spacer), Text and
   Image, the draw list (9-slice, clipping, text), resolution scaling,
   safe areas and hit testing.
2. ✅ **Done.** Interactive widgets (Button, Toggle, Slider, ProgressBar,
   TextInput, Dropdown, virtualized ListView, Tooltip), and input: pointer and touch
   routing (hover, press, capture, wheel), focus with gamepad and keyboard
   navigation, and the UI input context taking input before gameplay.
3. ✅ **Done.** Styles and themes as assets (per-state brushes, fonts and
   colours), `.aui` layout files, and data binding to reflected fields.
4. ✅ **Done.** Widget Blueprints (a layout plus an event graph on the Phase 12 VM,
   with Create Widget, Add to Viewport, Set Text and events such as
   OnClicked), and UI animations (tweens and keyframe timelines of
   position, scale, opacity and colour).
5. ✅ **Done.** SDF text (font atlases from TrueType via stb_truetype), and world-space
   UI (health bars over enemies, projected or on 3D quads).
6. ✅ **Done** (portable part). The UI Designer: palette, hierarchy, canvas with selection, anchors and
   resolution preview, details, and the animation timeline.

---

## Phase 19: VFX and particles

The concept is in [ROADMAP.md Phase 19](../ROADMAP.md). The engine side is
the `Aether::VFX` library (`vfx/`). An emitter is a stack of modules, as
in Niagara or Unity's VFX Graph, and a CPU simulation runs the stack over
arrays of particle attributes. It runs and is tested headless. The GPU
path is the same stack generated as a compute shader (step 5), with the
CPU simulation as its reference and as the path for small emitters.

### 19.1 Emitters, modules and the CPU simulation

- **Values** (`curves.h`):
  - Ranges: a random value between min and max.
  - Float curves: Linear, Smooth (Catmull-Rom, flat at the ends) or
    Constant keys, clamped outside them.
  - Colour gradients: colour and alpha keys apart.
  - A seedable PCG32 generator, with points in and on spheres and
    directions in a cone.
  - Perlin noise and its curl (divergence-free swirls).
  - Each type has a compact JSON form: a number for a constant, `[min,
    max]` for a range, `{keys, interp}` for a curve, `{colors, alphas}`
    for a gradient.
- **Modules** (`emitter.h`), in three stages:
  - **Spawn**: `SpawnRate` (per second, spread evenly through the frame,
    part-particles carried over); `SpawnBurst` (a count at a time in each
    loop, repeating for a number of cycles or until the loop ends);
    `SpawnPerDistance` (along the path the emitter moved).
  - **Initialize**: lifetime; shape (point, sphere, hemisphere, box,
    cone base, circle, edge, from surface to volume by `thickness`);
    velocity (a cone around a direction, radial from the shape, or a
    direction); size; colour (a random point on a gradient); rotation
    and spin; a share of the emitter's own velocity. Directions are in
    the emitter's frame.
  - **Update**:
    - forces: gravity (world), drag, curl noise with a drifting field,
      vortex (spin and pull), point attractors (falloff, kill radius);
    - `SpeedOverLife`;
    - collision planes (bounce, friction, life lost per hit, the
      particle's radius);
    - kill volumes (sphere or box, killing inside or outside);
    - `SizeOverLife` and `ColorOverLife`, which multiply the particle's
      initial values.
    - Places are in the emitter's frame unless `world` is set.
- **Emitter settings**: name, enabled, `max_particles`, the simulation
  space (World: particles stay behind; Local: they move with the
  emitter), loop duration, looping, start delay and warmup (simulated at
  once in 1/30 s steps).
- **Assets** (`.avfx`, JSON): `{version, name, emitters}`. Each module
  saves its type and fields; the same field list writes, reads and
  checks it. Errors name where
  (`emitter 'A': update[1] (Vortex): center: a vector is [x, y, z]`).
  Checks:
  - FX001 nothing spawns (warning);
  - FX002 a lifetime that isn't positive;
  - FX003 `max_particles` outside 1 to 1,000,000;
  - FX004 a range whose min is over its max;
  - FX005 a burst bigger than `max_particles` (warning);
  - FX006 a duration that isn't positive;
  - FX007 keys out of order.
- **The simulation** (`simulation.h`):
  - `ParticleBuffer` holds one array per attribute: position, velocity,
    age, lifetime, size, rotation, spin, colour, the initial size and
    colour, a seed and an id. A death swaps the last particle in.
  - `EmitterInstance` has its clock, loops, spawning and the stack.
    - Newborns are placed along the emitter's motion during the frame
      and have lived only part of it. Forces run in stack order, then
      the move, then collisions, kill volumes and looks.
    - The same asset, seed and steps give the same particles, and
      `Restart` replays them.
    - It can `Emit` on demand, `Stop` spawning, `Clear`, and report
      bounds, spawn totals and loops.
  - `ParticleSystemInstance` plays a system's emitters together.

### 19.2 Rendering data

- **Render modules**: a fourth stage on the emitter (`render`). An
  emitter can have several: sprites and a light, for example. They are
  saved and checked like the others:
  - FX008 nothing draws the emitter (warning);
  - FX009 a flipbook with no columns or rows, or more frames than cells;
  - FX010 a mesh renderer without a mesh.
  - **`SpriteRenderer`**: material, blend (alpha, additive,
    premultiplied, opaque), sort order, aspect, soft-particle fade
    distance and camera offset. Facing modes:
    - `Camera`: a billboard, turned in its plane by the particle's
      rotation;
    - `CameraPosition`: towards the camera's position;
    - `Velocity`: along the velocity, stretched by speed;
    - `FixedAxis`: up along an axis, turning about it to the camera;
    - `FixedPlane`: flat, facing an axis.
  - **Flipbooks**: `columns` x `rows` cells, optionally fewer `frames`.
    Frames run over life (`cycles` times, holding the last frame at the
    end), at a rate by age, or one random frame per particle. With frame
    blending, the next cell and the blend amount come too.
  - **`MeshRenderer`**: mesh and material, scale times the particle's
    size, and an orientation: the particle's rotation about an axis,
    +Y along the velocity, or +Z to the camera.
  - **`RibbonRenderer`**: a strip through the particles in birth order,
    optionally joined to the emitter.
    - It is as wide as each particle's size times a scale, and spreads
      across the view or along a fixed axis.
    - u runs 0 to 1 along the strip, or repeats every `tile_length`.
  - **`LightRenderer`**: point lights at every nth particle, up to a
    cap. The radius is the particle's size times a scale, and the colour
    is the particle's or a fixed one.
- **Sorting**: none, back to front or front to back along the view
  (for blending), or oldest or newest first.
- **Draw data** (`render.h`): `BuildRenderData(instance or system,
  camera)` appends:
  - sprite batches: a centre, half-size right and up axes, colour, UV
    and next UV with a blend amount;
  - mesh batches: transforms and colours;
  - ribbon strips: vertices and triangle indices;
  - lights.
  All of it is in world space: local-space emitters are transformed.
  `ParticleCamera::FromView` takes a view matrix. `ExpandSprites` turns
  sprites into plain triangles. The renderer's particle pass draws this
  on the GPU, with the Windows renderer.

### 19.3 Events, sub-emitters, parameters and scene collision

- **Events** (`ParticleEvent`): births, deaths (with whether it was old
  age or a kill) and collisions (with the surface normal). Each carries
  the particle's world position, velocity, colour, size and id.
  - An instance keeps only the kinds asked for (`RecordEvents`), until
    its next update.
  - A system also asks for what its sub-emitters need, and gameplay can
    add more.
- **`EmitAt`**: births at a world point with added velocity, and the
  colour and size taken from the event if given. The shape still
  offsets from the point, and the emitter's own pose isn't changed.
- **Sub-emitters**: on an emitter, "when mine are born / die / hit,
  emitter X of this system emits here".
  - Settings: a count, a probability, a share of the particle's
    velocity, whether to take its colour and size, and `max_per_frame`.
  - After all emitters update, the system feeds sub-emitters from the
    frame's events. The births they cause can trigger more
    sub-emitters, so it goes round until nothing new happens; the caps
    end loops.
  - An emitter with no spawn modules of its own counts as finished when
    it's empty.
- **Parameters**: declared on the system: a name, a type (float, vector
  or colour) and a default.
  - Emitters bind module fields to them by path: `spawn[0].rate`,
    `init[2].speed` (a range becomes a constant), `update[0].acceleration`,
    `init[3].color` (a gradient becomes one colour).
  - `SetParameter` checks the type and sets each bound field on that
    instance's copy of the emitter.
  - `SetModuleField` says why a path or type is refused. Choices and
    names can't be set.
- **Scene collision** (`SceneCollision` module, `collision.h`): each
  particle's move this frame, extended by its radius, is a ray against
  the instance's `ParticleCollider`. Hits bounce with friction, can use
  up life, and are collision events.
  - `FunctionCollider` wraps any raycast (the physics world's).
  - `DepthBufferCollider` tests against a depth buffer and the
    view-projection it was drawn with, as the GPU path does. A particle
    hits when it goes behind the surface by less than a thickness. The
    normal comes from neighbouring depths, and only what's on screen
    collides.
- **System checks** (`ValidateParticleSystem`): every emitter's checks,
  prefixed with its name, plus:
  - FX011 a sub-emitter naming no other emitter;
  - FX012 sub-emitters in a loop (warning);
  - FX013 a binding to an unknown parameter, a missing field or the
    wrong type;
  - FX014 two emitters with one name.
- The file saves `parameters`, and each emitter's `sub_emitters` and
  `bindings`.

### 19.4 The ParticleSystem component and Blueprint nodes

- **`ParticleSystem` component** (`particle_system.h`):
  - Settings: the asset, auto-activate, time scale, a cull distance, a
    LOD distance with a spawn scale, and destroy-when-finished.
  - Its methods are Blueprint nodes that queue commands for the next
    update:
    - `Activate` (from the start if it had finished or never started);
    - `Deactivate` (stop spawning; what's alive lives out);
    - `Restart`;
    - `SetFloatParameter`, `SetVectorParameter`, `SetColorParameter`;
    - `SetAsset`;
    - `IsActive`.
- **`ParticleWorld`** runs a world's components:
  - It gives each one an instance, taken from a pool per asset (up to 16
    kept) and returned when the component or entity goes. Assets are
    looked up by name.
  - Instances follow their entity's world transform (through parents
    with a GuidIndex) and are stepped by time scale.
  - With a camera:
    - past the cull distance, a system is paused and not drawn;
    - past the LOD distance, all its spawning is scaled.
  - A system is finished when it won't spawn more and everything has
    died, or when it was deactivated and has emptied out. That raises
    `Event.OnParticleSystemFinished` (argument: the asset) and removes
    one-shots' entities.
  - `BuildRenderData` draws what isn't culled. Given the
    view-projection, it also skips emitters whose bounds are wholly off
    screen.
  - Missing assets, missing transforms and bad parameters are reported
    once.
- **The `Particles` library**:
  - `SpawnEmitterAtLocation(asset, location, rotation)` makes an entity
    that removes itself when done, and returns it (so its parameters can
    be set).
  - `SpawnEmitterAttached(asset, target, offset)` follows the target. If
    the target goes, it stops spawning, finishes and goes too.
- Emitters take a spawn scale (`SetSpawnScale`), which multiplies rates,
  bursts and per-distance spawning.

### 19.5 The GPU path

- **Support** (`CheckGpuSupport`): everything runs on the GPU except
  sub-emitters, light renderers and ribbons. Sub-emitters and lights
  need particle data back on the CPU; ribbons need birth order. Scene
  collisions there use the depth buffer.
- **Choosing CPU or GPU** (`ChooseSimTarget`): each emitter has a target
  of Auto, Cpu or Gpu.
  - Auto picks the GPU when the device has compute, the stack is
    supported, and `max_particles` reaches a threshold (4096).
  - Gpu falls back to the CPU when the GPU can't run the emitter.
- **The shader** (`GenerateParticleShader`): one HLSL compute shader of
  64-thread groups, with three kernels:
  - `ResetMain` (every slot free);
  - `EmitMain` (per new particle): takes a slot from the dead list with
    an atomic decrement, is placed along the frame's motion, and runs
    the Initialize modules with a PCG hash per thread;
  - `UpdateMain` (per slot): runs forces, then the move, then
    collisions and kill volumes, then looks. It puts the dead back on
    the dead list and the living on the alive list the renderer draws.
  - Enums and `world` flags are compiled in, and disabled modules
    compile out. Noise and depth code are included only when used.
  - Its key is a hash of the code, so emitters whose stacks have the
    same shape share a shader.
- **Constants**: module values aren't in the code.
  - They are float4s in a `Modules` constant buffer, packed by
    `PackModuleConstants` in the order the shader reads them, each
    named.
  - Parameters therefore change them without a recompile.
  - Curves are baked into 16-sample tables (4 float4s), gradients into
    16 float4s, and both are read back with linear interpolation.
- **Frame constants** (`PackFrameConstants`): delta time, time, spawn
  count, a frame seed, the emitter's pose, previous position and
  velocity, particle cap, and the depth buffer's size and thickness.
  Also the view-projection and its inverse, column-major as HLSL
  stores them.
- **The driver** (`GpuEmitterDriver`): the CPU half of a GPU emitter.
  - It runs the clock, loops and spawn modules with the same code as
    the CPU path (an emitter instance whose particles are counted and
    dropped each frame).
  - Each frame it gives the spawn count and frame constants.
  - It applies parameters to both the spawner and the constants.
- Dispatch belongs to the Windows renderer. The CPU simulation is the
  reference, and the path for small emitters and the unsupported
  features.

### 19.6 The particle editor

The editor for `.avfx` systems (`editor/src/vfx/`). Like the other
editors it is built on Dear ImGui alone and runs headless in tests.

- **The document** (`ParticleSystemDocument`): the asset, its file, and
  whole-document undo (drags merge into one step).
  - Emitters:
    - add (with a basic stack), duplicate, move and remove;
    - rename, keeping names unique, with sub-emitters that named them
      following.
  - Settings are edited through the emitter's saved form, including the
    ones files leave out at their defaults.
  - Modules:
    - added by name into the right stage;
    - enabled, moved and removed;
    - fields set through the module's saved form, with the loader's
      reason when a value is refused.
    - Binding paths follow module moves; bindings to removed modules
      go.
  - Parameters: add, rename (bindings follow), change defaults and
    remove (bindings go). Bindings and sub-emitters can be edited too.
  - The system's checks are cached per revision.
- **Curve and gradient keys**: added in time order (replacing a key at
  the same time), moved, and removed, never the last one. Times are
  clamped to 0..1.
- **The preview** (`ParticlePreview`): the system on the CPU from a
  fixed seed in 1/60 s steps.
  - Seeking replays from the start, so the timeline scrubber always
    shows the same particles. Seeks stop at 60 s.
  - An orbit camera (yaw, pitch, distance) with projection.
  - Per-emitter stats: particles, spawned, bounds, CPU time, and
    whether it would run on the CPU or GPU.
- **The editor** (`ParticleEditor`):
  - Emitter list: enable, add, duplicate, remove, reorder.
  - The stack: four stages of modules with enable boxes, move and
    remove buttons, and an add menu per stage.
  - Details:
    - curves have a plot (click to add a key, drag to move, right-click
      to remove), an interpolation choice and numeric keys;
    - gradients have a preview bar and colour and alpha keys;
    - ranges are edited as min and max; enums are combos;
    - the emitter's settings, CPU/GPU placement with the reasons,
      sub-emitters and bindings.
  - The preview:
    - drag to orbit, wheel to zoom, a ground grid;
    - sprites, meshes, ribbons and lights drawn from their render data;
    - play, pause, restart, speed and looping, and a time slider that
      scrubs;
    - edits show at once, at the same moment of the preview.
  - Parameters, Stats and Diagnostics tabs.
  - Keys: Space, Delete, Ctrl+Z/Y, Ctrl+S.
- The editors share their JSON field widget (`core/json_edit.h`) with
  the UI Designer.

### 19.7 PR breakdown

1. ✅ **Done.** Curves, gradients, noise and randomness; the emitter's module
   stacks (spawn, initialize, update) and their asset format and checks;
   the CPU simulation.
2. ✅ **Done.** Rendering data: sprites and billboards (camera-facing, velocity-aligned,
   fixed axes), flipbook animation, sorting, soft-particle parameters,
   mesh particles, ribbons and trails, and particle lights, as instance
   and vertex data for the renderer.
3. ✅ **Done.** Events and sub-emitters (on birth, death and collision), collision
   against the depth buffer and physics, and user parameters that modules
   read (Set Parameter).
4. ✅ **Done.** The `ParticleSystem` component and system (pooling, culling by distance
   and bounds, LOD), and Blueprint nodes (Spawn Emitter at Location or
   Attached, Set Parameter, OnSystemFinished).
5. ✅ **Done** (portable part). The GPU path: the stack generated as a compute shader with the CPU
   simulation as its reference, and the choice between them per emitter
   (dispatch with the Windows renderer).
6. ✅ **Done** (portable part). The editor: the emitter stack with module details, curve and gradient
   editors, and a live preview with a timeline scrubber and per-emitter
   stats.

## Phase 20: AI and navigation

The concept is in [ROADMAP.md Phase 20](../ROADMAP.md). The engine side is
the `Aether::Nav` library (`nav/`). Navigation meshes are baked with
Recast and queried with Detour (recastnavigation 1.6, zlib licence,
fetched by CMake). Crowds, Behavior Trees and perception build on them.
It all runs and is tested headless.

### 20.1 Baking and path queries

- **Geometry** (`NavGeometry`): triangles with an area per triangle:
  - area 0 is not walkable, 63 is plain ground, and 1–62 are the game's
    own (water, grass, road);
  - helpers add planes and boxes;
  - step 2 gathers it from the scene's colliders and static meshes.
- **Settings** (`NavMeshSettings`), for one kind of agent:
  - the voxel size (cell size and height);
  - the agent's height, radius, maximum climb and maximum slope;
  - region sizes: small islands are dropped and small regions merged;
  - edge length and error, and vertices per polygon;
  - the detail mesh's sampling;
  - the tile size in cells (0 is one tile over everything).
- **Baking** (`BuildNavMesh`):
  - The world is cut into square tiles on a grid from the geometry's
    corner. Each tile is baked apart, with a border of the agent's
    radius plus three cells so the tiles meet cleanly.
  - Per tile, the Recast pipeline runs in order:
    1. rasterize the triangles that touch it, walkable by slope and
       keeping their areas;
    2. filter low obstacles, ledges and low ceilings;
    3. erode by the agent's radius;
    4. build the distance field and regions, then contours;
    5. build the polygon mesh and the detail mesh;
    6. make Detour's tile data.
  - Empty tiles are skipped. The stats count tiles, polygons, vertices
    and time.
  - Failures give a reason: no geometry, bad settings, too many tiles,
    or nothing walkable.
  - `BuildNavTile` rebakes one tile of an existing grid (used for
    obstacles in step 2).
- **Files**: `.anav` holds the magic `ANAV`, a version, the settings, the
  grid and each tile's Detour data. Loading refuses files that are cut
  short, foreign or from a newer version.
- **Queries** (`NavMesh`, over `dtNavMesh` and `dtNavMeshQuery`):
  - `FindPath` gives the corners of the shortest path from the points on
    the mesh nearest the start and the end:
    - **Complete** when it reaches the goal;
    - **Partial** when the goal is cut off, ending as near as it gets;
    - it fails (status **None**) with no start or goal on the mesh.
  - `NearestPoint` gives a point and its area.
  - `Raycast` walks a straight line over the mesh. It stops at a wall,
    the mesh's edge or an excluded area, and gives where and the wall's
    normal.
  - `RandomPoint` is weighted by area and seeded, so it's repeatable.
  - `RandomPointNear` stays within the radius and connected to the
    centre.
  - `Reachable` is true when a complete path joins the two points.
  - `ReplaceTile` swaps (or, when empty, removes) one tile, and the
    mesh's data follows so saving keeps it.
  - `Polygons` lists each polygon with its area and tile, for the
    editor's overlay.
- **Filters** (`NavQueryFilter`): a cost per area, which multiplies
  distance, and excluded areas. An excluded area is never entered, by
  paths, raycasts or random points: Detour is built with a virtual query
  filter for this.

### 20.2 Obstacles, areas, links and the scene

- **Volumes** (`NavVolume`) mark the ground inside them with an area:
  - shapes: a box turned about Y by a yaw, an upright cylinder, or a
    convex prism (points on the ground, between two heights);
  - area 0 carves a hole (an obstacle). Carving happens before the agent's
    radius is eroded, so agents keep their radius clear of obstacles as
    they do of walls;
  - any other area relabels the ground (water, a road), marked after
    erosion so it doesn't shrink the mesh.
- **Off-mesh links** (`NavLink`): a jump, ladder or drop from a start to an
  end:
  - each end must be within the link's radius of the mesh;
  - a link is both ways or one way;
  - it has an area whose cost and exclusion the filter applies, and a
    user id;
  - a link is stored in the tile where it starts (not in its border), and
    Detour joins it to the end's tile.
  - Path points flag where a link starts (`kNavPointLinkStart`), so an
    agent knows when to jump or climb.
  - `NavMesh::Links` lists the baked links (for the overlay).
- **Baked in**: `NavGeometry` carries static volumes and links, and
  `BuildNavMesh` bakes them in.
- **Changes at runtime** (`DynamicNavMesh`):
  - Volumes and links are added, updated and removed by id. An update
    that changes nothing costs nothing.
  - Each change marks dirty the tiles it can affect: those it overlaps,
    widened by the tile border (`NavTilesTouching`). A move marks both its
    old and new places; a link marks both its ends.
  - `Update(max_tiles)` rebakes that many dirty tiles (0 is all), with the
    static geometry plus every current volume and link. It spreads the
    work over frames.
  - `Revision` goes up whenever tiles change, so agents can replan.
  - `Load` starts from a saved `.anav` and keeps the geometry for later
    rebakes. Anything added before loading is baked by the first Update.
  - The mesh's data follows the rebakes, so saving keeps them.
- **Scene components**:
  - `NavObstacle` carves with the entity. The shape is a box (turned by
    the entity's yaw) or a cylinder, with an offset. It rebakes once the
    entity has moved `move_threshold` or turned 5 degrees; shape edits
    apply at once. With `carve` off, it makes no hole (agents steer round
    it in step 3).
  - `NavModifierVolume` sets an area over a box.
  - `NavLinkProxy` is a link with both ends in the entity's space; it can
    be enabled, and has a direction, area and user id.
- **`NavWorld`** runs a world's navigation:
  - `Gather` collects the static geometry: every `ModelRenderer`, through
    a provider that turns a model into triangles, placed by the entity's
    world pose (parents too, given a GUID index). Carving obstacles are
    left out.
  - `Bake` and `Load` build or load the mesh together with the current
    volumes and links.
  - Each `Update` follows the components (added, moved, edited,
    disabled, destroyed or recycled entities), then rebakes a budget of
    tiles.
  - The last `NavWorld` made is the active one, for step 3's nodes.

### 20.3 Agents and crowds

- **`NavAgent`** walks its entity over the mesh. It is steered by
  Detour's crowd, which:
  - anticipates corners;
  - optimizes the path by visibility and topology;
  - avoids other agents, at four qualities (0 is none);
  - separates it from neighbours by a weight.
- **Settings**:
  - the agent's radius, height, maximum speed and acceleration;
  - a stopping distance and a base offset above the mesh;
  - turning to face where it goes, at a turn speed;
  - `allow_partial`, and `goal_tolerance` (how far off the mesh a goal
    may be).
- **Methods** (Blueprint nodes, and Luau methods on the component):
  - `MoveTo` a point;
  - `MoveToEntity`, which follows the target: it replans once the target
    has moved 0.5 m (or the stopping distance), and fails if the target
    goes;
  - `Stop` and `Warp` (onto the mesh);
  - pure queries: `IsMoving`, `HasArrived`, `HasFailed`, `GetVelocity`,
    `GetSpeed`, `GetRemainingDistance` and `GetGoal`.
  - They queue commands. The next update applies them, and keeps them
    until there is a mesh.
- **Status** is `Idle`, `Moving`, `Arrived` or `Failed`:
  - A move fails at once when the goal is cut off, or lies more than
    `goal_tolerance` from the mesh (inside an obstacle, say), unless
    `allow_partial`; with it, the agent walks as near as it gets.
  - It arrives within the stopping distance of the goal (on the mesh).
  - Each arrival or failure is an `Event.OnMoveCompleted (success)` for
    Blueprints. Stopping isn't completing.
- **`NavCrowd`** runs a world's agents on its `NavWorld`:
  - An agent is added where its entity is, snapped to the mesh, and
    removed when its entity or component goes.
  - Settings apply every update.
  - When tiles change (the navigation revision moves), moving agents
    replan, so obstacles that appear are walked round and goals that
    are walled in fail.
  - A new bake or load makes a new crowd. Agents are added again, and
    moving ones carry on.
  - It writes the agents' Transforms (as world space) and turns them.
  - `Corners` gives an agent's next corners, for debugging.
- **`Navigation`** is a Blueprint library over the active navigation
  world:
  - `IsReachable`, `PathLength` (-1 without a complete path);
  - `ProjectPoint`, `IsOnNavMesh`;
  - `RandomReachablePoint` within a radius, and `Raycast` (where a
    straight walk stops).

### 20.4 Blackboards and Behavior Trees

The `Aether::AI` library (`ai/`), on top of `Aether::Nav`.

- **Blackboards**:
  - typed keys: Bool, Int, Float, String, Vector, Entity; each can have
    an initial value and a description;
  - `Set` refuses undeclared keys and the wrong types;
  - without keys (before a tree is known), any name can be set. `Adopt`
    then keeps the values that fit the tree's keys and starts the rest
    at their initial values;
  - revisions (overall and per key) move only on a real change;
  - clearing is a change too.
- **Assets** (`.abt`, JSON): `{version, name, blackboard: [{name, type,
  initial}], root}`. Each node has a type, a name, decorators, services,
  children and its own settings. Values are JSON typed by their key.
  Load errors say where (`root/Selector[1]: unknown node type 'Dance'`).
- **Composites**:
  - **Selector** tries its children in order until one succeeds;
  - **Sequence** runs them in order until one fails;
  - **Parallel** runs them all at once: it succeeds when all do (or
    when one does, with `succeed_on_one`), and fails the other way.
    When the result is decided, the rest are aborted.
- **Decorators**:
  - **BlackboardCondition**: IsSet/IsNotSet, Equal/NotEqual, and
    Less/LessEqual/Greater/GreaterEqual for numbers. A null entity
    counts as unset. Conditions gate entering, and can abort:
    - **Self**: the running branch ends when the condition turns false;
    - **LowerPriority**: under a Selector, a running later sibling is
      cut off when this branch's conditions come true;
    - **Both**.
  - **Cooldown**: can't run again for so many seconds after it ends or
    is aborted.
  - **Loop**: runs again on success, `count` times (0 is forever), from
    the next tick.
  - **TimeLimit**: fails after so many seconds.
  - **Inverter**, **ForceSuccess**, **ForceFailure**.
- **Services** run while their node is active: at once, then every
  interval. They are a Blueprint event, a Luau function, or
  **DistanceTo** (a Float key set to the distance to a Vector or
  Entity key; cleared when the target goes).
- **Tasks**:
  - **Wait** (seconds, plus or minus a seeded deviation).
  - **MoveTo**: sends the entity's NavAgent to a Vector or Entity key
    within an acceptance distance (which becomes the agent's stopping
    distance). It re-issues the move when a Vector goal changes, and an
    abort stops the agent. It fails without an agent or a goal.
  - **SetBlackboard** and **ClearBlackboard**.
  - **RunBlueprint**: dispatches `Event.Custom:<event>` and waits until
    `Behavior Trees > Finish Task (entity, success)`, which may be
    called during the dispatch itself. An abort is reported to a hook.
  - **RunLuau**: calls `<event>(entity, dt, first)` every tick, which
    returns "running", "success" or "failure".
  - **Log**, **Succeed** and **Fail**.
- **Running a tree** (`BehaviorTreeInstance`):
  - Each tick walks from the root down the running branch, re-checking
    conditions that abort, and runs services and time limits.
  - When the root ends, the tree starts over on the next tick.
  - Hooks (`BtHooks`) carry logging and the Blueprint and Luau tasks
    and services, so the library needs neither.
  - For the debugger: the flattened nodes with paths, which are active,
    each node's last status, the active path and the deepest active
    label.
- **Checks**:
  - BT001 a composite without children;
  - BT002 a task with children;
  - BT003 an unknown key;
  - BT004 a value or comparison that doesn't fit its key;
  - BT005 a one-child Parallel (warning);
  - BT006 a LowerPriority abort outside a Selector (warning);
  - BT007 a negative time or count, or an interval that isn't positive;
  - BT008 a Blueprint or Luau task or service without an event;
  - BT009 a MoveTo or DistanceTo key that isn't a Vector or Entity;
  - BT010 a duplicate key.
- **`BehaviorTreeComponent`**: the tree, `auto_start`, and a tick
  interval (time still adds up between ticks).
  - Blueprint and Luau methods: Start, Stop, Restart (applied on the
    next update), IsRunning, GetActiveNode, and blackboard getters and
    setters per type, ClearValue and IsValueSet (at once).
  - `BehaviorTreeWorld` runs them: an instance per component, a new
    tree when `tree` changes, and a missing tree reported once.
    Destroyed entities drop their trees.
  - `BehaviorTrees.FinishTask` goes to the active world.

### 20.5 Perception

- **`AIPerception`** is an AI's senses:
  - **Sight**:
    - a sight radius to notice within, and a larger lose-sight radius
      to keep tracking a seen actor within;
    - a view cone (the whole angle, about the entity's +Z, measured on
      the ground; 360 sees all round);
    - eyes at `eye_height`, aiming at each source's `target_height`;
    - line of sight through a callback, or over the navigation mesh
      (`UseNavMeshLineOfSight`: a mesh raycast between the two, so
      walls block and so do the gaps the agent radius leaves round
      them).
  - **Hearing**: noises within `hearing_radius` × loudness, from any
    direction.
  - **Damage**: the victim learns who hit it and where they are.
  - **Teams**: 0 senses everyone. The AI's own team is ignored unless
    `detect_friends`.
- **Memory**: each known actor keeps its last location, age (seconds
  since last sensed), whether it's visible now, the last sense, its
  strength (the loudness or damage) and its distance.
  - It is forgotten after `forget_after` seconds out of sense (0 is
    never), or at once when it's destroyed.
  - `GetTarget` gives what it's most aware of: the nearest it can see,
    else what it sensed most recently.
  - Blueprint and Luau methods: CanSee, IsAwareOf,
    GetLastKnownLocation, GetTarget, GetKnownCount and ForgetAll.
- **`AIStimuliSource`**: what can be seen (visible, target height,
  team). Hearing and damage don't need one.
- **`Perception`** is a Blueprint library: `ReportNoise(location,
  loudness, instigator)` and `ReportDamage(victim, instigator, amount)`.
  An AI doesn't hear its own noises, nor ones nobody made.
- **`PerceptionWorld`** updates every AIPerception and reports events:
  - `Event.OnTargetPerceived (actor, sense, sensed)` when an actor is
    first seen (not every frame), lost from sight, heard or felt;
  - `Event.OnTargetForgotten (actor)`.
- **The blackboard**: with `target_key` (an Entity key) and
  `location_key` (a Vector key), the AI writes its target and where it
  was last sensed to its Behavior Tree's blackboard. Both are cleared
  once it knows nothing, so a condition on the key switches the tree
  between chasing and patrolling.

### 20.6 The editor

- **Debug drawing** (plain world-space lines and triangles, tested
  headless; the viewport draws them):
  - **navigation** (`nav/debug_draw.h`):
    - the mesh's polygons filled by area (walkable is blue, other areas
      a steady colour each) and outlined, lifted off the ground;
    - the tile grid;
    - links as arcs (one-way ones with an arrowhead) with circles at
      their ends;
    - obstacles (red) and area volumes as wireframes, both static and
      dynamic;
    - paths.
  - **AI** (`ai/debug_draw.h`):
    - each agent's corners ahead and its goal;
    - each perception's sight cone (a circle for all-round vision) and
      hearing range;
    - a line to what it sees now, and a dimmer one to where it last
      sensed the rest.
- **The Behavior Tree document**:
  - nodes addressed by their path of child indices; children only under
    composites;
  - moves, which refuse the root and moving a node under itself, and
    keep the target's index right when it's a later sibling;
  - duplicates, removal, and type changes (a node with children stays a
    composite);
  - fields, decorators and services edited through their saved form, so
    whatever doesn't load is refused with the loader's reason;
  - blackboard keys with unique names:
    - renames follow every reference (tasks, conditions, services);
    - a type change clears the values that no longer fit;
    - initial values and descriptions;
    - which nodes use a key;
  - checks cached per revision, whole-document undo (drags merge), and
    files.
- **The Behavior Tree editor**:
  - The tree is laid out automatically: leaves in slots left to right,
    parents centred over their children, a row per depth. Node ids are
    the depth-first order that `BehaviorTreeInstance` uses.
  - Nodes show a title (with their settings) and their decorators and
    services as notes; errors show on their nodes.
  - Edits in the graph:
    - a wire from a composite to a node moves the node under it;
    - dragging a node sideways reorders it among its siblings;
    - Delete removes (never the root, and a node with its descendants
      once);
    - Ctrl+D duplicates;
    - breaking a wire is refused with a hint.
  - The palette offers composites and tasks under a composite.
  - Panels:
    - the Blackboard (keys, types, initial values, uses);
    - Details (name, type, fields, decorators with ordering, services);
    - Diagnostics (a click selects the node).
  - The debugger, given a live instance: active nodes highlighted and
    their wires lit, finished nodes green or red, the active path, and
    the blackboard's values.
  - Undo, Redo and Save are on the toolbar and keys.
- **The Navigation panel**:
  - the bake settings (edited as JSON), Bake and Rebuild All;
  - the mesh's stats: tiles and grid, polygons, tiles waiting,
    rebakes, revision, volumes, links and agents;
  - the overlay's toggles;
  - `BuildOverlay` gives the viewport the navigation and AI debug
    drawing.
- Like the other editors, these are hooked into the editor window with
  the Windows build.

### 20.7 PR breakdown

1. ✅ **Done.** Navmesh baking from geometry with Recast (tiles, agent
   settings, areas) and Detour queries (paths, nearest points, raycasts,
   random points, area costs and exclusion, `.anav` files, tile
   replacement).
2. ✅ **Done.** Runtime tile rebuilds for dynamic obstacles (boxes and cylinders that
   carve), off-mesh links (jumps, ladders, drops), area volumes that
   mark areas, and gathering the geometry from the scene.
3. ✅ **Done.** The `NavAgent` component on Detour's crowd (steering, local avoidance,
   the agent moving the entity's transform), and Blueprint and Luau
   nodes (Move To, Stop, Find Path, Random Reachable Point).
4. ✅ **Done.** Blackboards (typed keys) and Behavior Trees:
   - composites: Selector, Sequence and Parallel;
   - decorators: Blackboard condition, Cooldown, Loop and TimeLimit;
   - services;
   - tasks: Move To, Wait, Play Anim, and Blueprint or Luau tasks;
   - their assets and checks.
5. ✅ **Done.** `AIPerception`: sight (a cone plus a line-of-sight raycast), hearing
   (noise events), damage, and forgetting. Events go to Blueprints.
6. ✅ **Done** (portable part). The editor:
   - a navmesh overlay in the viewport;
   - the Behavior Tree graph editor;
   - a BT debugger that shows the active path during Play in Editor.

## Phase 21: World building

The concept is in [ROADMAP.md Phase 21](../ROADMAP.md). Two libraries make
up the engine side:
- `Aether::Terrain` (`terrain/`): heightmap terrain, splatmap layers,
  brushes, foliage and splines;
- `Aether::Streaming` (`streaming/`): world partition, cell streaming and
  the floating origin.

Both build and test headless. Steps 1–5 landed on master directly; they
were then repaired so they build and match their documentation (#96,
#97).

### 21.1 Heightmap terrain

- `Heightmap`: samples on a grid with a cell size, and bilinear sampling.
- Procedural heights are fractal value noise (deterministic, smooth,
  in -scale..scale).
- `TerrainData` splits the heightmap into square chunks of `chunk_size`
  vertices. Chunks share their edge vertices, so N samples need
  ceil((N - 1) / (chunk_size - 1)) chunks per side.
- Each chunk's mesh comes from its own part of the heightmap. A vertex
  is position, normal (central differences) and UV: 8 floats.
- Indices form a triangle list. A LOD level takes every 2^lod-th sample,
  and the LOD is picked by distance.

### 21.2 Splatmaps and brushes

- **Splatmaps**: up to four layers as RGBA8 weights, with bilinear
  sampling over UV.
- **`BuildSplatmap`**:
  - with no weights, the base layer everywhere;
  - with one weight per layer, a constant mix;
  - with one weight per layer per pixel, a painted map.
- **Brushes**: full strength inside a core, fading smoothly over the
  outer `falloff` share of the radius (0 is a hard edge).
  - Painting raises the target layer and lowers the others, then
    renormalizes.
  - Height brushes: raise or lower by up to `vertical_scale` units at
    full weight (a negative strength lowers); smooth (from the heights
    before the stroke); flatten (toward the height under the center).

### 21.3 Terrain rendering

- `TerrainRenderer` keeps a render chunk per terrain chunk:
  - it uploads vertex and index buffers through a `TerrainGpu`
    (create and destroy a buffer), which the renderer implements over
    the RHI;
  - it rebuilds dirty chunks, and re-uploads a chunk whose LOD changes;
  - it frees everything when destroyed;
  - it estimates VRAM.

### 21.4 Foliage

- Foliage types each have:
  - a mesh and LOD and impostor distances;
  - a scale range and Y-rotation range;
  - alignment to the ground normal, and an anchor offset.
- **Generation**: each sample point is kept with the density map's
  probability under it (1 m cells), up to `max_instances`. Its type is
  picked at random.
- **Painting**: `layer.density` instances per m² are scattered uniformly
  over the brush disc and thinned by the map. The eraser removes
  instances within a radius.
- Instances can be filtered by chunk, sorted by type for instanced
  draws, and bounded.

### 21.5 Splines

- Catmull-Rom splines whose control points carry a width and a roll.
  They give positions, frames, arc lengths, uniform samples and closest
  points.
- **Meshes**: a strip with `verts_per_segment` cross-sections per
  segment. Road markings give each strip vertex a dash flag (painted for
  the first half of every dash length) and its distance along the road.

### 21.6 Large worlds

- **Partition**: a level is split into square grid cells on the ground
  plane (`cell_size`, from an origin).
  - A root entity with a Transform goes to the cell its position is in.
  - Its descendants, found through Parent GUIDs, go with it.
  - Entities with no Transform, AlwaysLoaded ones, and streaming
    sources go to the persistent scene.
  - Cells are additive binary scenes. `CopyEntity` moves an entity
    between worlds through each component's serializer.
- **The index** (`world.aworld`, JSON): the cell size and origin, the
  persistent entity count, and each cell's entity count and position
  bounds.
  - Files: `persistent.aesc`, plus `cells/<x_z>.aesc`.
  - Saving again removes stale cell files.
- **Streaming** (`WorldStreamer`):
  - `StreamingSource` components (a radius) say where to load.
  - A cell loads once it's within a source's radius, nearest first, at
    most `max_loads_per_update` per update.
  - It unloads once it's beyond every source's radius × `unload_margin`
    (1.25), so standing on a border doesn't thrash. At most
    `max_unloads_per_update` unload per update.
  - A pinned cell stays loaded regardless.
  - A cell that can't be read is reported once and not retried.
  - Loading adds the cell's entities to the world (and the GUID index),
    so parents resolve across cells.
  - Unloading destroys the entities it added that still exist; changes
    to them aren't kept. Entities spawned at runtime aren't touched.
  - The events say which cells loaded or unloaded and how many entities
    they held.
- **Floating origin**: once the focus is more than `threshold` from the
  origin on the ground plane, every root Transform shifts back by whole
  `step`s.
  - The offset (a double-precision whole-world position) grows by the
    same amount.
  - The streamer works in whole-world positions through the offset, and
    shifts the cells it loads into local positions.

### 21.7 The world-building editors

- **Terrain** (`TerrainEditDocument`, `TerrainToolPanel`):
  - Tools: Raise, Lower (`raise_height` per full-weight dab), Smooth,
    Flatten and Paint (a splatmap layer).
  - The brush has a radius, strength and falloff, and paint radii are in
    world units.
  - A stroke is a series of dabs and is one undo step. It stores only
    the rectangle of heights it changed, or the splatmap for paint, and
    a stroke that changes nothing isn't kept.
  - Touched chunks wait in a dirty set for the renderer. Normals reach
    one sample out, so a dab on a shared edge dirties both chunks.
  - The brush cursor is a ring that follows the ground.
  - The viewport drives the panel with `Pointer(down, x, z)`. Dabs are
    spaced a quarter radius apart along a drag, so the drag speed
    doesn't change the result.
- **Foliage** (`FoliagePaintDocument`, `FoliagePanel`):
  - paint or erase over a radius; painted instances are set on the
    ground through a height sampler, plus their type's anchor offset;
  - one undo step per stroke;
  - types edited with undo (removing one removes its instances and
    renumbers the rest);
  - density, and an instance cap that truncates;
  - counts per type.
  - Painting dabs every half radius along a drag.
- **Splines** (`SplineEditDocument`, `SplinePanel`):
  - add a point, insert one halfway after another, remove, move (a drag
    is one undo step), and set width and roll;
  - picking the nearest point within a radius;
  - the curve and the road's edges for the viewport.
  - In the viewport, Ctrl+click adds a point, a click picks one, and a
    drag moves it.
- **World partition map** (`WorldPartitionPanel`):
  - every cell of the index, coloured by state (loaded, unloaded,
    failed), with pins outlined and entity counts;
  - the streaming sources and their reach;
  - stats and problems.
  - Clicking a cell pins or unpins it, and hovering it shows its
    details.
- Like the other editors, these are hooked into the editor window with
  the Windows build.

### 21.8 PR breakdown

1. ✅ **Done.** Heightmap terrain, chunks and LOD.
2. ✅ **Done.** Splatmaps and brushes.
3. ✅ **Done** (portable part). Terrain rendering through `TerrainGpu`
   (the RHI implementation with the Windows renderer).
4. ✅ **Done** (portable part). Foliage generation and painting (GPU
   culling with the renderer).
5. ✅ **Done.** Splines, meshes along them and road markings.
6. ✅ **Done.** World partition, cell streaming and the floating origin.
7. ✅ **Done** (portable part). The editor:
   - the terrain sculpt and paint panel with a viewport brush cursor;
   - the foliage panel;
   - spline editing;
   - the world partition map (cells loaded, entity counts, and the
     sources' reach).

## Phase 22: Networking and multiplayer

✅ **Done** on the engine and portable-editor side.

The concept is in [ROADMAP.md Phase 22](../ROADMAP.md). The engine side
starts with the `Aether::Net` library (`net/`).

The transport is our own small protocol, in the manner of ENet and
yojimbo, over a datagram socket interface. That way:
- the same code runs over UDP and over an in-memory network that
  simulates latency, jitter, loss and duplication;
- tests are deterministic;
- the editor gets simulated bad networks for free.

### 22.1 The transport

- **Sockets** (`DatagramSocket`):
  - `UdpSocket`: non-blocking, POSIX or WinSock, bound to a port (0 for
    any free one).
  - `LoopbackNetwork`: sockets on 127.0.0.1 ports in one process.
    Datagrams are delivered at now + latency + random jitter (so they
    can reorder), lost, or duplicated, by seeded chance. Time moves
    only with `Advance`, and datagrams to a closed port are dropped as
    with UDP.
- **Addresses**: IPv4 and a port, parsed from and printed as
  `a.b.c.d:port`.
- **Connections** (`Connection`):
  - Packets: a 16-bit sequence number (wrapping) and acks of the newest
    packet received plus a bit for each of the 32 before it. Duplicate
    packets are dropped. Payloads are at most 1200 bytes.
  - Channels, by type:
    - **ReliableOrdered**: messages ride packets until one carrying them
      is acked, resent after max(0.1 s, RTT × 1.25 + 20 ms), delivered
      once and in order. A window of 256 messages per channel is in
      flight, and messages larger than 1024 bytes are split into
      fragments and joined (up to `max_message`, 256 KB).
    - **Unreliable**: messages go once and fit one packet.
    - **UnreliableSequenced**: unreliable, and never older than one
      already delivered (for state).
  - Queues are bounded per channel. Sends that don't fit are refused,
    as are messages to a channel that doesn't exist.
  - Stats: RTT (smoothed from acks), packet loss (a packet unacked
    after max(1 s, 4 × RTT) counts as lost), packets and bytes, and
    resends.
- **Hosts** (`NetHost`):
  - A server listens and accepts peers up to `max_peers`, refusing the
    rest. A client connects with a nonce, retrying every 0.25 s until it
    is accepted, refused, or 5 s pass.
  - A lost Accept is sent again on a repeated request. A new request
    from a known address (a restarted client) replaces the old peer.
  - Connected peers keep alive (at least every 0.25 s) and time out
    after 5 s of silence.
  - Goodbyes are sent three times, unacknowledged.
  - Packets with another protocol id are ignored.
  - Events: Connected, Disconnected (with a reason: Local, Remote,
    Timeout, Denied, ConnectFailed) and Message (peer, channel, bytes).
  - Send, Broadcast (with an exception) and peer info, including the
    connection's stats.

### 22.2 Replication

The server is authoritative. It sends each client snapshots of the
networked entities, in the manner of Quake 3 (`ReplicationServer` and
`ReplicationClient`, `net/include/aether/net/replication.h`).

- **What replicates**:
  - Entities with a `NetIdentity`: a net id (the server assigns it), an
    owner (the server's id for a client, or none), an archetype name,
    a relevancy radius and a priority.
  - Their components' `Field_Replicated` fields, in reflection's binary
    form. `Transform` always replicates whole.
  - Components are named on the wire by an FNV-1a hash of their name,
    and fields by their index among the replicated ones (up to 32).
- **Snapshots**:
  - Every `send_interval` (20 Hz), each client gets one on an
    UnreliableSequenced channel. It holds a snapshot id, a baseline id,
    the server time, despawns, and updates.
  - An update holds, per component, a mask of the fields that differ
    from the baseline and their bytes. A mask of 0 means the component
    was removed. New entities carry everything, plus their owner and
    archetype.
  - The baseline is the newest snapshot the client acknowledged (on an
    Unreliable channel). The server remembers 64 snapshots per client
    as "the baseline plus what was sent". A lost snapshot therefore
    costs only latency, and an idle world costs a 17-byte header.
  - The client rebuilds each snapshot from its own copy of the baseline.
    It drops one whose baseline it has forgotten. It writes only the
    fields that changed since the snapshot it last applied.
- **Spawning**: the client creates an entity with the `NetIdentity`,
  then runs the `OnSpawn` hook for its archetype (or the "" hook), which
  adds what isn't replicated (a mesh, a collider). Then it writes the
  fields. `locally_owned` is set when the owner is this client.
  Despawns are explicit, and an `OnDespawn` hook runs first.
- **Relevancy**: an entity goes to a client when its radius is 0, the
  client owns it, or it is within its radius of the client's viewer.
  The viewer is set with `SetViewer`, or else is the Transform of an
  entity the client owns; with neither, everything is relevant. An
  entity that stops being relevant is despawned on that client.
- **Bandwidth**: each snapshot fits a byte budget (1000 bytes, at most
  one unreliable message). Entities with changes add their priority to
  an accumulator each snapshot and are sent highest first. Those that
  don't fit wait, with their accumulators growing, so everything gets
  through in the end and important entities keep up best.
- **Known limits**:
  - A snapshot with no baseline (one that falls out of the 64 kept)
    that the budget cuts short leaves out entities. The client then
    drops them until they are sent again.
  - The server keeps a full copy of the state per remembered snapshot;
    sharing it is for later.

### 22.3 Remote calls

Remote procedure calls in the manner of Unreal's RPCs
(`net/include/aether/net/rpc.h`, `engine/include/aether/ecs/remote_call.h`).

- **Flags** on reflected member functions:
  - `Fn_Server`: a client asks the server to run it.
  - `Fn_Client`: the server runs it on the owning client.
  - `Fn_Multicast`: the server runs it, and so does every client that
    has the entity.
  - `Fn_Unreliable`: may be lost. The default is reliable and in order.
- **One path for every caller**: C++ calls go through `CallFunction`,
  and the Blueprint VM's native calls and Luau's method calls now do
  too. `CallFunction` asks the world's `RemoteCallRouter`:
  - **RunLocally**: run it here;
  - **Sent**: it went over the network, and nothing runs here;
  - **Refused**: not allowed. Blueprints report BP204, and Luau raises
    "was refused".

  A world without a router runs everything where it is called, as a
  single-player game would.
- **`net::RpcRouter`**: one on the server and one per client. Each
  installs itself as its world's router and removes itself when
  destroyed.

  | Called on | `Fn_Server` | `Fn_Client` | `Fn_Multicast` |
  |---|---|---|---|
  | the server | runs | sent to the owner (runs here if nobody owns it) | runs here and is sent to every peer whose last snapshot had the entity |
  | a client | sent if this client owns it (`locally_owned`), refused otherwise | runs here | runs here only |

- **Checks on arrival**:
  - The server runs only `Fn_Server` calls, and only from the entity's
    owner. Others are rejected and logged.
  - Clients run only client and multicast calls.
  - Calls for an unknown entity, component or function are counted, as
    are malformed ones.
  - `Caller()` names the sender while a call runs.
- **Wire format**: kind 3, then the net id, an FNV-1a hash of the
  component name and of the function name, the argument count, and
  each argument in reflection's binary form with a 16-bit length. Calls
  go on the reliable channel, or on the unreliable one for
  `Fn_Unreliable`. Arguments must match the parameters exactly.
- **Return values**: remote calls send nothing back.

### 22.4 Prediction and interpolation

Things a client owns are predicted. Everything else is shown slightly in
the past, between two snapshots (`net/include/aether/net/prediction.h`,
`interpolation.h`).

- **Snapshot interpolation** (`SnapshotInterpolation`):
  - On each snapshot, every replicated entity's authoritative Transform
    is read from the snapshot (`ReplicationClient::ReadReplicated`, not
    from the world, which shows interpolated values) and pushed into
    its `InterpolationBuffer` at the snapshot's server time. A buffer
    holds 32 samples.
  - Each frame, entities are shown at render time = the estimated
    server time − `delay` (0.1 s, two snapshots at 20 Hz). Position is
    lerped and rotation nlerped along the shorter arc. Outside the
    buffer, the oldest or newest sample holds; the frames where that
    happens are counted as starved.
  - The server-time estimate follows the snapshots' arrival, smoothed
    (it snaps when off by more than 0.25 s), and the render time never
    runs backwards.
  - Entities this client owns are left alone, and buffers go with their
    entities.
- **Predicted movement**:
  - `NetMovement`: velocity and grounded replicate; speed,
    acceleration, jump speed, gravity and ground height don't.
  - `MovementInput`: a sequence number, dt, a move direction and jump.
  - `StepMovement` is the deterministic rule both sides run: horizontal
    velocity eases towards the input's, then jump, gravity and landing.
    `MovementServer::SetStep` and `MovementClient::SetStep` replace it,
    for example with a physics character controller.
  - **`MovementClient::Predict`**:
    - runs the step at once and keeps the input;
    - sends the last 8 unacknowledged inputs on an unreliable channel,
      so losses are covered by redundancy;
    - registers Transform and NetMovement with `PredictLocally`, so
      replication writes them on owned entities only when spawning.
  - **`MovementServer`**:
    - runs inputs from the entity's owner only, each sequence number
      once and in order, at most 32 per entity per update;
    - sanitizes them: dt is clamped to (0, 0.1], the direction to unit
      length, and NaNs are zeroed. The client sanitizes the same way, so
      a speed hack replays identically on both sides and gains nothing;
    - after each batch, acknowledges the last sequence number it ran,
      with the resulting position, velocity and grounded state.
  - **Reconciliation**: on an acknowledgement newer than the last, the
    client drops the inputs it covers, resets to the server's state and
    replays the rest. The distance the prediction moved is the error.
    Errors over 1 cm count as corrections, for example when the server
    knocks the character back.

### 22.5 Sessions and discovery

Finding, hosting and joining games (`net/include/aether/net/session.h`).

- **The simulated network gains hosts**:
  - `LoopbackNetwork::OpenAt(ip, port)` binds sockets on simulated
    machines (10.0.0.x), so one process can stand in for a LAN.
  - `Address::Broadcast(port)` reaches every socket on that port except
    the sender's own.
  - UDP sockets set `SO_BROADCAST`.
- **`SessionInfo`**: name, map, mode, the game port, players and
  maximum, the build, whether a password is needed (the password itself
  is never advertised), and key/value properties. It has a compact
  binary encoding, and strings are cut to 255 bytes.
- **LAN discovery**:
  - `LanBeacon` listens on the discovery port (7778) and answers
    queries carrying our protocol id with the session's info. The
    query's nonce is echoed back so the browser can time it. A disabled
    beacon reads queries and ignores them.
  - `LanBrowser::Search` broadcasts a query. Answers to our own nonces
    become results, one per game address (the beacon's host plus
    `info.port`), with the ping and whether the build matches. Results
    are sorted by ping, expire after `expiry` (5 s), and can be
    refreshed every `auto_search` seconds.
- **Hosting and joining**:
  - `SessionClient::Join` connects, then asks to join over the reliable
    channel with its build, a player name and the password.
  - `SessionHost` checks, in order: the build, then the password, then
    room (`max_players`, or the host's peer limit). If a check fails,
    the peer is told why and dropped once the refusal is acknowledged
    (or after 1 s).
  - Peers that connect but never ask within 3 s are refused with
    NoRequest.
  - Accepted players get a unique name ("Ada", "Ada (2)") and the
    session's info, and the host keeps `info.players` current.
  - `Kick` refuses with Kicked.
  - Events: PlayerJoined, PlayerLeft and JoinRefused, each with a
    reason.
  - The client ends Joined, or Failed with one of: WrongPassword,
    BuildMismatch, Full (including the transport's own refusal),
    NoRequest, Kicked, ConnectFailed or Lost.
  - The password travels in the clear and is meant for the LAN; secure
    authentication comes with platform services.
- **Lobbies**: `LobbyService` is the interface games and the editor use:
  advertise or update a session, stop, search, results and update.
  `LanLobbyService` implements it with a beacon and a browser. Steam,
  EOS and console services implement the same interface later.

### 22.6 The editor

Networked Play-in-Editor and the network profiler (`editor/src/net/`).

- **`NetPlaySession`**:
  - Copies the edited world into a server world through the binary
    scene format. The edited world is never touched.
  - Starts N client worlds that connect over a `LoopbackNetwork`, in
    one process and deterministic for a seed.
  - Its latency, jitter, loss and duplication can change while playing.
  - **Modes**: a listen server, which is also a player (its pawn is
    moved directly on the server), or a dedicated server, which isn't.
  - **Options**:
    - **Replicate the scene**: every scene entity with a Transform gets
      a NetIdentity ("scene").
    - **Spawn players**: a predicted "player" pawn (Transform and
      NetMovement) for each connecting client, placed `spawn_spacing`
      apart and removed when it disconnects.
  - **Each frame**:
    - the network advances;
    - the server runs replication, RPCs and movement;
    - every client runs replication, RPCs, movement prediction and
      interpolation.
  - **Client spawns**: a newly spawned client entity is given copies of
    the server entity's other components (models, lights), as if the
    client had loaded the same level. Shipped games do this through
    `OnSpawn` instead.
  - **Input**: `MoveClient` (predicted) and `MoveListenPlayer`.
  - **Per-client views**: connected, RTT, loss, bytes each way, entity
    count and prediction corrections.
- **`NetProfile`** (kept by `ReplicationServer`): bytes per entity (with
  updates and spawns), per component and per field since `Reset`, plus
  snapshot totals and despawns.
  - **`BuildEntityRows`** lists entities by cost, each with its
    components by cost.
  - **`BuildFieldRows`** lists fields by cost over all entities.
  - Both give rates in bytes per second.
- **Panels**:
  - **`NetPlayPanel`**: the mode, the client count, scene and player
    options, the network sliders (live while playing), Play and Stop,
    and a table of the clients.
  - **`NetProfilerPanel`**: totals and rate, Reset, and "By entity"
    (tree) and "By field" (table) views.

### 22.7 PR breakdown

1. ✅ **Done.** The transport: sockets (UDP and the simulated network),
   connections with reliable, unreliable and sequenced channels, and
   hosts.
2. ✅ **Done.** Replication (§22.2):
   - the `Replicated` field flag and `NetIdentity`;
   - server-authoritative snapshots of reflected fields, with delta
     compression against acked baselines;
   - spawning and despawning on clients;
   - relevancy by distance, and per-connection priority within a
     bandwidth budget.
3. ✅ **Done.** RPCs (§22.3): `Server`, `Client` and `Multicast` function flags, called from
   C++, Blueprints and Luau, with ownership checks.
4. ✅ **Done.** Client-side prediction and reconciliation for character movement,
   and snapshot interpolation for everything else (§22.4).
5. ✅ **Done.** Sessions (§22.5): hosting, joining and LAN discovery (broadcast); a lobby
   abstraction for platform services later.
6. ✅ **Done.** The editor (§22.6):
   - Play in Editor with N clients and a listen or dedicated server;
   - simulated latency and loss;
   - a network profiler (bandwidth per entity and field).

## Phase 23: Profiling, debugging and developer tools

✅ **Done** on the engine and portable-editor side.

The concept is in [ROADMAP.md Phase 23](../ROADMAP.md). The core pieces
(console variables, the console, the profiler's zones and counters,
debug drawing, crash capture) live in the engine, so every module can
use them. Their panels are in the portable editor library and work as
in-game overlays too.

### 23.1 Console variables and the console

- **Logger** (`aether/core/log.h`):
  - sinks: listeners for every line that passes the level, called
    outside the logger's lock;
  - the 512 most recent lines;
  - stdout can be switched off.
- **CVars** (`aether/core/cvar.h`): named, typed settings declared where
  they're used.
  - Declared like
    `static AutoCVar<int> cascades("r.shadows.cascades", 4, "Shadow cascades", CVar_Archive, 1, 4);`.
  - Types: bool, int, float and string.
  - Names are case-insensitive and dotted by system.
  - Reads are atomic, so any thread can read while the console writes.
  - **Parsing**:
    - bools take 1/0, true/false, on/off and yes/no;
    - ints take decimal, hex, octal, and whole-number floats such as
      "3.0";
    - floats reject NaN and infinity;
    - values outside the range are clamped.
  - **Floats print** in their shortest round-trip form ("0.1").
  - **Defaults**: `IsDefault` and `Reset`.
  - **Flags**:
    - ReadOnly;
    - Cheat (changed only while cheats are allowed);
    - Archive (saved by `writeconfig` when not at its default);
    - RequiresRestart (the console says so).
  - **Changes**: `OnChange` callbacks, plus a generation counter for
    cheap polling.
  - **`CVarRegistry`** also holds console commands (`AutoConsoleCommand`).
    - Registering a name again returns the existing variable when the
      type matches, and fails on a type clash.
    - A name can't be both a variable and a command.
- **`Console`** (`aether/core/console.h`):
  - **Syntax**:
    - `name` describes a variable (value, type, range, default, flags,
      help);
    - `name value` sets it (strings take the rest of the line);
    - `command args` runs a command;
    - statements are separated by `;`;
    - `//` and `#` start comments;
    - quotes, with `\"`, group arguments.
  - **Built-in commands**: help [prefix], find text (names and help),
    cvarlist [prefix], set, reset, toggle, echo, exec file (nested up
    to 8 deep), writeconfig file, history, clear.
  - **Errors**: unknown names suggest close matches; Execute returns
    false if any statement failed.
  - **History**: newest last, no repeats, bounded at 64.
  - **Completion**: `Complete(prefix)` lists sorted variables, commands
    and built-ins. `CompleteLine` extends the word being typed to the
    candidates' common prefix, adding a space when only one remains.
  - **The command line**: `+name value +command args`.
  - **Log capture**: log lines from any thread wait in a queue until
    `Pump`.
- **`ConsolePanel`** (editor):
  - output coloured by level, with a filter;
  - Clear, "Show log" and "Cheats" toggles;
  - the input line: Up/Down walk the history; Tab completes, and a
    second Tab lists the candidates.
  - `Overlay(toggle, width)` draws the same panel as a drop-down window
    for game builds, toggled with the tilde key.

### 23.2 The profiler

- **Zones** (`aether/core/profiler.h`): `AETHER_PROFILE_ZONE("Physics")`
  or `AETHER_PROFILE_FUNCTION()`.
  - Each zone is recorded on its own thread without a lock: two clock
    reads, and a push when it ends.
  - It carries its nesting depth and thread.
  - `Intern` gives stable names for zones named at runtime.
  - `SetThreadName` names a thread; job workers name themselves
    "Worker N".
- **Frames**: `BeginFrame`/`EndFrame` on the main thread. `EndFrame`
  gathers the following into a `ProfileFrame`, and the last `history`
  (300) frames are kept:
  - every thread's zones since the last frame;
  - the per-frame counts (`Count`: summed over the frame, then reset);
  - the gauges (`Gauge`: they keep their last value);
  - the GPU pass timings the render backend submitted.
  - **Enabled**: off records nothing.
  - **Paused**: frames still run but aren't kept.
- **Statistics**: `Aggregate(frames)` gives per zone: calls, total, self
  (total minus direct children on the same thread), max, and per-frame
  average, sorted by total.
- **Export**: `WriteChromeTrace` writes Chrome trace JSON for
  chrome://tracing and Perfetto, containing:
  - a "Frames" track;
  - zones as complete events per thread, with thread names;
  - counters as counter events.
- **Tracy**: with `-DAETHER_TRACY=ON`, Tracy's client is fetched and the
  same macros also emit Tracy zones and frame marks.
- **Instrumented already**:
  - every scheduler phase ("Update", "FixedUpdate", ...);
  - every system, on whichever thread runs it.
- **Memory by category**: `MemoryTracker` keeps bytes in use, the peak,
  and allocation and free counts per category.
  - `LinearAllocator` and `PoolAllocator` report under the category set
    with `SetMemoryCategory`.
  - `FrameAllocator` reports its reserved buffers.
- **Console**:
  - CVars `profiler.enabled` and `profiler.history`;
  - commands `profiler.pause`, `profiler.clear` and
    `profiler.dump [file]` (a Chrome trace).
- **`ProfilerPanel`** (editor): Record, Pause and Clear toggles, trace
  export, and the average, fps and worst frame time.
  - **Frame graph**: green, yellow or red against 60 and 30 fps, with
    budget lines. Click a frame to inspect it; right-click to follow the
    newest again.
  - **Zones**: a table of this frame or all kept frames.
  - **Timeline**: per-thread lanes, zones nested by depth, with hover
    times.
  - **Counters**: the frame's counters and GPU passes.
  - **Memory**: memory by category.

### 23.3 Debug drawing and stat overlays

- **`DebugDrawList`** (`aether/debug/debug_draw.h`): thread-safe drawing
  requests.
  - Shapes: `Line` (with or without depth testing), `Arrow`, `Box`
    (oriented, 12 lines), `Sphere` (three great circles), `Circle`,
    `Point` and `Axes` (x red, y green, z blue). All shapes are stored
    as lines, so a consumer only draws lines and text.
  - Text: `Text` in the world and `ScreenText` stacked top-left.
  - Colors are 0xAABBGGRR (`DebugColor(r, g, b, a)`).
  - **Duration**: 0 means this frame only; otherwise the item stays
    for that many seconds. `Tick(dt)` once per frame ages items and drops
    the expired ones. Screen text closes up the gaps.
  - The `debug.draw` CVar switches it off. `max_lines` (200,000) caps
    it, and dropped lines are counted.
- **Every language**:
  - **C++**: `DebugDrawList::Get()` or `DebugDraw::*`.
  - **Blueprints**: `DebugDraw` is a reflected struct of static
    BlueprintCallable functions (Line, Arrow, Box, Sphere, Point, Text,
    ScreenText; 0..1 RGB colors and a duration), so they appear as
    Call.Native nodes.
  - **Luau**: the read-only global `Draw` table: `line`, `arrow`, `box`,
    `sphere`, `point`, `text` and `screen`. Arguments are vectors, with
    an optional color (default white) and duration (default 0).
- **Stat overlays** (`aether/debug/stats.h`): `StatGroups` are named
  providers of coloured text lines, shown in the order they were turned
  on.
  - **Built in**:
    - `fps`: fps, average, min and max over 60 frames;
    - `zones`: the top 10 zones per frame, with self time;
    - `counters`: the last frame's counters and gauges;
    - `gpu`: pass times;
    - `memory`: by category.
  - **`net::NetStatGroup(host)`** adds `net` (peers, RTT, loss and
    traffic) while it exists.
  - **The `stat` console command**: `stat fps memory` toggles groups,
    `stat none` hides them all, and `stat` lists them.
- **`DebugOverlay`** (editor; also usable in game builds): draws over a
  viewport with ImGui, given a view-projection matrix:
  - lines, clipped at the near plane;
  - world text centred on its projected point;
  - screen text;
  - the shown stat groups top-right on a dark backing.

  `ProjectToScreen` is the projection it uses. The renderer's debug pass
  can draw the same lines with depth testing.

### 23.4 Crash handling

- **`CrashHandler::Install(config)`** (`aether/debug/crash.h`) handles:
  - fatal signals on POSIX (SIGSEGV, SIGABRT, SIGFPE, SIGILL and
    SIGBUS), on an alternate stack so a stack overflow can still be
    reported;
  - unhandled SEH exceptions on Windows;
  - uncaught C++ exceptions everywhere, through the terminate handler.
- **The report** is `crash-<unix time>-<pid>.txt` in the configured
  directory. It holds:
  - the app, build, reason (a signal name, an exception code, or the
    uncaught exception's `what()`), fault address, time, process and
    thread;
  - the game's context;
  - a backtrace (`backtrace_symbols_fd` on glibc, raw frames
    elsewhere);
  - the last 200 log lines.

  On Windows, `MiniDumpWriteDump` also writes `crash-....dmp` next to it.
- **Signal safety**: the handler does only async-signal-safe work.
  - Log lines are copied into a fixed ring as they're logged (through a
    log sink).
  - Up to 16 context pairs (`SetContext("level", "Docks")`) live in
    fixed buffers.
  - The report is written with plain `write()` calls and hand-rolled
    number formatting.
  - Then the default action is restored and the signal raised again,
    so the process still dies as it would have (a core dump, an exit
    status).
  - A second fault while reporting isn't reported again.
- **Reading reports**:
  - `ParseCrashReport` returns the fields (`context.*` included), the
    backtrace and the log.
  - `ListCrashReports(dir)` lists the unseen ones, newest first.
  - `MarkCrashReportSeen` renames a report to `.seen.txt`.
  - `DeleteCrashReport` removes it and its minidump.
  - `CrashHandler::WriteReport(reason)` writes one on purpose (for "report
    a problem" and tests).
- **`CrashReporterDialog`** (editor): `Refresh` at startup opens the
  dialog when unseen reports exist.
  - It lists the reports by time and reason.
  - The selected report shows its reason, build and process, its
    context, backtrace and log.
  - Buttons: Copy report, Dismiss, Delete and Dismiss all.

### 23.5 Functional tests

Whole-game scenarios, run headless (`aether/testing/functional_test.h`).

- **`FunctionalTest(name)`** is built from steps and runs them in order
  on a fresh World, SystemScheduler and FrameLoop with a fixed step
  (1/60 s by default; `FixedStep` changes it). The steps:
  - `Scene(path)`: loads a binary or JSON scene;
  - `Setup` and `Do`: arbitrary code that builds the world and adds
    systems;
  - `Simulate(seconds, each_frame)`;
  - `SimulateUntil(description, done, timeout)`;
  - `Check(description, predicate)`;
  - `ExpectReaches(tag, center, half_extents, timeout)`: "entity X
    reached trigger Y". It checks the first entity with the tag against
    the box.
  - Options: `Tag`, and `Timeout` (simulated seconds across the whole
    test, 600 by default).
- **`FunctionalContext`**: `world`, `scheduler`, the simulated `time` and
  `frame`, `FindTagged` and `AllTagged`, `Position`, `Inside`, `Log`,
  `Fail` and `Expect`.
- **Failures**: the first failing step ends the test. Its result names
  the step and why it failed:
  - a check that didn't hold;
  - not done after N s;
  - where the entity got to;
  - no entity with the tag;
  - a scene that couldn't load;
  - an exception;
  - the overall time budget running out.

  Log lines written during the test, from any thread, are kept in the
  result.
- **Registry and reports**:
  - `AETHER_FUNCTIONAL_TEST(Name) { return FunctionalTest(...)...; }`
    registers a test.
  - `FunctionalTestRegistry::Run(options)` runs them sorted by name,
    filtered by name substrings and tags, optionally stopping at the
    first failure.
  - Reports: `WriteJUnitXml` (for CI), `WriteJsonReport` and
    `FormatFunctionalSummary`.
- **`aether_functional`** (`tools/functional`): a headless runner.
  - Options: `--list`, `--filter`, `--tag`, `--stop-on-failure`,
    `--junit FILE`, `--json FILE` and `--quiet`.
  - It exits 0 when everything passes, 1 on a failure and 2 on bad
    arguments.
  - It's registered with CTest, and runs two sample scenarios: a seeker
    reaching its goal, and a spawner.
  - Games link their own scenarios into a runner like it.
- **Console**: `functional.run [filter ...]` and `functional.list` run and
  list the tests in game builds and the editor.

### 23.6 PR breakdown

1. ✅ **Done.** Console variables and the console (§23.1).
2. ✅ **Done.** The profiler (§23.2): CPU zones per thread and frame, counters, memory by
   category, with Tracy forwarding as an option (`AETHER_TRACY`), and
   the editor's profiler panel (frame graph, per-zone times, counters).
3. ✅ **Done.** The debug draw API (§23.3) (lines, boxes, spheres, arrows and world text,
   with a duration) from C++, Luau and Blueprints, and stat overlays
   (`stat fps`, `stat memory`, `stat net`).
4. ✅ **Done.** Crash handling (§23.4): signal and exception handlers, captured log lines,
   and a crash report file (a minidump on Windows); the editor's crash
   reporter dialog.
5. ✅ **Done.** Functional tests (§23.5): scenarios ("load the scene, simulate 5 s, assert
   that entity X reached trigger Y") run headless, with a CLI runner
   and reports for CI.

## Phase 24: Cross-platform

The concept is in [ROADMAP.md Phase 24](../ROADMAP.md). Until now the
engine was tested on Linux in this repository's sessions, while the
Windows-only parts (the D3D12 renderer and editor, Win32 windows and
input) were written for Windows without a Windows build to check them.
Phase 24 starts by making every platform build on every change.

### 24.1 Continuous integration

`.github/workflows/ci.yml` runs on pushes to master, on pull requests,
on version tags, and by hand.

- **Linux** (Ubuntu 24.04, Ninja), four jobs: GCC, Clang, ASan+UBSan
  (Debug), and with physics (Jolt).
  - Each builds the portable engine, editor UI, scripting and tests.
  - Each runs `aether_tests`, then `aether_functional`, whose JUnit
    report is kept as an artifact.
  - Fetched dependencies are cached.
- **Windows** (windows-2022, MSVC, Visual Studio generator):
  - **Build**: everything, the D3D12 editor included. Vulkan is off,
    because hosted runners have no Vulkan loader.
  - **Tests**: the unit tests run but don't fail the job yet, because
    hosted runners have no GPU for the D3D12 device tests. The
    functional tests do fail the job.
  - **Package**: `AetherEditor-windows-x64.zip`, containing
    `aether_editor.exe`, its DLLs (dxcompiler), the assets and the
    README, uploaded as an artifact.
- **Release**: a `v*` tag attaches that zip to a GitHub release, with
  generated notes, once both platforms pass.
- **Assets**: the editor now looks for its assets next to the
  executable first, then in the working directory, then in the source
  tree, so the packaged editor runs from wherever it's unzipped.

### 24.2 A portable window and input layer

`platform::Window` is now one interface over two backends: Win32 on
Windows and GLFW 3.4 (fetched by CMake) on Linux and macOS, with X11,
optionally Wayland (`AETHER_GLFW_WAYLAND`), and Cocoa.
`AETHER_BUILD_WINDOWING` turns the GLFW backend off for builds with no
windowing (`AETHER_HAS_WINDOW` tells code which it has).

- **Events**: every backend reports the same `WindowEvent`s, in
  `input::Key` terms: keys and mouse buttons (left and right modifiers
  told apart), mouse movement and position, the wheel, typed
  characters, gamepad axes and buttons (GLFW's gamepad mappings, Y axes
  pointing up, triggers 0..1), focus, resizes and the close request.
  `TakeEvents()` drains them each frame.
- **Input**: `ApplyWindowEvents` feeds a frame's events into
  `input::InputState`; losing focus releases every key and axis, so
  nothing sticks down after Alt-Tab.
- **Key maps**: `KeyFromVirtualKey` (Win32), `KeyFromGlfwKey`,
  `KeyFromGlfwMouseButton`, `KeyFromGlfwGamepadButton`,
  `KeyFromGlfwGamepadAxis`; keys with no engine name map to `Key::None`.
- **Native handles**: `NativeHandle()` (HWND, X11 Window, NSWindow),
  `NativeDisplay()` (the X11 or Wayland display) and, on GLFW,
  `PlatformWindow()` for `glfwCreateWindowSurface`. Also `Backend()`,
  `SetTitle`, `RequestClose`, and `WindowDesc::visible` for hidden
  windows.
- **Tests**: the key maps and event application everywhere; a real GLFW
  window (created hidden, pumped, a second window alongside, closed)
  wherever there is a display - Linux CI runs the tests under Xvfb.
- **Windows build fix**: MSVC builds with
  `_ENABLE_EXTENDED_ALIGNED_STORAGE`, needed for sorting 16-byte-aligned
  types.

### 24.3 The Vulkan backend off Windows

The RHI's Vulkan backend (`gfx/rhi/vulkan`) used to build only on
Windows. It now builds on Linux (and is ready for macOS) and renders
headless, which is what lets CI test rendering with no GPU.

- **Build**: on Linux and macOS, CMake uses the system Vulkan loader and
  headers (`libvulkan-dev`) and glslang (`glslang-dev`). If either is
  missing, the backend is left out and nothing else changes.
  `rhi::CreateDevice(Backend::Vulkan)` works off Windows;
  `Backend::D3D12` reports itself unavailable there.
- **Shaders**: `CompileHLSLToSPIRV` uses glslang's HLSL front end off
  Windows, in place of DXC. It compiles the same HLSL with DXC's mapping:
  `register(tN, spaceM)` becomes set M, binding N; `[[vk::push_constant]]`
  becomes push constants; entry points keep their names.
- **Instance**: on Windows the Win32 surface extension. Elsewhere,
  whichever window-system surface extensions the loader offers (xcb,
  Xlib, Wayland, Metal), or none on a machine with no window system.
- **Window swap chains**: `CreateSwapChain` takes an HWND on Windows and
  a `GLFWwindow*` (`Window::PlatformWindow()`) elsewhere. The surface
  comes from `glfwCreateWindowSurface`, and present support is checked
  against it.
- **Offscreen swap chains**: `CreateSwapChain(nullptr, w, h, n)` creates
  n device-local images behind the same render pass, framebuffers and
  depth buffer.
  - It has no surface and no acquire or present semaphores, and
    `Present` does nothing.
  - Pipelines, bindless textures, push constants and draws all work as
    they do with a window.
  - Texture handles stay stable across `Resize`.
- **Read back**: `ISwapChain::ReadBack(rgba8)` copies the current back
  buffer to memory (RGBA, rows top to bottom). Call it between Submit
  and Present.
  - Offscreen swap chains always support it, and window swap chains do
    where the surface allows `TRANSFER_SRC`.
  - Other backends return false, for now.
- **Tests**, on any Vulkan driver (lavapipe in CI):
  - glslang output (SPIR-V magic number, entry-point names, errors
    reported).
  - An offscreen frame: a push-constant-coloured triangle that checks
    +Y is up, image rotation, and resizing.
  - A textured quad through the bindless table.
  - A window swap chain under Xvfb.
- **CI**: the Linux jobs install the Vulkan loader, glslang and Mesa's
  lavapipe, and build with Vulkan on.
  - The ASan+UBSan job hides the driver, because LeakSanitizer reports
    lavapipe's own thread allocations after unload. Its device tests
    skip; the other jobs run them.
- **MSVC**: the missing standard includes it reported (`<array>`,
  `<numeric>` in terrain) are added, along with the others a scan found.

### 24.4 The editor on Vulkan

The editor's panels now run wherever Vulkan does. The D3D12 editor
(`editor/main.cpp`, with its 3D viewport) stays the Windows editor.

- **ImGui on the RHI** (`editor/src/host/imgui_vulkan_host.h`,
  `Aether::EditorVulkan`): `ImGuiVulkanHost` runs `imgui_impl_vulkan` on
  the engine's own `VkDevice` and queue.
  - Its pipeline is built for the swap chain's default render pass.
  - Each frame's draw data is recorded into an RHI command list between
    `BeginRenderPass` and `EndRenderPass`.
  - Window and offscreen swap chains work alike.
- **Input from the engine's windows** (`host/imgui_window_input.h`):
  `FeedImGuiEvents` turns the platform-neutral `WindowEvent`s into ImGui
  input: keys with modifiers, mouse position, buttons and wheel,
  characters, focus and size. `ToImGuiKey` maps `input::Key`. This
  replaces a per-platform ImGui backend, so Win32 and GLFW windows drive
  the editor the same way.
- **The editor shell** (`editor/shell`, `aether_editor_shell`), built
  wherever the Vulkan backend is:
  - **Panels**: the Blueprint, Material, Animation (graph, blend space,
    clip viewer) and Behavior Tree editors in tabs; the terrain tools;
    the console and profiler. Each opens on a sample document.
  - **Window**: GLFW (Win32 on Windows), resizable, with VSync.
  - **Offscreen**: set `AETHER_EDITOR_HEADLESS=1`. It also renders
    offscreen when there's no display.
  - **Automation**: `AETHER_EDITOR_MAX_FRAMES` and
    `AETHER_EDITOR_SCREENSHOT` (a PNG of the last frame, through
    `ISwapChain::ReadBack`) work as in the Windows editor;
    `AETHER_EDITOR_SIZE=WxH` sets the size.
    `AETHER_EDITOR_TAB` (`blueprint`, `material`, `animation`,
    `behavior`; any prefix) picks the editor tab it opens on.
- **Tests**:
  - Key mapping.
  - Window events driving ImGui: mouse, buttons, Ctrl+S chords,
    left/right modifiers, wheel, characters, resize.
  - ImGui drawn through Vulkan offscreen and read back: origin at the top
    left, solid shapes, and a window.
- **CI**: the Linux GCC job runs the shell in an Xvfb window on lavapipe
  and uploads the last frame as `editor-screenshot-linux`.
- **Fixes**:
  - Jolt's own Vulkan compute (`JPH_USE_VK`) is off, because with the
    loader present but no shader compiler it tried to build its shaders.
  - Linux CI installs `glslang-tools`, which Ubuntu's glslang CMake
    package references.
  - A raw string in a macro argument that MSVC rejects
    (`test_console.cpp`) is now an ordinary string.

### 24.5 macOS, ARM and platform plugins

- **ARM**: the SIMD math (`math/vec.h`, `mat4.h`, `quaternion.h`) uses
  basic SSE intrinsics. On ARM64 (Apple Silicon, Android) they compile
  through sse2neon, fetched by CMake, which maps them to NEON. x86 builds
  keep `-mavx2 -mfma`; ARM builds don't get them.
- **Apple's libc++** has no floating-point `std::from_chars`. Where it's
  missing, the reflection serializer reads its shortest round-trip floats
  back through a classic-locale stream, which is just as
  locale-independent.
- **MoltenVK**: the Vulkan backend works on macOS through MoltenVK,
  Vulkan on Metal.
  - When the loader offers `VK_KHR_portability_enumeration`, the instance
    enables it and sets the enumerate-portability flag.
  - On devices that have `VK_KHR_portability_subset`, the device enables
    it.
  - Surfaces come from GLFW through `VK_EXT_metal_surface`.
- **Platform plugins** (`aether/platform/platform_plugin.h`,
  `cmake/AetherPlatform.cmake`, `platforms/README.md`): a platform the
  engine doesn't build in (Android, a console) plugs in from its own
  directory.
  - **Build**: `aether_platform_plugin(NAME ... SOURCES ...)` builds the
    plugin as an OBJECT library, so its self-registration is never dropped
    by the linker. `AETHER_PLATFORM_PLUGINS` lists the plugin directories.
    `aether_link_platform_plugins(target)` links them into the tests, the
    functional runner and the editor shell.
  - **Code**: `AETHER_PLATFORM_PLUGIN(id)` registers a `PlatformPlugin`
    with optional hooks: `startup` and `shutdown` (shutdown runs in
    reverse order, and a plugin registered after startup starts at once),
    `user_data_dir`, and `create_device`, an RHI device for a graphics API
    the engine doesn't build in.
  - **Example**: `platforms/example` is a working template.
- **Tests**:
  - Registry: duplicates refused, start/stop order, a late registration,
    user-data fallthrough, device factories.
  - The example plugin registering itself from its own library.
- **CI**:
  - A macOS job (Apple Silicon, `macos-15`, Homebrew MoltenVK, the Vulkan
    loader and glslang) builds the engine, physics, scripting, the Vulkan
    backend and the editor shell, and runs the unit and functional tests.
    It also tries an offscreen editor screenshot; that step doesn't fail
    the job, because hosted runners may have no Metal device.
  - The Linux jobs build `platforms/example` into the tests.
  - The release job waits for macOS too.

### 24.6 PR breakdown

1. ✅ **Done.** Continuous integration (§24.1), and the packaged Windows
   editor.
2. ✅ **Done.** Fix what the first Windows builds report, until the
   Windows job is green and the editor package launches: MSVC's extended
   aligned storage, missing standard includes, and a raw string in a
   macro. The packaged editor now builds, passes its tests, and starts and
   screenshots itself on WARP in CI.
3. ✅ **Done.** A portable window and input layer (§24.2): GLFW on
   Linux and macOS behind `platform::Window`, alongside Win32.
4. ✅ **Done.** The Vulkan backend off Windows (§24.3): glslang shaders,
   GLFW surfaces, offscreen swap chains with read-back, and rendering
   tests on lavapipe in Linux CI.
5. ✅ **Done.** The editor on Vulkan (§24.4): RHI-hosted ImGui, window
   events as ImGui input, and the portable editor shell, screenshotted
   on lavapipe in Linux CI.
6. ✅ **Done.** macOS through MoltenVK, ARM through sse2neon, and the
   platform plugin structure for Android and consoles (§24.5). Phase 24
   is complete.

## Phase 25: Build, cook and package

The concept is in [ROADMAP.md Phase 25](../ROADMAP.md). A shipped game
reads its content from a few archives through a virtual file system,
cooked from the project by a command-line cooker and run by a player
executable with no editor code. Step 1 is the archive format that
everything else writes and reads.

### 25.1 .apak archives and the virtual file system

- **Format** (`aether/pak/pak.h`):
  - **Header**: magic, version, entry count, and the index's offset,
    size and CRC-32.
  - **Data**: each entry's stored bytes, back to back.
  - **Index**: at the end; per entry, its path, compression, offset,
    stored and original sizes, and the CRC-32 of its original bytes.
  - **Paths** are normalized to `/`-separated with no leading slash;
    `..` is rejected.
- **Compression**: LZ4 1.10 and zstd 1.5.7, both fetched by CMake and
  BSD-licensed.
  - Policies: `None`, `LZ4`, `Zstd`, or `Auto`, which uses zstd for
    entries of 16 KiB and up and LZ4 below that.
  - An entry that doesn't shrink by at least 1/32 is stored raw.
- **Writing** (`PakWriter`):
  - `Add` takes bytes or text, `AddFile` a file, `AddDirectory` a whole
    tree, sorted so the archives are deterministic.
  - `Write` writes a temporary file and renames it, so a crash never
    leaves half an archive.
  - `Build` returns the archive in memory.
- **Reading** (`PakReader`):
  - `Open` (a file) or `OpenMemory` checks the magic, the version, the
    index bounds and the index CRC, and the bounds of every entry. Entry
    data stays on disk until read.
  - `Read` decompresses an entry and checks its CRC.
  - `Verify` reads everything and lists the damaged entries.
- **Virtual file system** (`aether/pak/vfs.h`):
  - Directories and archives mount at mount points, each with a
    priority. The highest priority wins; ties go to the later mount.
  - So patch and DLC paks override the base pak, and a loose directory
    on top overrides everything, for iterating on content.
  - `Read`, `Exists`, `Resolve` (which mount a path comes from), and
    `List` (every visible path, each once).
- **`aether_pak`** (`tools/pak`): `create` (from a directory, with a
  prefix and a compression policy), `list`, `extract` and `verify`.
- **Tests**:
  - Path normalization, and the CRC-32 check value.
  - Round trips under every policy, including raw fallback for
    incompressible data and empty entries.
  - Damage: bad magic, truncation, a damaged index, a flipped data byte
    (CRC), damaged zstd data.
  - Files on disk, and deterministic order.
  - The VFS's overrides, unmounting, mount points and in-memory
    archives.
- **CI**: the GCC job packs `assets/` with `aether_pak`, verifies the
  archive, extracts it and compares the result with the original.

### 25.2 The cooker

- **`aether::cook::Cook`** (`aether/cook/cooker.h`) turns a project into
  one `.apak` archive (`<out>/<pak_name>.apak`), plus
  `CookManifest.json` beside it.
- **What gets cooked** (`CollectCookSet`):
  - **Roots**: the startup scene, the project's new `always_cook` list
    (asset paths, or folders ending in `/`), and any extra roots passed on
    the command line.
  - **Walk**: from the roots it follows the asset database's
    dependencies transitively. A sub-asset brings its source file.
  - **Reasons**: each asset records why it was cooked ("startup scene",
    "always cook", "used by `<path>`").
  - **Skipped**: unreachable assets are left out and counted. Unknown
    roots and missing dependencies become warnings.
- **Editor-only data**:
  - A new reflection flag, `Field_EditorOnly`, marks fields the editor
    saves but the game never reads.
  - `StripEditorOnly` removes them from scene and prefab components,
    which it looks up by their reflected names, including inside nested
    structs and arrays.
  - Scenes and prefabs are then written minified.
- **The archive**:
  - `Content/<path>`: each cooked asset, plus the helper files `.gltf`
    models refer to (buffers, images that aren't assets themselves).
  - `Imported/<guid>.bin`: each asset's and sub-asset's importer output,
    served from the derived data cache when it's current.
  - `Manifest.json`: the configuration, the project's runtime settings
    (startup scene, timestep, gravity, layers, collision matrix), and
    every cooked asset with its GUID, path, importer, dependencies,
    imported flag and sub-assets.
  - No `.ameta` files ship.
- **Configurations**: `BuildConfiguration` is Debug, Development or
  Shipping. Shipping writes its manifest minified.
- **`aether_cook`** (`tools/cook`): `--out`, `--config`, `--always`,
  `--compression`, `--no-imported`, `--pak-name`, and `--verbose`, which
  lists every asset with its reason.
- **Tests**:
  - Configuration names.
  - Stripping, top level and nested, and leaving unreflected components
    alone.
  - A full cook of a test project: startup scene → prefab → texture, a
    model kept by `always_cook` with its `.bin`, and an unused texture.
    It checks the cook set, the reasons, the skip count, the stripped
    field count, the archive's minified and stripped scene, the manifest
    and the imported data.
  - The refusals.
- **The test runner**: output is now line-buffered, and a crash prints
  the name of the test that was running, so a CI crash points somewhere.

### 25.3 Cooked textures

Packaged games load textures in the form GPUs sample directly:
block-compressed, with a full mip chain. They stay compressed in GPU
memory too: BC1 at 8:1, BC3, BC5 and BC7 at 4:1, and BC4 at 2:1 against
RGBA8.

- **Encoders**: `bc7enc_rdo` by Rich Geldreich (MIT), fetched by CMake.
  `rgbcx` encodes BC1–BC5 and `bc7enc` encodes BC7, each with a decoder.
  Only the library sources are built, as `aether_bcenc`.
- **`aether/cook/texture_cook.h`**:
  - **Formats**: `TextureFormat` is RGBA8, BC1, BC3, BC4, BC5 or BC7.
    `BlockBytes` and `MipBytes` give sizes.
  - **Mips**: `GenerateMips` builds the chain down to 1×1, for any size,
    not only powers of two.
    - It uses a 2×2 box filter in linear light for sRGB colour, so a
      black/white checker averages to sRGB 188, not 128.
    - For normal maps it averages the normals and renormalizes them.
  - **Auto format**: `ChooseTextureFormat` picks BC5 for normal maps,
    BC4 for grey images without alpha, and BC7 otherwise.
  - **`CookTexture`**: takes RGBA8 of any size. Edge blocks repeat the
    last row and column. Quality runs from 0 to 4 and controls BC7's uber
    level and partitions, and rgbcx's level. BC4 and BC5 hold data, not
    colour, so they're never sRGB.
  - **`DecodeMip`**: decodes a mip back to RGBA8. For BC5 normal maps it
    rebuilds z from x and y.
  - **The `.atex` container**: format, sRGB and normal-map flags, size,
    and the mip chain. `LoadAtex` checks every mip's size against its
    format.
- **The cooker**:
  - Each texture asset is decoded and cooked according to its `.ameta`
    settings: `cook_format` (auto or a format name), `normal_map`, `srgb`
    and `mips`.
  - The result is stored as `Cooked/<guid>.atex`, and the manifest
    records `cooked` and `cooked_format` for it.
  - `CookOptions` gets `cook_textures` and `texture_quality`.
- **Tests**:
  - Formats and sizes.
  - Mips: sRGB versus linear averaging, odd chains, and normal
    renormalization.
  - Quality, against PSNR bounds on a gradient:
    - BC1 and BC3: 32 dB.
    - BC4 and BC5: 38 dB.
    - BC7: 40 dB, and 38 dB at an odd size, which checks the partial edge
      blocks.
  - The automatic format choice, the normal map's rebuilt z, and the
    `.atex` round trip and damage checks.
  - The full cook test now checks both of its textures' cooked forms.

### 25.4 The player

The game without the editor: `player/` builds `aether_game`, the runtime
library, and `aether_player`, the executable.

- **`GameManifest`** (`aether/player/game.h`): `ParseGameManifest` reads
  the cooker's `Manifest.json`:
  - the project name and configuration;
  - the startup scene, the fixed timestep, gravity, the layers and the
    collision matrix;
  - every cooked asset's GUID, path, importer and cooked form.

  It refuses text that isn't a cook manifest, an unknown configuration, a
  timestep of 0 Hz or less, and an asset with no GUID or path.
- **`GamePackage`**:
  - Mounts `.apak` archives, or folders for a loose cook, into a virtual
    file system. A higher priority wins, so a patch pak replaces files.
  - `FindPaks` lists a folder's archives sorted by name, so later names
    (patches) mount on top.
  - Reads the manifest, finds assets by path or GUID, and reads
    `Content/<path>`.
- **`Game`**:
  - **Loading**: the startup scene, or any cooked scene, JSON or binary,
    from the package. Its prefab instances are resolved from the
    package's prefabs, read once and cached.
  - **Play**: `BeginPlay` and `EndPlay` drive the scene's lifecycle
    callbacks. `Tick(dt)` runs the fixed-timestep frame loop at the
    manifest's rate: physics, then the lifecycle's FixedUpdate in each
    fixed step, then Update and LateUpdate.
  - **Physics**: when the engine has its physics module, gravity and the
    collision matrix come from the manifest, and a `PhysicsScene` makes
    bodies for the scene's colliders.
  - **Game systems**: added through `Systems()`.
  - **Stats**: frames, fixed steps, game time, entities, prefab instances
    and physics bodies.
- **`aether_player`**:
  - **Mounting**: every `--pak` given (a folder means all its archives),
    or else `Paks/` beside the executable, then in the working directory.
  - **Running**: loads the startup scene (or `--scene`) and runs it in a
    window, through the RHI (D3D12 on Windows, Vulkan elsewhere), or with
    `--headless`. Headless runs step at the fixed rate, so a run is
    reproducible. `--frames` stops after that many frames.
  - **Configurations**: Debug logs everything and turns on the graphics
    debug layer. Development logs info plus a stats line every second, and
    puts the configuration in the window title. Shipping logs only
    warnings and errors.
  - **Drawing**: the window shows the clear colour for now. Drawing the
    scene comes with the renderer's RHI path.
- **Packaging**: the Windows zip now carries `aether_cook.exe`,
  `aether_pak.exe` and `aether_player.exe` next to the editor.
- **Tests**:
  - The manifest, and its refusals.
  - A pak with a startup scene and a prefab: the prefab's entity comes
    from the pak, the lifecycle runs, and 1 s at 60 Hz is 60 fixed steps
    and 60 updates.
  - A 30 Hz base pak, and a patch pak that replaces its scene.
  - The refusals: nothing mounted, no manifest, a missing pak, a missing
    scene.
  - A project cooked by the cooker and then played: its 50 Hz timestep
    and the entity's position come through.
  - With physics: a ball dropped onto a floor falls to about 8.8 m after
    0.5 s and comes to rest on the floor.

### 25.5 Project settings, packaging and the Build and Package window

- **Project settings** (`ProjectSettings`):
  - **Window**: `window_title` (empty means the project name),
    `window_width`, `window_height` and `vsync`.
  - **Quality presets**: `QualityPreset` has a name, a resolution scale,
    the shadow resolution and cascades, the view distance, the MSAA
    samples and bloom.
  - `DefaultQualityPresets()` gives Low, Medium, High and Epic.
    `default_quality` is High, and `FindQualityPreset` looks one up by
    name.
  - Projects saved before these settings existed load with the defaults.
- **The cooker**:
  - The manifest now carries `window`, `quality_presets` and
    `default_quality`. It warns when the default isn't one of the presets.
  - `CookOptions::progress` reports the fraction done and the current
    stage ("Cooking `<path>`", then "Writing", then "Done").
  - `CookOptions::cancel` stops the cook between assets. It then fails
    with "Cancelled" and writes nothing.
- **The player**:
  - `GameManifest` reads the window and quality settings.
  - `ChooseQuality` picks the requested preset, else the default, else
    the first.
  - `aether_player` opens its window with the project's title, size and
    vsync. `--size` overrides the size, and `--quality <preset>` picks a
    preset.
- **Packaging** (`aether/cook/package.h`):
  - **`Package`** cooks into `<output>/Paks/` and copies the player in
    beside it, named for the project (`GameExecutableName`). On Windows it
    copies the player's DLLs too.
  - Old paks are removed first, so a stale one can't mount over the new
    one. The result runs on its own: the player finds `Paks/` beside
    itself.
  - **`FindPlayerExecutable`** looks for `aether_player` beside the
    running executable (a packaged editor), else uses the one the engine
    was built with.
- **Processes** (`aether/platform/process.h`):
  - `ExecutablePath()` gives the running executable's path.
  - `ChildProcess` starts a program with arguments in a working folder,
    reports whether it's running, waits for its exit code, and kills it.
    It uses `CreateProcessW` on Windows and `posix_spawn` elsewhere.
- **The editor**:
  - **Build and Package window** (`editor/src/packaging`):
    - **Settings**: the configuration, the compression, the texture
      quality, the output folder (by default
      `Saved/Packaged/<Configuration>`) and the player.
    - **Buttons**: Cook, Package, Cancel, and Launch, with arguments for
      the player.
    - **While it runs**: the build runs on a worker thread while a
      progress bar and the log (stages and warnings) update.
  - **Project Settings panel**: the settings in the reflected property
    table, with Save and Revert. It warns when the default quality isn't
    a preset.
  - **The workspace**: both are in a new Project category of the
    editor's tools, on the project the editor was given. Without one,
    they use a sample project made under the temp folder: a startup scene
    with a player, a camera and crates.
- **Tests**:
  - **Settings**: the window and quality settings round trip, and old
    projects get the defaults.
  - **Cook**: progress is monotonic from 0 to 1, the manifest carries the
    settings, `ChooseQuality` falls back correctly, and a cancelled cook
    writes nothing.
  - **Packaging**: the staged player and pak, stale pak removal, and the
    refusals.
  - **Processes**: exit codes, the working folder, and a missing program.
  - **Build and Package window**: it packages the sample project with the
    real player and launches the packaged game headless, which exits with
    0. A cook, a failed cook, a missing player, and the settings panel's
    edits, saves and reverts are tested too.

### 25.6 Patches, DLCs and encryption

- **Encryption** (`pak::PakKey`):
  - A 32-byte key, read and written as 64 hex digits. `Generate()` makes
    a random one.
  - `Id()` is a fingerprint of the key (a ChaCha20 block under a fixed
    nonce), not the key itself.
  - With `PakWriter::SetEncryption`, each entry's stored bytes (after
    compression) are encrypted with ChaCha20 (RFC 8439, implemented in
    `pak.cpp`, checked against the RFC's test vector). So is the index.
    Each gets its own nonce, from its offset and a tag.
  - The header's flags mark the archive as encrypted, and its last field
    holds the key id. The index CRC covers the bytes as stored.
  - `PakReader::Open` takes keys and uses the one whose id matches. No
    key, or a wrong one, fails with "the archive is encrypted, and no key
    given is its key". Unknown flags are refused.
  - Neither the content nor the paths are readable on disk.
  - It keeps content from being opened with an archive tool. It isn't
    DRM: the game ships with the key.
- **Patches**:
  - `MakePatch(base, updated, patch)` writes the entries that are new or
    differ in size or CRC.
  - Removed paths are listed in `PatchRemoved.json`. The virtual file
    system hides them in the mounts below the patch, in reads and in
    `List`. A mount above the patch can still provide them.
  - `PatchReport` lists what was added, changed and removed, and counts
    what's unchanged.
  - A patch named to sort after its base (`Game_p1.apak` after
    `Game.apak`) mounts on top of it in the player.
- **DLCs**: the cooker's `dlc_name` mode cooks only the `--always`
  roots, leaving out what `dlc_base` already has. It writes
  `DLC/<name>.json` (a `DlcManifest`) instead of `Manifest.json`.
  `GamePackage::LoadManifest` merges every mounted DLC's assets, and
  `Dlcs()` names them.
- **The cooker**:
  - `encryption_key` encrypts the archive, and opens encrypted bases.
  - `patch_base` cooks in full, then writes only the patch against the
    base. `CookReport::patch` says what it holds.
  - `CookReport::in_base` counts what a DLC left out.
- **The tools**:
  - **`aether_cook`**: `--key`, `--patch-of` and `--dlc`/`--dlc-base`.
  - **`aether_pak`**: `keygen`, and `patch <base> <updated> <out>`.
    `--key` (anywhere) encrypts what it writes and opens what's
    encrypted.
- **The player**: keys come from `--key`, from `AETHER_PAK_KEY`, or from
  the key a game's player was built with (`AETHER_GAME_PAK_KEY`). It logs
  the DLCs it found.
- **The editor**: the Build and Package window has an Encrypt option,
  with a key field and Generate. Launch passes the key to the game.
- **Tests**:
  - **Keys**: the RFC 8439 vector, and keys' hex form and ids.
  - **Encrypted archives**: nothing readable on disk, a missing or wrong
    key refused, and reads through the virtual file system with the key.
  - **Patches**: added, changed and removed entries, read and listed
    through the virtual file system, with a loose folder bringing a
    removed file back.
  - **A full cook**: an encrypted 1.0, a 1.1 patch (a changed scene, a
    removed one), and a DLC that leaves out the base's assets, all played
    together. The archives refuse to mount without the key.
  - **The editor**: packaging encrypted, then launching with the key
    (exit code 0) and without it (it fails).

### 25.7 PR breakdown

1. ✅ **Done.** `.apak` archives, compression, the virtual file system
   and `aether_pak` (§25.1).
2. ✅ **Done.** The cooker (§25.2): the cook set from the startup scene
   and `always_cook`, editor-only stripping, imported data, the
   manifest, and `aether_cook`. Blueprint bytecode precompilation comes
   with the player, which runs it.
3. ✅ **Done.** Cooked textures (§25.3): BC1/3/4/5/7 with mips in
   `.atex`. Still to do from this step: ASTC for mobile, which comes with
   Phase 33's mobile targets, and precompiled shaders (DXIL, SPIR-V),
   which come with the player that loads them.
4. ✅ **Done.** The player (§25.4): `aether_game` and `aether_player`,
   the startup scene from the mounted paks with prefabs and physics, and
   the Debug, Development and Shipping configurations. Scripts, Blueprints
   and input run in the player since §26.3. Still to do: drawing the scene,
   with the renderer's RHI path.
5. ✅ **Done.** Project settings (window defaults, quality presets), the
   cook's progress and cancel, packaging with the player, and the
   editor's Build and Package window with progress, logs and Launch
   (§25.5).
6. ✅ **Done.** Patch and DLC paks, and optional archive encryption
   (§25.6). With this, every Phase 25 step is done. Still to come, as
   the notes on steps 2–4 say: ASTC textures (with Phase 33's mobile
   targets), precompiled shaders and Blueprint bytecode (with the
   renderer's RHI path and the player running them), and drawing the
   scene in the player.

## Phase 26: Ecosystem (templates, plugins, docs)

The concept is in [ROADMAP.md Phase 26](../ROADMAP.md). Step 1 is the
plugin system that templates, editor extensions and the 2D toolkit plug
into.

### 26.1 Plugins

- **Descriptors** (`aether/plugin/plugin.h`): a plugin is a folder,
  `<Name>/<Name>.aplugin`. The descriptor is JSON with `$type` "Plugin",
  read through reflection, and holds:
  - the plugin's name (an identifier that matches the folder), friendly
    name, version, description, category and author;
  - the oldest engine version it works with;
  - whether it's enabled by default, and whether it has content;
  - its dependencies (each with a name, a minimum version, and whether
    it's optional);
  - its modules (each with a name, Runtime or Editor, and a loading phase:
    PreDefault, Default or PostDefault).

  `CompareVersions` compares dotted versions. `CreatePluginScaffold` makes
  a new plugin folder: a descriptor with one runtime module, and an empty
  `Content/`.
- **Modules** are code compiled into the executables that host plugins:
  - A module class derives from `IModule` (`Startup`, `Shutdown`).
  - It registers a factory under its name with `AETHER_MODULE`.
  - `aether_module()` in `cmake/AetherModules.cmake` builds it as an
    OBJECT library, so the registration links in. The player, both
    editors and the tests get every module the build has
    (`aether_link_modules`).
- **`PluginManager`**:
  - **Discovery**: it searches the engine's `plugins/` folder and the
    project's `Plugins/`. A project plugin replaces an engine plugin of
    the same name. Misnamed or unreadable plugins are skipped with
    warnings.
  - **Resolution**: it enables the project's `plugins`, those enabled by
    default, and everything they need (each records why it's enabled). It
    refuses:
    - an unknown plugin;
    - a missing or too-old required dependency (optional ones only warn);
    - a plugin that needs a newer engine;
    - a dependency cycle, named in the error ("A -> B -> C -> A").
  - **Order**: enabled plugins come after their dependencies.
  - **Modules**: they start by loading phase, then in plugin order. A game
    gets runtime modules only, the editor both kinds. A module the
    executable wasn't built with is a warning. The manager shuts its
    modules down in reverse order.
  - **Content**: `ContentMounts()` gives each enabled plugin's `Content/`
    and its mount point, `Plugins/<Name>/`.
  - `ResolveProjectPlugins` does all of this for a project.
- **The engine's own plugins** (`plugins/`): Physics, Audio, Navigation,
  AI (which needs Navigation) and Networking.
  - Each has a descriptor (enabled by default) and a PreDefault runtime
    module that registers its components, so scenes can name them.
  - Each is built when its module is: a build without physics lists the
    Physics plugin, but its module isn't there.
- **The cooker**: it resolves the project's plugins, and fails when they
  don't resolve. The manifest lists the enabled `plugins` and the runtime
  `modules` in start order. Each enabled plugin's content is cooked into
  `Content/Plugins/<Name>/`.
- **The player**: before loading its first scene, `Game` starts the
  manifest's modules, and shuts them down with the game. A module the
  player wasn't built with is a warning.
- **The editor**:
  - The workspace resolves its project's plugins and runs their runtime
    and editor modules.
  - A new **Plugins** panel in the Project tools lists every plugin. Each
    row has an enabled checkbox and shows the plugin's version, source,
    category, modules (built in or not), description and dependencies.
    It has a search box.
  - Turning a plugin on saves it in the project, and what it needs comes
    along. A change that doesn't resolve leaves the project as it was.
    Plugins enabled by default, or needed by another, stay on.
  - **New Plugin** makes one in the project's `Plugins/`.
- **Tests**:
  - **Descriptors**: they round trip, and bad types and names are
    refused. Versions compare correctly, and scaffolding works.
  - **Discovery and resolution**: a project plugin overrides an engine
    one, and broken plugins are skipped. Dependencies come first, each
    plugin records why it's enabled, and an optional dependency only
    warns. An unknown plugin, an old dependency, a newer engine and a
    cycle are each refused.
  - **Modules**: they start by phase and dependency, editor modules start
    only when asked, a missing module warns, and shutdown runs in reverse.
  - **The engine's plugins**: they're found, and they resolve.
  - **A project plugin, end to end**: one with a module, content and an
    engine dependency is cooked. The cook reports it, refuses a missing
    plugin, and the player starts the module and stops it with the game.
  - **The Plugins panel**: default plugins stay on, New Plugin works, and
    enabling saves to the project. A plugin with a missing dependency is
    refused, leaving the project as it was.

### 26.2 Project templates

New Project starts from a template: a project with a startup scene, a
controller script, input bindings and a Blueprint. The templates have no
art; each scene is entities with transforms, tags, a camera, a script and
a Blueprint, so the starter is wired up and runs, and yours to dress.

- **`aether/templates/templates.h`** (`templates/`, `Aether::Templates`):
  - `ProjectTemplates()` lists them; `FindProjectTemplate` looks one up by id.
  - Each has an id, name, genre, description and a list of its features.
  - A template can be unavailable, with the reason. `CreateProjectFromTemplate`
    refuses those, unknown ids, and whatever `CreateProject` refuses (a bad
    name, a folder that isn't empty).
- **The templates**:
  - **Blank**: a Main scene with a camera and a Player Start, and nothing else.
  - **First Person**: the player is the camera, at eye height.
    `FirstPersonController.luau` walks, looks with the mouse or right stick,
    and jumps.
  - **Third Person**: a character, and a camera that orbits behind it.
    `ThirdPersonController.luau` moves it relative to where the camera
    looks, turns it to face where it runs, and keeps the camera at a set
    distance.
  - **Top Down**: `TopDownController.luau` moves a character along the
    world's axes (up the screen is -Z), faces where it goes, and a tilted
    camera follows from above.
  - **Vehicle**: `VehicleController.luau` is an arcade car with
    throttle, braking, drag, and steering that needs speed (reversed going
    backwards), with a chase camera. Pickups are placed along a line ahead.
  - **2D Platformer** (§26.6): a tilemap level, a platformer character
    with 2D physics and a following camera. No scripts: the controller is a
    component, so it creates `Tiles/Level.atileset`, `Tiles/Level1.atilemap`
    and the Move and Jump bindings, not scripts or Blueprints.
- **What a playable template creates**:
  - `Scenes/Main.ascene`, set as the project's startup scene.
  - `Scripts/<Controller>.luau`, whose top-level fields are the settings
    the Inspector shows (speed, look speed, jump, gravity, camera distance).
  - `Blueprints/BP_Pickup.abp`, a Blueprint that spins (Angle plus
    delta times the instance-editable SpinSpeed, applied as a rotation
    about +Y). The scene places pickups: a ring in the first three, a line
    in the vehicle one.
  - `Input/`: the Move action (WASD and the left stick), and for the
    first and third person templates Look (mouse and right stick) and Jump
    (Space and the A button), plus the `Gameplay` mapping context. The
    stick Y axes are negated or swizzled to match the keys, since the
    window layer reports GLFW's axes.
  - `always_cook` lists `Input/`, since nothing in a scene refers to it.
- **The asset database** now knows `.abp` files (as "Blueprint"). Before
  this, a Blueprint in a project had no GUID, so a scene couldn't refer to
  one, and the cooker wouldn't have found it.
- **The editor**: a **New Project** tool in the Project category lists the
  templates with their features and, for an unavailable one, why. It takes a
  name and a folder (`AetherProjects` in the home folder by default). A
  created project becomes the one the Project tools (settings, Build and
  Package, Plugins) and the plugins' modules work on
  (`EditorWorkspace::OpenProject`).
- **Playing them**: the packaged player runs a template's controller,
  Blueprint and bindings (§26.3).
- **Tests** (`test_templates.cpp`):
  - The list, including the unavailable template, and every refusal.
  - Blank's contents. Each playable template creates a project that loads,
    whose assets are in the database under their GUIDs, whose scene refers
    to them, that cooks (scene, script, Blueprint and bindings), and that
    the player loads with no warnings.
  - The Blueprint compiles and has its variables.
  - **With scripting, each controller is run from its own bindings**:
    - First person: a second of W at 5 m/s covers 5 m along -Z, diagonals
      aren't faster, mouse and look turn it, and a jump leaves the floor and
      lands back on it.
    - Third person: the camera trails behind and above, running right
      faces +X, orbiting the camera changes forward, and the distance holds.
    - Top down: movement on the map's axes, facing, and the camera settling
      above and behind at its tilt.
    - Vehicle: no steering when still, 14 m/s after a second, a top speed,
      turning right at speed, braking, reversing to the limit, and the chase
      camera's distance.
    - The scene's pickups spin a quarter turn in a second.
  - The New Project panel creates a project, announces it, refuses a taken
    name, the unavailable template and a bad name, and the workspace opens
    it.

### 26.3 The player runs scripts, Blueprints and input

A packaged game now plays what a project's scripts, Blueprints and bindings
say, not only its lifecycle callbacks and physics (§25.4).

- **Input**:
  - `InputAssetLibrary::AddFromText` adds an `.aaction` or `.amapping` from
    its text, without a database on disk (a bad one is refused, and listed
    in `Errors()`).
  - `Game` loads the manifest's input assets from the archives, registers
    the actions, and activates every mapping context, in name order.
  - The host sets keys, buttons and mouse movement on `Game::Input()`
    before each `Tick`. The window's events go in with `ApplyWindowEvents`.
    `Tick` turns them into this frame's actions, and clears the per-frame
    movement deltas after it.
- **Scripts** (when the player is built with Luau; `Game::HasScripting()`):
  - A `ScriptSystem` reads each script's source from the archives, runs
    its callbacks through the lifecycle, and ticks timers each frame.
  - It's bound to the game's input, so `Input.GetAxis2D("Move")` and the
    rest work.
- **Blueprints**: a `BlueprintSystem` loads each `.abp` from the archives
  and runs its entities' events (BeginPlay, Tick, ...) through the same
  lifecycle.
- **A scene load rebuilds all of it**: a fresh Luau host, script system and
  Blueprint system per scene, torn down before the world, lifecycle and
  input they refer to, so a level change leaves no state behind.
- **Errors don't stop play**: they're counted in `GameStats`
  (`script_errors`, `blueprint_errors`) and listed by `ScriptErrors()` and
  `BlueprintErrors()`. `GameStats` also has the running script and
  Blueprint instance counts.
- **`aether_player`**:
  - The window's events feed the game's input.
  - `--press <Key>` holds a key for the whole run, which makes a headless
    run exercise the controls, and `--report` prints where the
    Player-tagged entity ended up.
  - It logs script and Blueprint errors and exits with 3 if there were any.
  - Checked by hand with the real binary on the four cooked templates, 120
    headless frames holding W: first person ends at z = -10 (5 m/s for 2
    s), third person and top down at -12 (6 m/s), the vehicle at -28.2.
- **Not yet**: physics events (collisions and triggers) aren't passed to
  scripts or Blueprints, and the window still shows the clear colour
  (drawing waits on the renderer's RHI path).
- **Tests** (in `test_templates.cpp`):
  - Each playable template is created, cooked and run by `Game`: its input
    assets are loaded and the `Gameplay` context is active, one script and
    one Blueprint per pickup are running, a second of W moves the player as
    each controller says, the pickups have spun a quarter turn, and nothing
    errored.
  - A first person run driven by window events: a mouse move and a key
    down turn it right and move it that way.
  - `AddFromText` loads real shipped bindings, refuses bad text, the wrong
    kind and a non-input asset, and the loaded `Move` action reads WASD and
    a gamepad stick (negative Y forward) the same.

### 26.4 PR breakdown

1. ✅ **Done.** Plugins (§26.1): descriptors, discovery, resolution,
   modules, the engine's optional modules as plugins, and the cook,
   player and editor wiring.
2. ✅ **Done.** Project templates (§26.2): Blank, First Person, Third
   Person, Top Down and Vehicle, each a small starter game, and New
   Project from a template (the 2D Platformer joined with the 2D toolkit).
   The player runs their scripts, Blueprints and input too (§26.3).
3. ✅ **Done.** The editor extensibility API (§26.5): panels, menu
   items, property drawers and asset types from C++, and panels, menu
   items and asset types from Luau editor scripts.
4. ✅ **Done.** The 2D toolkit (§26.6): sprite atlases and
   animation, tilesets, tilemaps with autotiles, render batching and the
   pixel-perfect camera, the tilemap editor, 2D physics, the platformer
   controller, the 2D Platformer template and 2D lights.
5. ✅ **Done.** Documentation (§26.7): the API reference generated from
   reflection (`aether_docgen`), the manual (`docs/manual`), the sample
   projects (the templates), and version control status in the Content
   Browser.

### 26.5 The editor extensibility API

What a plugin, a game module or a project adds to the editor without
editing it. `ExtensionRegistry` (`editor/src/ui/extensions.h`) holds four
kinds of entry, each registered under an owner (a plugin or script name) so
one owner's entries leave together:

- **Panels**: a name, a category (default "Extensions") and a draw
  function. The workspace lists them as tools after the built-in ones, in
  the Tools menu and the hub, and a popped-out panel stays open when the
  list is rebuilt.
- **Menu items**: a "Menu/Sub/Item" path, an optional shortcut
  ("Ctrl+Shift+B") and an optional enabled check. Paths under `Tools/`
  join the Tools menu; any other root gets a menu of its own beside it.
  `PollShortcuts()` fires the pressed ones, except while a text field has
  focus.
- **Property drawers**: for a reflected type's name; the Inspector draws
  that widget in the value column instead of its own, for every field of
  the type. A drawer returns whether it changed the value, which feeds
  the Inspector's changed-field and undo result as any edit does.
- **Asset types**: an extension, a display name, the text of a new file
  and an opener for double-click in the Content Browser.

`ExtensionRegistry::Active()` is the registry the editor made active, so a
plugin's editor module (`ModuleType::Editor`) registers into it from
`Startup` and removes itself in `Shutdown`.

**Editor scripts.** Every `.luau` file under a project's `Content/Editor/`
runs when the project opens (and on `Reload`), with these globals:
`editor.AddPanel(name, fn)`, `editor.AddMenuItem(path, fn [, shortcut])`,
`editor.AddAssetType(name, ext, newText [, fn(path)])`, `editor.Log(text)`,
and in a panel `ui.Text`, `TextDisabled`, `Button`, `Checkbox`,
`SliderFloat`, `InputText`, `CollapsingHeader`, `SameLine`, `Separator` and
`Spacing`. Widgets that edit a value return the new one (and whether it
changed). The VM is sandboxed as game scripts are, with a smaller
instruction budget since a panel runs every frame. A script that fails to
load leaves nothing registered; an error in a callback shows in its panel
and in `Errors()` and clears when it next succeeds. Property drawers are
C++ only for now.

`LuauHost::RegisterNative` and `NativeCall` are the general piece beneath
this: a host adds its own functions to a VM, and keeps a Luau function
argument as a reference to call later.

**Tests** (`test_extensions.cpp`):

- Panels and menus: replace by owner and name, remove by owner, shortcuts,
  a disabled item, and drawing the menus headless.
- A property drawer replaces the Inspector's widget and reports the
  change; unregistering restores it; the active registry clears when it
  is destroyed.
- Asset types: extension normalising, new files numbered, and the opener.
- The workspace shows panels as tools and keeps a popped-out one open.
- Editor scripts: panels, menus and asset types register; a script that
  fails to load or is called wrongly is reported; menu actions and openers
  run Luau; every `ui.*` widget draws; reload replaces the old entries; a
  runaway callback stops at its budget; the sample project's script loads.

### 26.6 The 2D toolkit: sprites, tilemaps and the pixel-perfect camera

The `sprite2d` module (`sprite2d/`) holds the 2D toolkit's data and its
render batching. It is engine-only, so it builds and tests headless. 2D
physics, 2D lights and the tileset editor are the next steps (§26.4).

- **Sprite atlases** (`.aatlas`, JSON): one texture, referenced by GUID so
  the cooker follows it, holding named frames (a pixel rectangle and a
  pivot as a fraction of the frame, from its top left) and named clips
  (frame names, frames per second, loop). Loading refuses a frame without
  a size, a repeated name, and a clip naming a missing frame.
  `PackRects` shelf-packs rectangles into the smallest power-of-two
  texture up to a limit, `ComposeAtlasImage` draws the images into it, and
  `AtlasFromGrid` cuts a sprite sheet into numbered frames.
- **Components**: `Sprite` (atlas, frame, tint, flips, sorting layer and
  order, pixels per unit), `SpriteAnimator` (clip, time, speed, playing;
  `UpdateSpriteAnimations` advances it, a clip that doesn't loop holds its
  last frame and sets `finished`) and `TilemapRenderer`. They sit on the
  entity's Transform: x and y position, z depth, rotation about z.
- **Tilesets** (`.atileset`): a texture cut into tiles (size, margin,
  spacing), a solid flag per tile, and autotiles: 16 tile numbers indexed
  by which of the four neighbours hold the same autotile (north 1, east 2,
  south 4, west 8).
- **Tilemaps** (`.atilemap`): layers of cells, row 0 at the bottom; a cell
  is a tile number, empty (-1), or an autotile reference (-2 - index),
  which resolves by its neighbours' mask when drawn or queried.
  `IsSolidAt` asks the colliding layers; `LocalToCell` maps positions to
  cells.
- **Batches**: `BuildSpriteBatches` turns every sprite and tilemap layer
  into quads, sorts them by sorting layer, order, then z, and merges runs
  that share a texture into `SpriteBatch`es (vertices and indices), with an
  optional view rectangle that culls (a tilemap only visits the visible
  cells).
- **Pixel-perfect camera**: `ComputePixelViewport` shows the reference
  resolution at the largest whole-number scale the window allows, centred
  with bars (or filling the window, still at a whole scale), and gives
  the orthographic size in world units; `SnapToPixelGrid` rounds a
  position to the art's pixels.
- The asset database knows the three extensions, and scans them for the
  GUIDs they hold (a tilemap's tileset, a tileset's and an atlas's texture),
  so the cooker packages what a 2D scene reaches.

**Tests** (`test_sprite2d.cpp`): atlas frames, UVs and JSON (and refusing bad
data); clip frames, looping and holding; packing with no overlaps, padding
and composing the image; tileset geometry with margin and spacing;
tilemap editing, resizing and autotile masks (corners, edges, holes);
solidity; the JSON round trip and its errors; batch order and merging;
quad geometry for pivots, flips and a quarter turn; culling; the animator;
components through reflection; and the pixel-perfect viewport at several
window sizes.

**The tilemap editor** (`editor/src/sprite2d`). `TilemapEditDocument` edits a
tilemap and its tileset: Paint, Erase, Rectangle, Fill (a 4-connected flood
fill) and Pick tools with a brush that is a tile or an autotile; strokes
that change nothing record nothing and one that does is one undo step;
layers (add, remove down to one, rename, "blocks movement"), resizing, the
tileset's solid flags and autotile rules (the tile shown per neighbour
mask), and saving both files. `TilemapPanel` draws it: layers, the tiles as
numbered colour swatches (solid ones outlined red), the autotile masks, and
the map as a grid (drag to paint, right-drag to erase, wheel to zoom,
middle drag to pan). It needs no texture, so it runs headless and in every
host. The workspace lists it under a new **2D** category on a sample
platform level whose ground is an autotile.

Its tests (`test_tilemap_editor.cpp`): strokes as single undo steps, empty
strokes, rectangles with corners in any order, flood fill regions and
refilling with the same value, picking, autotile painting and rules,
layers, solids and resizing with undo, saving both files, the panel
drawing without editing anything, and the workspace's 2D tool.

**2D physics** (`sprite2d/physics2d.h`). Box2D v3 was the plan; a compact
solver of our own does what 2D games here need without another dependency
to build on every platform. `Physics2D` steps a world's bodies:

- **Components**: `Rigidbody2D` (static, kinematic or dynamic; mass, gravity
  scale, damping, velocity) and `Collider2D` (a box of half extents or a
  circle, an offset, friction, restitution, a trigger flag, a layer and a
  mask). A collider with no body is static. Bodies move the Transform's x
  and y; its z and rotation are left alone, and bodies don't rotate.
- **Step**: gravity and damping, then movement, then candidate pairs by a
  sweep along x, then sequential impulses over `iterations` passes with
  positional correction (restitution only for impacts above 1 m/s, so a
  resting body doesn't jitter, and friction limited by the normal impulse).
  Frames longer than `max_step` are cut. Kinematic bodies move by their
  velocity and push dynamic ones.
- **Layers**: two colliders meet only if each one's layer is in the other's
  mask.
- **Triggers**: overlaps are reported and nothing is pushed.
- **Tilemaps**: solid cells of every `TilemapRenderer`'s map collide as
  static boxes (with the entity's position and pixels per unit). A contact
  face whose neighbouring cell is also solid is dropped, so a body slides
  over the seams between tiles without catching.
- **Events**: `Events()` lists the last step's transitions: Begin, End,
  TriggerEnter and TriggerExit, naming the moving body first (a tile
  contact has a null second entity). A resting contact isn't a new Begin
  every frame, and all the tiles a body touches count as one contact.
- **Queries**: `IsGrounded` and `WallSide` for platformer controllers,
  `Raycast` (bodies and solid tiles, with a layer mask; tiles are layer 1)
  and `OverlapBox`.

Tests (`test_sprite2d.cpp`, `Physics2D_*`): falling and resting on a floor
with grounding; stacking; a heavy box pushing a light one; a circle's
bounce, friction against none, and a kinematic platform; layer masks and
trigger enter and exit; contact begin and end events; tile floors, running
along seams at full speed, stopping at a wall with its side, and tile
contact events; ray and overlap queries against boxes, circles and tiles;
and the components through reflection.

**The platformer controller and the template** (`sprite2d/platformer.h`).
- **`PlatformerController2D`** on an entity with a Rigidbody2D and a
  Collider2D: run speed with ground and air acceleration, a jump speed,
  coyote time (a jump still works shortly after leaving a ledge), jump
  buffering (a press just before landing counts), variable height (letting
  go early keeps `jump_cut` of the rise), a faster fall (`fall_gravity_scale`)
  and a terminal speed. It flips a Sprite on the entity to face the way it
  runs. The host sets `input_move` and `input_jump` each fixed step;
  `UpdatePlatformers` runs before `Physics2D::Step`, which supplies grounding.
- **`CameraFollow2D`**: eases a camera toward the first controller's entity
  (plus an offset), optionally inside bounds; `UpdateCameraFollow2D`.
- **In the player**: `Game` creates a `Physics2D` for each scene, reading the
  scene's tilemaps and tilesets from the package, and steps it each fixed
  step after the 3D physics: the controllers get the "Move" x axis and the
  "Jump" action, then the bodies and the following cameras move.
  `Game::Physics2D()` exposes it.
- **The 2D Platformer template**: a 48 x 16 tile level (ground with a pit,
  platforms, a step and a tall block at the end) on a tileset whose ground
  is an autotile, a player with a body and the controller, and an
  orthographic camera 11.25 units high (a 320 x 180 view at 16 pixels a
  tile) that follows it. Move is A and D, the arrow keys and the left
  stick; Jump is Space, W, Up and the A button, held for height.
- **Tests**: the template's files, dependencies and scene components; cooking
  it (the tilemap and tileset are packaged because the scene refers to them);
  and playing it in the player: landing, acceleration and stopping, held and
  tapped jumps, falling into the pit, facing, crossing the pit with a
  jump, and stopping at the end wall.

**2D lights** (`sprite2d/lights2d.h`). The module computes the lighting; a
renderer uploads the light map or draws the polygons.
- **`Light2D`**: a global light (the same everywhere: ambient), a point
  light (colour, intensity, a radius where it reaches 0, and a falloff
  power: 1 is linear) or a spot light (a direction in degrees added to the
  entity's rotation, a full-bright inner half angle and an outer one where
  it reaches 0, with a smooth edge). Lights can be disabled, can skip
  shadows, and carry a layer mask.
- **`ShadowCaster2D`**: a box that blocks light. Solid tilemap cells block
  it too; `GatherOccluders` turns them into segments, emitting only the
  edges that face an empty cell, so a wall is a line and a 3 x 3 block has
  12 edges, not 36.
- **`LightAt`** sums the global lights and each light that reaches the point
  (distance falloff, spot cone, and shadow: `Occluded` tests the line from
  the light). **`BuildLightMap`** evaluates it over a grid of cells: empty
  cells at their centres, and a solid cell takes the brightest of its empty
  neighbours so a wall's face glows like the air in front of it. `Sample`
  blends bilinearly and clamps at the edges.
- **`VisibilityPolygon`**: what a light can see, by casting rays at every
  occluder end (and a hair either side) and keeping the nearest hit, bounded
  by a square round the light; for drawing the lit area or its shadow.

Tests (`Lights2D_*`): point falloff, tint, steeper falloff, disabled and
global lights adding; spot cones, the soft edge, the entity's rotation and
the light's own direction; casters blocking, passing beside, ignoring
shadow-free lights and region culling; tile walls as lines; a light map
with a shadow behind a wall, lit wall faces, sampling and refused regions;
the visibility polygon with and without a wall; and the components through
reflection.

### 26.7 Documentation

- **The API reference** (`engine/src/docs/api_docs.cpp`, `tools/docgen`).
  `docs::GenerateApiMarkdown`, `GenerateApiJson` and `WriteApiDocs` document
  every type in the reflection registry: components (registered with the
  ECS by name), structs and enums, each with a table of its fields (type,
  editable / read-only / internal, not saved, editor only, replicated,
  Blueprint, then tooltip, range, unit and group), its functions with
  signatures and flags, and an enum's values. Types link to their entries;
  `AssetRef<T>` is written where it's used, and plain data types (numbers,
  strings, arrays) have no entries unless `skip_scalars` is off. Entries are
  sorted by name and the output is deterministic. `aether_docgen --out <dir>`
  registers all the engine's modules and writes `API.md` and `api.json`;
  the Windows release zip carries the manual and the reference generated
  from that build (with physics). A game or plugin documents its own types
  by registering them and calling `WriteApiDocs`.
- **The manual** (`docs/manual`): getting started, projects and assets,
  scripting, input, 2D, packaging and shipping, plugins and editor
  extensions, and the API reference. The templates are the sample projects.
- **Tests**: `test_api_docs.cpp` (a reflected type's table, its escaped
  tooltip, ranges, units, flags, functions and enum values; the JSON with the
  same facts; sorted, one entry per name, links; both files written,
  deterministically; type names) and `test_manual.cpp` (every chapter exists
  and is in the contents, every relative link resolves, and the manual names
  the real templates, tools and flags).

**Version control status in the Content Browser** (`engine/include/aether/assets/vcs.h`,
`editor/src/content`).
- **`VcsStatus`**: the repository's changed files by repository-relative
  path, each Untracked, Renamed, Added, Modified, Deleted or Conflicted,
  with the queried folder's `prefix` inside the repository. `Of`, `OfLocal`
  (a path relative to the queried folder) and `OfFolder` / `OfLocalFolder`
  (the worst state of everything under a folder; whole folder names only).
  States combine by severity: Conflicted, Deleted, Modified, Renamed, Added,
  Untracked, Clean.
- **`ParseGitStatus`** reads `git status --porcelain=v1 -z`: the two status
  letters decide the state (a conflict is a `U`, `AA` or `DD`; then
  deleted, renamed, added, modified), a rename also marks the old name
  Deleted, ignored entries are left out and malformed ones skipped.
  **`QueryGitStatus(dir)`** runs `git rev-parse --show-prefix` and
  `git status` there (untracked files listed one by one); outside a
  repository, or with no git, it returns `available == false` with the reason.
- **`ApplyVcsStatus`** sets each `ContentEntry::vcs`: an asset takes the worse
  of its own file and its `.ameta` sidecar (a new asset's sidecar is
  untracked before the asset is committed), a sub-asset follows its source,
  a folder shows its worst child.
- **The Content Browser tool** (Project category): a folder tree and a
  listing, each with a coloured badge (M, A, ?, D, R, !), a search that
  looks in subfolders, a "Changed only" filter, Refresh (which asks git
  again), a count of changed files, and a "not under version control" note
  otherwise. Double-clicking a folder opens it, and an asset whose extension an
  editor extension registered runs its opener (§26.5).
- **Tests** (`test_vcs.cpp`): parsing every status pair, rename, ignored and
  malformed input; local paths, folder aggregation and severity; badges on a
  real content folder, including a sidecar-only change; a real `git init`
  repository with modified, untracked and deleted files (skipped where git
  isn't installed), queried from a subfolder and from the root; the panel's
  listing, filter, navigation, search and drawing; and the workspace tool.

---

# Phase 27: Cinematics (sequencer)

The concept is in [ROADMAP.md Phase 27](../ROADMAP.md). Step 1 is the data and
playback core, in its own module, `sequencer/` (`aether::seq`).

### 27.1 Level sequences

- **`LevelSequence`** (`.asequence`, JSON, `kVersion = 1`): a name, `duration`
  in seconds, `fps` (for frame snapping and rendering) and a list of tracks.
  `EffectiveDuration` is the duration, or the last key's time when the
  duration is 0.
- **Tracks** are bound to an entity by `EntityGuid`, so a sequence survives
  scene reloads and prefab instancing. Two kinds so far:
  - **Transform**: three position channels (x, y, z) plus a list of
    rotation keys (quaternions) blended by slerp the short way round. It
    writes the entity's `Transform` position and rotation; a channel with no
    keys leaves its axis alone.
  - **Property**: a reflected `component` and `field`, written through the
    reflection registry's field offset. It can key floats (f32/f64),
    integers (rounded and clamped to the type), bool, enums (by their
    underlying type) and structs whose members are all floats (one channel
    per member).
- **Channels** hold keys (time, value, interpolation). `Constant` holds the
  value until the next key, `Linear` blends, `Bezier` is a cubic Hermite
  segment with in and out tangents. Before the first key and after the last,
  the value holds.
- **Diagnostics** (`ValidateSequence`): SQ001 no tracks; SQ002 keys not in
  increasing time order; SQ003 a key outside the duration; SQ004 a property
  track with no component or field; SQ005 a repeated track id; SQ006 bad
  duration or fps; SQ007 a track without an id; SQ008 a track bound to no
  entity; SQ009 a transform track without three channels; SQ010 a property
  track without channels. `SequenceFromJson` refuses malformed files with
  a message and leaves its output untouched.
- **`SequencePlayer(sequence, world, guid index)`**: `Bind` resolves every
  track once (problems, such as a missing entity, component or field, are
  reported once through `Problems()` while the other tracks still play); an
  entity that dies is re-resolved when it comes back. `SetTime` clamps to the
  duration, `Evaluate` writes the state at the current time and depends only
  on the time, so two players on the same sequence agree exactly. `Play`,
  `Pause`, `Stop`, `SetRate` (negative plays backwards), `loop` and
  `on_finished`, driven by `Update(dt)`.
- **Tests** (`test_sequence_core.cpp`): channel interpolation in every mode;
  slerp the short way; each diagnostic; JSON round trip and refusals;
  transform and property tracks; once-only problems; rebinding after a
  destroyed entity; playback, looping and `on_finished`; determinism.

### 27.2 Events, visibility and the scene component

- **Event track**: `events` of (time, name, payload). The player calls
  `on_event` for each key the playhead crosses while playing forward: once
  each, in time order, an event at t = 0 on the first frame, and a loop fires
  the keys at the end and then those at the start. Scrubbing (`SetTime`),
  `Evaluate` and backward playback fire nothing, so `Evaluate` stays a pure
  function of time. The binding is optional (the entity the event is about).
- **Visibility track**: one channel of 0 and 1 keys (held until the next); it
  sets the bound entity's `Active` flag, calling `set_active(entity, on)`
  only when the value changes. A host routes that through
  `Lifecycle::SetActive`, so OnEnable/OnDisable fire.
- **Diagnostics**: SQ011 an event key with no name; SQ012 a Visibility track
  without exactly one channel of 0/1 values; SQ013 keys on a track of the
  wrong kind. SQ008 doesn't apply to an Event track.
- **`SequenceComponent`**: `sequence` (asset path), `auto_play`, `loop`,
  `rate`, `destroy_when_finished`, and `time` and `playing` written back (not
  saved). Its methods `Play`, `Pause`, `Stop`, `SetTime`, `SetRate`,
  `SetLoop`, `IsPlaying` and `GetTime` are reflected as Blueprint nodes and
  Luau calls, and queue commands like `AudioSource`. The **`Sequencer`**
  library has `PlaySequence(sequence, loop)` (an entity that plays and is
  destroyed at the end) and `StopAll`.
- **`SequenceSystem`**: a player per component (made when it appears or its
  `sequence` changes), queued commands, Update, time written back, players of
  destroyed entities dropped, problems reported once. `Events()` is what
  happened in the last Update: a Marker (name, payload, the track's entity) or
  Finished (the sequence). The host dispatches them to Blueprints as
  `Event.OnSequenceEvent` (name, payload) and `Event.OnSequenceFinished`.
  Wiring it into the player's scheduler comes with the asset loading in a
  later step, as the audio system's does.
- **Physics note**: a body the sequence moves should be kinematic.
- **Tests** (`test_sequence_system.cpp`): both new tracks' JSON and
  diagnostics; events once, not on scrub or backward, across a loop wrap, with
  muted tracks and bound events; visibility and its change-only hook;
  auto-play, every command, event and finished collection, Lifecycle
  visibility, destroy-when-finished and the library, once-only problems,
  dropped players, and the reflected Blueprint surface.

### 27.3 Spawn, Camera Cut, Audio and Animation tracks

The player drives these through callbacks, so the sequencer module links no
audio, animation or prefab code and tests headless; the `SequenceSystem`
forwards them (below).

- **Spawn track**: `spawns` of (time, duration, prefab, position, rotation).
  A prefab is alive exactly while the playhead is in [time, time + duration)
  (duration 0: to the end). `on_spawn(track, parent, key)` makes it (the
  binding, when it has one, is the parent), `on_despawn(entity)` removes it.
  Leaving the range, `Stop` and destroying the player despawn it; scrubbing
  back in respawns it; evaluating twice inside the range spawns once; one
  that gameplay destroyed isn't remade while the playhead stays inside.
- **Camera Cut track** (no binding): `cuts` of (time, camera GUID). The cut in
  force at the playhead gives its camera priority `kCutPriority` (1000) so
  `FindActiveCamera` picks it; the previous camera gets its own priority back,
  as does the one cut away from, before the first cut, and when the player is
  destroyed. `on_camera_cut(camera)` is told of each change (null before the
  first cut).
- **Audio track**: `audio` keys of (time, cue, action Play / Stop / FadeIn /
  FadeOut, volume_db, fade). **Animation track**: `anims` of (time, montage,
  action Play / Stop, rate), bound to the animated entity. Both fire like
  Event keys (once, forward only) through `on_audio` / `on_animation`.
- **JSON**: `"type": "spawn" | "cameracut" | "audio" | "animation"` with the
  key arrays `spawns`, `cuts`, `audio` and `animation`; `$version` stays 1.
- **Diagnostics**: SQ014 a Spawn key with no prefab; SQ015 a negative spawn
  duration; SQ016 a cut to no camera; SQ017 an Audio key with no cue; SQ018 an
  Animation key with no montage. SQ002 and SQ003 cover every key list, SQ013
  keys on the wrong kind of track. Spawn, Camera Cut and Audio tracks need no
  binding; an Animation track does.
- **`SequenceSystem`**: `SetSpawner(fn)` is how a host instantiates prefabs;
  despawning goes through `Lifecycle::Destroy`. Audio and Animation keys come
  out of `Events()` as `Kind::Audio` (name = the cue, payload = the action,
  `value` = volume dB, `fade`) and `Kind::Animation` (name = the montage,
  payload = the action, `value` = the rate), with `subject` the track's
  entity, for the host to give to its audio and animation systems.
- **Tests**: JSON round trip and refused actions; every new diagnostic;
  spawn range, scrubbing, Stop, destructor and externally destroyed spawns;
  camera cuts against `FindActiveCamera`, restoring priorities and the
  destructor; audio and animation firing once and forward only; and the
  system's forwarding and Lifecycle despawn.

### 27.4 Sequences in the player

- **Asset type**: `.asequence` imports as "Sequence". Its GUID strings (a Spawn
  key's prefab, a Camera Cut's camera is an *entity* GUID, so not that) are
  scanned as dependencies, so cooking a sequence cooks the prefabs it spawns.
- **`Game::FindSequence(path)`**: reads the sequence from the package by
  asset path, once per scene (a miss is remembered), with a warning when it
  isn't in the package or can't be parsed.
- **Cooking a sequence**: a scene names its sequences by path
  (`SequenceComponent.sequence`), which the cooker doesn't follow yet, so a
  project lists its sequence folder under `always_cook` (for example
  `Sequences/`), as the platformer template does for `Input/`.
- **The system**: each scene's runtime owns a `SequenceSystem` (so it goes
  with the scene), registered as the `Sequencer` library's active one. The
  `Player.Sequencer` system runs in Update after `Player.Update` and before
  `Player.Scripting`, so scripts and Blueprints see this frame's events.
- **Spawn tracks** resolve their `prefab` as a GUID or an asset path, and
  place it at the key's position and rotation (relative to the track's
  entity when it has one, by its position and rotation), through
  `InstantiatePrefab` on the same cached prefab data the scene uses. A prefab
  that isn't in the package is a warning.
- **Events**: Marker keys reach Blueprints as `Event.OnSequenceEvent` (name,
  payload) and the end as `Event.OnSequenceFinished` (the sequence), on the
  `SequenceComponent`'s entity. Audio and Animation keys are collected in the
  system's `Events()` but the player has no audio or animation runtime to hand
  them to yet.
- **Tests** (`test_player.cpp`): a sequence played from a hand-built pak (the
  moved entity, a prefab spawned at its time and position, the final pose, no
  warnings); a missing sequence warned about once; and a project cooked with
  `always_cook` that imports the sequence as "Sequence" and plays it.
- Not in this step: `CineCamera` (a camera component with focal length,
  sensor and aperture that derives the field of view), cooker following of
  scene sequence paths, and audio and animation in the player.

### 27.5 The sequencer editor

- **`SequenceDocument`** (`editor/src/sequencer/`): a `LevelSequence` being
  edited, with whole-sequence undo and redo (a sequence is small), a dirty
  flag and the diagnostics. A drag is one undo step (`BeginEdit` ..
  `EndEdit`). Tracks: add (with the channels its kind needs and a unique
  id), remove, rename, bind, mute, lock, and the Property track's component
  and field. Keys: add a value key (one at the same time has its value
  replaced), move any key (value, rotation, event, spawn, cut, audio,
  animation; the list is re-sorted and the new index returned, and a move onto
  another value, rotation or cut key's time is refused), set a value or
  interpolation, add and edit rotation and event keys, remove. Duration, fps
  and name. A locked track refuses every edit, including removal, until it is
  unlocked. `Load` replaces the document (and clears the history), and leaves
  it unchanged when the file can't be read; `Save` clears the dirty flag.
- **`SequencerPanel`**: a toolbar (undo, redo, add a track of any kind,
  play / pause / stop, loop, the time, zoom, a count of problems), a timeline
  (a ruler with a tick each second, a header per track with mute and lock
  toggles, a row per value channel and key list with the keys as diamonds,
  and a red playhead; click the ruler to scrub, snapped to the frame rate;
  click a key to select it and drag it to move it), and an inspector (the
  track's name, entity and property target, the selected key's time, value,
  interpolation or event name and payload, delete, and the diagnostics).
  The panel draws without textures, so it runs headless.
- **Preview**: the panel scrubs and plays a `SequencePlayer` on the world
  and GUID index the host gives it (the sample scene in the workspace),
  rebuilt whenever the document changes so undo and edits show at once.
  Spawn tracks make empty entities there and remove them again, including
  when the panel is destroyed; audio and animation keys do nothing in the
  preview.
- **Workspace**: the **Sequencer - Intro** tool (category Cinematics) opens a
  sample cutscene: a door that slides open on a curve, markers, and a light
  that switches on.
- Not yet: editing the keys of Spawn, Camera Cut, Audio and Animation tracks
  (they show on the timeline and can be moved and deleted, but their fields
  are changed in the file), curve and tangent editing, multi-key selection,
  and previewing in the Viewport of an open level.
- **Tests** (`test_sequencer_editor.cpp`): track edits with undo and redo;
  sorted keys, replace, move, remove; rotation and event keys; a drag as one
  undo step; locked tracks; diagnostics following edits; save, load, dirty and
  a failed load; the panel scrubbing deterministically, playing, pausing,
  stopping, looping and following edits and undo; preview spawns removed with
  the panel; drawing and selection headless; and the workspace tool.

### 27.6 CineCamera and movie render

- **`CineCamera`** (`scene/gameplay.h`): a lens beside a `Camera`:
  `focal_length_mm` (35), `sensor_width_mm` (36), `sensor_height_mm` (20.25, a
  16:9 gate), `aperture_f` and `focus_distance` (stored for depth of field).
  `CineFovDegrees` is the vertical field of view, 2 * atan(sensor_height / (2 *
  focal_length)), kept to 1..170 degrees and safe for zero or negative
  values. `ApplyCineCameras(world)` writes it into each entity's `Camera`
  (making it perspective; a CineCamera without a Camera is left alone) and
  returns how many it updated. The renderer and `CameraProjection` are
  unchanged: the CineCamera wins over a hand-edited `Camera.fov_degrees`.
  Every field is a reflected float, so a Property track can zoom the lens
  (component `CineCamera`, field `focal_length_mm`); the `SequenceSystem`
  applies the lenses at the end of each update, and the movie loop after each
  frame. No `.asequence` change.
- **`SequencePlayer::AdvanceTo(time)`**: moves the playhead forward in one
  step, firing the Event, Audio and Animation keys on the way (once each, the
  one at t = 0 included on the first call) and applying the tracks. Backward
  fires nothing.
- **`RenderMovie(sequence, world, guids, options, sink, setup)`**
  (`sequencer/movie.h`): the headless frame loop. Frame i is at exactly i /
  fps seconds (from the integer, so nothing drifts); a sequence has
  `MovieFrameCount` = floor(duration * fps) + 1 frames, the last at the end
  (2 s at 24 fps: 49). For each frame it advances the player, applies the
  CineCameras and calls `sink(frame, time, world)`; returning false stops
  (stopping on the last frame still counts as complete). Options: `fps`
  (0: the sequence's), `start_frame` and `end_frame` (events before the
  range don't fire), `fire_events`. `setup` gets the player first, for its
  hooks (the Spawn spawner, `on_event`, `on_audio` ...). The result has the
  frames rendered, the total, whether it completed, and the problems (a bad
  frame rate, no tracks, a start past the end, and what couldn't be bound).
  The same sequence on the same world renders the same frames every time.
  Camera Cuts and spawns are undone when the render ends. What a sink does with
  a frame (draw it, write an image, encode) is the caller's.
- **Tests** (`test_cine_movie.cpp`): the field of view for known lenses and
  bad values; writing the Camera and leaving others alone; a Property track
  zooming the lens; reflection; frame counts and exact times; determinism, the
  frame range and another frame rate; stopping and bad options (and an orphan
  track reported while the rest renders); events once each, from a later start
  and silenced; and camera cuts and a lens zoom in force on the right frames,
  with the cut undone afterwards.
- Still to come: Fade and Subsequence tracks, a command-line movie renderer
  (it needs scene loading and a draw path), and depth of field using the
  aperture and focus distance.

### 27.7 Fade and Subsequence tracks

- **Fade track**: one channel named `amount` (0 to 1, any interpolation) and a
  `fade_color` (RGBA, default opaque black); no entity. The player's `Fade()`
  is the strongest fade at the playhead: the highest amount among the
  unmuted Fade tracks and the sequences playing inside this one, with that
  track's colour. `on_fade` is called when it changes, and `Stop` clears it.
  `SequenceSystem::Fade()` is the strongest across every playing sequence
  (a finished one holds its last value, so a fade-out stays out). Drawing it
  over the screen is the host's (the UI module has no screen fade yet);
  `RenderMovie`'s sink reads it from the player it got through `setup`.
- **Subsequence track**: `subs` of (time, duration, sequence path, offset,
  scale); no entity. While the playhead is in [time, time + duration) the
  child plays at its own time `(t - time) * scale + offset`, so a child can
  start part way in or run faster. `duration` is required (above 0), so a
  sequence's length never needs the child loaded. The child is found by a
  `SequenceResolver` the player is given (the `SequenceSystem` passes its
  lookup, `RenderMovie` takes one in its options); without one the track
  reports a problem and plays nothing.
- **A child is a player of its own**, made when the parent is bound, that
  reports through the parent's hooks as they are when they fire (events,
  audio, animation, spawns, camera cuts, visibility). Its events fire once
  each as the parent's playhead crosses them, scaled and offset, and those at
  the child's start fire as it enters the range. Leaving the range the child's
  last pose is applied (its first, going backward), then what it spawned is
  removed and its cuts and fade released; destroying or stopping the parent
  does the same. A child's fade merges into the parent's.
- **Cycles and depth**: a child whose path is already in the chain of
  parents is refused ("plays itself"), as is nesting deeper than
  `kMaxSubsequenceDepth` (4); both, a child that isn't found, and a child's
  own problems are in the parent's `Problems()`. Pass the sequence's own path
  to the player (the system does) to catch a sequence that plays itself
  straight away; without it the cycle is caught one level later.
- **JSON**: `"type": "fade"` (with `fade_color` and one `amount` channel) and
  `"type": "subsequence"` (with `subs`); `$version` stays 1.
- **Diagnostics**: SQ019 a Fade track without exactly one channel; SQ020 a
  fade amount outside 0..1; SQ021 a subsequence key with no sequence; SQ022 a
  duration not above 0; SQ023 a scale not above 0.
- **Editor**: Fade and Subsequence are in the Add Track menu (Fade gets its
  amount channel; subsequence keys show on a timeline lane and can be moved
  and deleted, their fields are set in the file for now).
- **Tests** (`test_sequence_fade_sub.cpp`): the fade's amount, colour,
  change notifications and Stop; the strongest of several tracks and muting;
  JSON and every new diagnostic; a child at its local time, scale and offset
  (and back and forth); events forwarded once and spawns removed on leaving;
  the destructor releasing spawns, cuts and fade; self reference, mutual
  cycles, the depth limit, missing sequences and no resolver; a child's own
  problems; the system's strongest fade and nested playback; and rendering a
  movie with a subsequence and a fade.

### 27.8 PR breakdown

1. Level sequences and the player core (done, §27.1).
2. Event and Visibility tracks, the SequenceComponent and system (done, §27.2).
3. Spawn, Camera Cut, Audio and Animation tracks (done, §27.3).
4. Sequences in the player and asset pipeline (done, §27.4).
5. The sequencer editor (done, §27.5).
6. CineCamera and the movie-render loop (done, §27.6).
7. Fade and Subsequence tracks (this step, §27.7).
8. A release (v0.27.0) that carries the Phase 26 documentation and lights and
   all of Phase 27.


---

# Phase 28: Save Game and Persistence

The concept is in [ROADMAP.md Phase 28](../ROADMAP.md). Step 1 is save slots,
in its own module, `save/` (`aether::save`, `Aether::Save`).

### 28.1 Save slots

- **What is saved**: any reflected struct (`AETHER_REFLECT`), through the
  reflection layer's JSON serializer, so its schema version (`$v`) and its
  migration hook (`reflect::RegisterMigration`) are the ones every other
  asset uses: loading an older save runs the hook, a field the save lacks
  keeps its default, one it doesn't know is skipped with a warning.
- **`.asav`**: one file per slot, `<slot>.asav`, JSON text in an envelope:
  `$type` ("aether.save"), `format` (1), `type` (the struct's name),
  `type_version`, `slot`, `timestamp_utc`, `checksum` (CRC-32 of the data's
  compact JSON, as 8 hex digits) and `data`. Readable and diffable, and the
  Save Inspector (a later step) opens it.
- **`SaveSystem(directory)`**: `Save`, `Load` (by type or template),
  `Exists`, `ListSlots` (slot, type, version, time, size, validity; a damaged
  file is listed as invalid), `DeleteSlot`, and `Active()` / `MakeActive()`
  for the Blueprint nodes. Results are `SaveResult` (ok, an error code, a
  message and warnings), never exceptions.
- **Slot names** are 1 to 64 of `A-Z a-z 0-9 _ -`, so a name can't reach
  outside the folder (`InvalidSlot` otherwise).
- **Errors**: `NotFound`, `IoError`, `Corrupt` (not a save, truncated, an
  unknown envelope format, a checksum that doesn't match), `WrongType` (the
  slot holds another struct) and `FutureVersion` (saved by a newer version of
  the struct than the code has: refused, not guessed at). A failed load never
  touches the object.
- **Atomic writes** (`fs::WriteFileAtomic`): the data goes to `<file>.tmp` and
  is renamed over the target, so a crash leaves the old save or the new, and
  a failed write removes the temp file. Each save first copies the existing
  file to `<slot>.asav.bak`; loading a missing or `Corrupt` file falls back to
  the backup (with a warning), while a wrong type or a newer version doesn't.
  `DeleteSlot` removes both. (An fsync before the rename, for a true
  power-loss guarantee, is not done yet.)
- **Async**: `SaveAsync` turns the object into text on the calling thread, so
  it may change right after, and a worker thread writes the file. Saves are
  written in the order made; the callback runs from `Pump()` on the pumping
  thread; `Flush()` waits for the queue; the destructor finishes what is
  queued (undelivered callbacks are dropped). Synchronous and async calls
  share one lock, so they never interleave on a file.
- **Where saves live** is the caller's: the directory is a constructor
  argument. The per-user folder on each platform comes with the player wiring.
- `fs::ListDirectory` lists a folder's files, sorted.
- **Tests** (`test_save.cpp`): a struct round trip and the envelope; slot name
  validation; listing, existence and deletion; atomic writes and the file
  helpers; the backup and recovery; corruption, truncation and unknown
  formats; wrong type and newer versions; migration and defaults; async
  completion, ordering, the destructor draining and sync/async mixing; and
  the active system.

### 28.2 World state

- **`SaveableEntity`** (component, in `save/`): `tag` (free text) and `fields`:
  `"Door.open"` keeps one field of the entity's `Door` component, `"Door"` the
  whole component. Names are the reflected ones. A list on the entity (not a
  type-level flag) because "a door is open" is per instance, a designer can
  edit it in the Inspector, and it needs no reflection change; a
  `Field_SaveGame` flag that supplies defaults is a possible follow-up. It
  needs the entity's `IdComponent`.
- **`WorldSnapshot`** (reflected, so it can be a field of the game's own save
  struct or saved on its own through `SaveSystem`): `entities` (`SavedEntity`:
  guid, tag, and `SavedComponent`s of component name plus compact JSON of the
  listed values) and `destroyed` (guids).
- **`CaptureWorld(world, guids, tracker, report)`** refreshes the GUID index,
  and for every saveable entity (in guid order, so equal state gives an equal
  snapshot) writes the listed fields of each component. Warnings (also logged)
  for an entity with no GUID, two entities sharing a GUID, a component or
  field that doesn't exist, a component the entity lacks, and a component that
  isn't reflected.
- **Destroyed entities** can't be captured, so a **`WorldTracker`** remembers
  which saveable entities the scene started with: `Begin` right after the
  scene loads, before `RestoreWorld`. Capture then lists the baseline guids
  that are gone as `destroyed`; it works with plain `World::DestroyEntity`
  (no hook to remember to call). With a tracker, saveable entities that
  weren't in the baseline (made while playing) are skipped and counted in
  `CaptureReport::skipped_runtime`; spawn recording (a prefab plus overrides)
  is a follow-up, as are Blueprint and script variables.
- **`RestoreWorld(world, guids, snapshot, report, lifecycle)`** finds each
  entity by GUID (so a freshly loaded scene with new entity handles works),
  lays the saved values over the live component and loads it (the unlisted
  fields keep the scene's values; a whole component's `$v` runs the migration
  hook for an older version), and destroys the `destroyed` entities (through
  the `Lifecycle` when given, so OnDestroy fires; restoring twice is
  harmless). A component the entity lacks is reported, not added. It returns
  true when nothing went wrong; otherwise the `RestoreReport` has the
  `missing` entities, `unknown_components`, `unknown_fields` and `warnings`,
  and everything that could be restored was. Call it after the scene loads
  and before play starts.
- **Tests** (`test_world_state.cpp`): a round trip into a fresh scene, with
  unlisted fields and non-saveable entities untouched; only the listed fields
  in the snapshot, and equal state giving equal snapshots; destroyed entities
  staying destroyed (and why Begin goes before the restore); Lifecycle
  destruction; runtime entities skipped; every restore problem reported;
  capture warnings; and a snapshot saved and loaded through a slot.

### 28.3 Settings

- **Apart from saves**: what the options menu sets lives in its own file,
  `<directory>/settings.asettings`, an `aether.settings` file (a save slot
  can't be mistaken for it, and the other way round), written like a save:
  atomically, with the previous file kept as `.bak`, a CRC-32, the struct's
  version and migration hooks. `save/envelope.h` is the shared mechanics
  (`Make`, `Write`, `Read`, `ReadWithBackup`); `SaveSystem` and
  `SettingsStore` both use it.
- **`SettingsStore<T>`** works for any reflected struct (a game's own options
  struct, `GameSettings` by default): `Load` (defaults when the file is
  missing, which is normal and gives no warning; defaults plus an `error`
  (`Corrupt`, `WrongType`, `FutureVersion`) and a warning when the file, and
  then its backup, can't be used, leaving the bad file alone until the next
  `Save`; `ok` means `Get()` is usable), `Save`, `Get`, `Set` (validated; true
  if anything changed), `ResetToDefaults`, and observers `fn(now, before)`
  called only for real changes (a `Set` from inside an observer takes effect
  without notifying again). One thread (the main one) owns a store; there is
  no worker, since settings are written rarely.
- **`GameSettings`**: `quality` (a project quality preset's name, a string so
  a project can define its own presets), `width`, `height`, `fullscreen`,
  `vsync`, `master` / `music` / `sfx` / `voice` volumes (0 to 1), `language`,
  and `bindings` (`input::UserBindings`: the player's key rebinds, reused as
  they are, with their stale-override and conflict handling; they replace
  the separate `Saved/Config/Input.json`, whose one-time migration comes with
  the player wiring).
- **Validation**: `Validate(GameSettings&, warnings)` clamps volumes to 0..1
  (NaN back to 1), the window size to 320..16384, and resets an empty or
  over-long language or quality name; one warning each. The store runs it on
  load, `Set` and `Save`; a game's own struct gets its own `Validate`,
  found by ADL. A hand edit has to recompute the checksum or the file is
  treated as damaged (defaults, with the backup tried first).
- **`BusVolumeDb(linear)`**: 20 * log10(v) for the mixer's dB, with 0 (or
  anything below -80 dB) the floor of -80.
- Applying settings (the mixer's bus volumes, the quality preset, the
  window) belongs to the player and comes with its wiring, so `save/` stays
  engine-only.
- **Tests** (`test_settings.cpp`): defaults for a missing file; a round trip
  including key rebinds and the separation from saves; the backup and the
  defaults for damaged files; validation and the checksum guard; observers; a
  game's own struct with its own validation and a newer-version file; the dB
  conversion; and the backup and no stray temp file. The save tests ran
  unchanged on the refactored mechanics.

### 28.4 Blueprints and Luau: the save bag and the SaveGames library

- **The problem**: a game's save struct is a C++ type Blueprints and Luau
  can't name. So they keep their save state in a **`SaveBag`**: named values
  (bool, int, float, string, vector) plus an optional `WorldSnapshot`. It is an
  ordinary reflected struct (`SaveEntry` list, the snapshot, `has_world`), so
  C++ code can `Save("slot", bag)` and `Load("slot", bag)` and share slots with
  Blueprint saves. A bag has no schema and no migration: nothing checks that a
  key keeps its type between versions of the game, and a get of the wrong kind
  gives its default (loosely typed by design). Keys are unique; a Set replaces
  an entry of any kind; lookup is a linear scan (bags are small).
- **`SaveSystem`** owns the bag (`Bag()`), the world the capture and restore
  nodes act on (`SetWorldContext`: world, GUID index, a `WorldTracker`, a
  `Lifecycle`, all set by the host), the last error (`LastError()`), and the
  async saves that finished (`TakeFinishedSaves()`). Main thread only.
- **`SaveGames`** (a reflected static library, so every function is a
  Blueprint node under `Call.Native:SaveGames.*`): `CreateSaveObject`,
  `SetBool / SetInt / SetFloat / SetString / SetVector`, the matching pure
  `Get...(key, default)`, `HasKey`, `RemoveKey`, `SaveToSlot`,
  `SaveToSlotAsync`, `LoadFromSlot` (replaces the bag; unchanged on failure),
  `DoesSaveExist`, `DeleteSave`, `GetSlotCount`, `GetSlotName(index)`,
  `GetLastError`, `CaptureWorld` and `RestoreWorld`. All act on
  `SaveSystem::Active()`; without one they do nothing (false, the default,
  empty). A function that can fail returns false and leaves the reason in
  `GetLastError` (a success clears it). `CaptureWorld` needs a world
  context and stores the snapshot in the bag; `RestoreWorld` puts it back,
  returns false if something couldn't be restored, and lists what in the
  error.
- **Async**: `SaveToSlotAsync` copies the bag now; after the host calls
  `Pump()` it takes `TakeFinishedSaves()` and dispatches **`Event.OnSaveFinished`**
  (slot, success) to the Blueprint instances, as it does
  `Event.OnSequenceFinished`.
- **Luau**: a `SaveGames` table with the same functions
  (`scripting/save_api.h`): `SetNumber` (stored as an int when it has no
  fractional part, else a float, so Blueprint's Get Int and Get Float read
  what a script saved), `GetNumber` (reads either), `SetVector(key, x, y, z)`
  and `GetVector(key, dx, dy, dz)` returning three numbers, the others as
  their Blueprint twins. Installed by `ScriptSystem`; a missing or wrongly
  typed argument raises a script error. Only this table is hand-registered:
  a generic binding of every reflected static library is a possible later step.
- **Not here**: settings access from Blueprints and Luau, and wiring a
  `SaveSystem` into the player (PR 6): a host makes one active, sets its world
  context, pumps it and dispatches the event.
- **Tests** (`test_save_games.cpp`): the bag's set, get, replace and remove; a
  bag saved and loaded by C++ with its world; the library's save, load, exists,
  delete, list and errors; no active system; capture and restore through a
  slot (and a vanished entity reported); async finishing through the host; the
  reflected function list; a real Blueprint graph that saves, clears and loads
  back, and handles `Event.OnSaveFinished`; and the Luau table including
  errors.

### 28.5 The Save Inspector

`Tools > Data > Save Inspector` shows the save and settings files in a folder
(`editor/src/save/`). It is **view-only** on purpose: editing a save means
recomputing its checksum, running migrations and silently changing a player's
progress, so the actions are the ones that don't alter contents: **Reload**,
**Delete** (with the `.bak`, after a confirmation) and **Copy as...** (a valid
slot name that isn't taken; a damaged file is copied raw, for a bug report).

- `save::envelope::Inspect(file)` reads an envelope without loading it:
  kind, format, type and version, slot, time, size, the stored and computed
  checksum, the data, and the raw text. Files over 64 MB are refused.
- `SaveInspectorDocument` adds what the file doesn't say about itself: a
  backup beside it, and problems (bad checksum, unknown type or format, a
  newer version than the code has, what loading would skip, data that doesn't
  fit the type). When the type is registered in the build it makes a
  temporary instance, so the generic Inspector draws it (read-only); else it
  shows a JSON tree capped at `kMaxTreeNodes` (5000), plus a Raw JSON tab.
- The list marks unreadable files and bad checksums in red. The editor's
  Content Browser opens `.asav` and `.asettings` here (extension asset types).
- The workspace writes sample saves under the temp folder (a bag with a
  world, a settings file, a tampered copy) so the tool is never empty.

Tests (`test_save_inspector.cpp`): a bag opens with its backup and a reflected
instance; tampering, junk, empty and missing files; an unknown type falling
back to JSON; the node cap; delete and copy rules; the panel drawing headless
and rescanning; and the workspace tool and extension opener.

### 28.6 Player wiring

`Game` (the player runtime) gets saving (`player/`):

- `ResolveUserPaths(project, override)` gives `{root, Saves, Config}`: the
  `--user-dir` option, then `AETHER_USER_DIR`, then the OS's per-user data
  folder (`%APPDATA%`, `~/Library/Application Support`, `$XDG_DATA_HOME` or
  `~/.local/share`) plus the project name with anything but letters, digits,
  `-` and `_` replaced by `_` (a project can't name its way out of the folder).
- `Game::SetUserPaths` opens a `SaveSystem` over `Saves` (made the active one,
  so `SaveGames` in Blueprints and Luau works) and a `SettingsStore` over
  `Config`. Without it there is no save system (`Saves()` is null).
- The save system's world context (world, GUID index, lifecycle and a
  `WorldTracker`) follows the loaded scene: cleared before the old world
  goes, set after the new one is built. `~Game` drains and destroys the save
  system before the world.
- `Player.Save` (Update, after `Player.Sequencer`, before `Player.Scripting`)
  pumps the system, then sends `Event.OnSaveFinished(slot, success)` to every
  Blueprint with it. That is the new `BlueprintVM::DispatchAll(event, args)`,
  a broadcast returning how many ran it. Luau callbacks run from the same pump.
- `ApplySettings(settings, targets, before)` pushes volumes (through
  `BusVolumeDb`, buses Master, Music, SFX, Voice), the quality name and the
  window to host hooks, only what changed when given `before`. The hooks are
  injected so the runtime doesn't link the mixer, the renderer or the window.
  `Game::LoadSettings(targets)` loads the file, applies it, and keeps applying
  later `Settings().Set(...)` changes.

Tests (`test_player_save.cpp`): path precedence and sanitizing; no paths, no
save system; the context following a scene reload and the active system
cleared at teardown; an async save finishing on a tick; `DispatchAll`; what
`ApplySettings` pushes; and `LoadSettings` applying and observing.

### 28.7 The player executable

`aether_player` (`player/main.cpp`) uses all of it:

- `--user-dir <dir>` (else `AETHER_USER_DIR`, else the OS folder, §28.6) is
  where `Saves/` and `Config/` live. `Game::SetUserPaths` and `LoadSettings`
  run before the scene loads.
- **Precedence** for the window size and the quality preset: the command line
  (`--size`, `--quality`), then the settings file, then the project's own.
  The settings count only if a `settings.asettings` exists: the defaults
  (1280x720, High) never override the project. Vsync follows the same rule.
  Fullscreen is read but only logged: `Window` can't change mode yet.
- **Written on exit only if changed.** The settings are saved after the run
  if the game changed them (`Settings().Set`), never because they were loaded,
  so a damaged file isn't overwritten by a run that didn't touch it.
  Command-line overrides are not copied into the settings.
- **Key bindings** are part of `GameSettings`. `MigrateLegacyBindings` moves
  an old `Config/Input.json` (the pre-Phase 28 place) into them once: no file,
  nothing; a damaged file, a warning and the file stays; rebinds already in
  the settings win and the old file is set aside; otherwise they are copied in
  and the file becomes `Input.json.migrated`. `Game` applies the settings'
  rebinds to every input context, and re-applies them when they change
  (live rebinding).

Tests (`test_player_save.cpp`): the migration cases above, and a run of the
real `aether_player --headless --user-dir` that writes no settings file when
nothing changed, moves the old rebinds in once, and leaves the file alone on
the next run.

Not done (follow-ups): applying the volumes (the player has no mixer or audio
system yet), fullscreen and live window resize (`Window` has no setters), a
quality preset change at runtime, and cloud saves as plugins.

### 28.8 PR breakdown

1. Save slots (done, §28.1).
2. World state (done, §28.2).
3. Settings (done, §28.3).
4. The save bag and the Blueprint and Luau library (done, §28.4).
5. The Save Inspector panel for `.asav` (done, §28.5).
6. Player runtime wiring: user folders, the save system and its world
   context, `Player.Save`, `DispatchAll`, `ApplySettings` (done, §28.6).
7. The player executable (`--user-dir`, saved window size and quality,
   settings on exit, the `Input.json` migration) and the Phase 28 wrap-up
   (this step, §28.7).

---

## Phase 29: localization and text

### 29.1 The core (`loc/`, `aether::loc`)

A small, dependency-free library (`Aether::Loc`, engine-only, headless):

- **`LocText { key, source }`**, a reflected struct (so it saves, cooks and
  shows in the Inspector like any field). `key` indexes the string tables;
  `source` is the text as written, shown when no table has the key. An empty
  key makes a *literal* (`LocText::Literal`), shown as it is and never looked up.
- **`NormalizeLanguage` / `FallbackChain`**: `pt_br` and `PT-br` are `pt-BR`;
  the chain is the tag, then each shorter prefix, then the default language
  (`pt-BR` gives `{pt-BR, pt, en}`), without repeats.
- **`StringTable`**: translations by key and language, with **CSV** import and
  export. Columns are `key,en,pt-BR,...`; a `comment` column is ignored;
  cells follow RFC 4180 (commas, quotes and line breaks inside quotes); a BOM
  and CRLF are fine; an empty cell is no translation. Export sorts keys and
  languages so diffs stay small. Import merges, reports a missing header, an
  unclosed quote, a row with no key and a repeated key (the later row wins),
  and keeps going where it can.
- **`Format(pattern, language, args)`**: `{name}` substitution; plurals with
  `{count|s}` (the suffix unless the category is *one*, so
  `{count} coin{count|s}`) or explicit forms,
  `{count|one:coin;other:coins}` (falling back to `other`); `{{` and `}}` are
  literal braces. A problem leaves that part as written and adds a line to
  the error list; it never throws.
- **`PluralCategory(language, n)`**: the CLDR rules, written by hand for en
  (and the languages that follow it), pt (pt-BR: 0 and 1 are *one*; pt-PT: 1
  only), fr, ja/zh/ko/vi/th/id/ms (no plurals), ru/uk, pl and ar. Other
  languages follow English. ICU is not used.
- **`Localizer`**: resolves a `LocText`: a table entry along the fallback
  chain, else `source`, else the key, so a string is never blank; with
  arguments the result goes through `Format`. It isn't a global yet.

Tests (`test_loc.cpp`): tags and chains; table set, find, remove and counts;
CSV round trips of hard cells, BOM/CRLF/comment columns, and every reported
problem; the plural rules; formatting and escapes, including malformed
patterns; the localizer's resolution order; and `LocText` as a reflected field.

### 29.2 Assets, the runtime service and the language switch

- **`.astrings`** is a cooked asset type, importer `StringTable`: plain CSV in
  the format of §29.1 (`key,en,pt-BR,...`), so translators and spreadsheets
  open it as it is. Nothing references a string table, so the cooker adds
  every `StringTable` asset to the cook set on its own (reason
  "localization"); a table can't be forgotten and silently lose all text.
- **`loc::Localization`** is the running game's text: every table merged
  (a later file's entry replaces an earlier one's for the same key and
  language), the current language, and listeners told when it changes (only on
  a real change). `Text(key, fallback, args)` follows §29.1's order (the
  chain, then the source text, then the key). One can be made the active
  one, like `SaveSystem`.
- **In the player**, `Game::Localization()` is always there and active for
  the game's life. The first scene load reads every `StringTable` asset in the
  package (a bad table is a warning, the rest still load; the warnings stay
  across scene loads). The language and `GameSettings::language` follow each
  other: a settings change switches the language, and a language chosen in play
  (`Localization.SetLanguage`) is written into the settings, so it is saved on
  exit like any other change. `LoadSettings` applies the loaded language.
- **Blueprints and Luau**: the reflected `Localize` library
  (`Call.Native:Localize.GetText`, `FormatInt`, `FormatString`, `HasText`,
  `SetLanguage`, `GetLanguage`) and Luau's `Localization` table
  (`GetText(key, default)`, `Format(key, default, name, value)` with a number or
  a string, `HasText`, `SetLanguage`, `GetLanguage`). With no active
  Localization they return the default (or the key), so text never blanks.

Tests (`test_loc_runtime.cpp`): `.astrings` cooked without `always_cook` and
read by the player; merging and a bad table's warning; the language following
the settings both ways, with a listener firing once per real change; a saved
language applied on load; the library with and without a service, with
plural rules per language; its reflected functions; and the Luau table.

UI and Blueprint text are §29.3.

### 29.3 Runtime UI and Blueprint text

Widget text is localized with a **key beside the text**, not by changing the
field's type: `Text::text_key` and `TextInput::hint_key`, with `text` / `hint`
as the source text. This keeps every existing layout, binding, script and
test working, and no call site had to change.

- `Text::Shown()` (and `TextInput::ShownHint()`) is what is measured and
  drawn: with a key and an active `Localization`, the key's text in the current
  language (the fallback chain of §29.1, then the source text); otherwise the
  text as it is. Layout and drawing ask each frame, so a language change shows
  at the next layout and paint with nothing to subscribe to.
- Layout files write `text_key` / `hint_key` only when set; a file without
  them loads as before. The UI Designer lists them as properties ("Text key",
  "Hint key") and its canvas preview shows the translated text.
- Runtime text wins over a key: `UI.SetText` and a binding to `text` clear
  `text_key`. `UI.SetTextKey(target, widget, key, default)` sets one (on a
  Text, or on a button's first Text), so Blueprints localize a widget without
  a new pin type. `Localize.GetText` into `UI.SetText` also works.
- `aether_ui` now links `Aether::Loc` (no cycle: `loc` needs only the engine).

Tests (`test_ui_loc.cpp`, and the UI library test): the shown text with and
without a service, a key and a translation; layout width changing with the
language; JSON round trips, old files, and keys written only when set; and
`SetTextKey` / `SetText`.

Not done (later, as needed): a Blueprint `Text` pin type (a `Make LocText`
node), localizing a binding's `format` string, tooltip and dropdown option keys,
and a `LocText` JSON converter for scene and component fields (the gather step
needs it).

### 29.4 Gather, merge and .po files

The gather step is `aether_loc` (tool in `tools/loc/`; the work is in
`loc/` so it is tested headless):

- **Gather** (`loc::GatherContent`) reads the JSON of every `.ascene`,
  `.aprefab`, `.aui` and `.abp` under `Content/` and finds: a `text_key` /
  `hint_key` (source: the sibling `text` / `hint`); a reflected `LocText` (an
  object with a non-empty string `key` and `source` and nothing else but `$v`);
  and a Blueprint `Localize.GetText` / `FormatInt` / `FormatString` or
  `UI.SetTextKey` node whose `key` pin has a literal default (source: its
  `default`). A key that comes from a linked pin can't be known and is a
  warning. Keys are merged: the same key with two different texts is a
  warning (the first by path is kept). It is a JSON walk, not the reflection
  registry, so it needs no `LocText` converter; the `LocText` shape is the
  heuristic.
- **Merge** (`loc::MergeKeys`): a new key gets its text in the source language;
  a changed source text is updated; **no translation is touched and no key is
  removed**: keys nothing uses are only listed.
- **PO** (`loc::ExportPo` / `ImportPo` / `PoLanguage`): one file per language,
  `msgctxt` = the key, `msgid` = the source text, `msgstr` = the translation,
  with a UTF-8 header and `Language:`. Import skips empty and `fuzzy`
  entries (fuzzy with a warning) and obsolete ones, accepts the split-string
  form and a `.po` with no `msgctxt` (the msgid is the key), and reports a
  bad line by number. Plural forms are not used: write plurals inside the string (§29.1).
- **`aether_loc <project.aproject>`**: `--strings` (default
  `Localization/strings.astrings` under Content), `--source-language en`,
  `--languages fr,de`, `--po-out <dir>`, `--po-in <dir|file.po>`, `--check`
  (write nothing). It applies returned `.po` files, merges, writes the CSV back
  and the `.po` files out, and prints the counts and the unused keys. Files are
  deterministic, so a run with nothing new rewrites identical ones.

Tests (`test_loc_tools.cpp`): gathering from each kind of content, merging
and conflicts; the merge rules; `.po` export, round trip with escapes, and
every import rule and error; and the whole sync (new strings, a returned
translation, the second run changing nothing, `--check`, a damaged table).

### 29.5 The Localization dashboard

`Tools > Data > Localization` (`editor/src/loc/`) opens a `.astrings`:

- **Languages**: each language, how many keys it has translated out of all,
  and a bar; the source language is marked. A language can be added (a column
  with no cells yet; `StringTable::AddLanguage`, and the CSV reader now keeps
  header-only columns, so an empty column survives a save).
- **What's missing**: for the selected language, its untranslated keys beside
  the source text, each with a box (Enter sets the cell; an empty text clears
  it). Add and remove keys. At most 200 are drawn at a time.
- **Files**: Save (atomic CSV, dirty marker), Reload, **Export .po** for a
  language and import of a returned one (the language comes from its header).
- **Gather from project**: runs §29.4's `SyncProject` on the content folder,
  **Check only** on by default (nothing written); a real gather needs the table
  saved first and reloads it after. It reports found, new, changed and unused
  keys and the warnings.
- **Pseudo-localization**: `loc::Pseudo` accents the letters (Latin-1, which
  the default font has), pads about 35% (`~`), wraps in `[ ]`, and copies
  `{placeholders}`, `{{`/`}}` and line breaks as they are, so a pseudo string
  still formats. `PseudoTable` adds the `qps-Ploc` language. The panel's
  **preview** checkbox makes a Localization holding that table the active one,
  so every keyed text (the UI Designer's preview and a running UI) shows pseudo
  text; it is rebuilt when the table changes and the previous active one is put
  back when switched off.
- A double-click on a `.astrings` in the Content Browser opens it here (an
  extension asset type). The workspace opens a sample table (English, French,
  German with gaps) and a small content folder with UI text to gather.

Not done: undo (edits are explicit and saved with Save), stale-key diffs,
plural forms, and a per-viewport language.

Tests (`test_loc_editor.cpp`): pseudo-localization (accents, length,
placeholders, deterministic, still formats); stats, missing keys and every edit;
save and reload including an empty language; `.po` round trip and gather
(check, dirty, real); the panel drawing and the preview taking over and giving
back the active Localization; and the workspace tool.

### 29.6 Font fallback

One font rarely has every script, so text can use a **chain** (`ui/`):

- `FallbackFont` holds fonts in order and takes each code point from the
  first one that has a glyph for it (`Font::HasGlyph`, new; true for fonts that
  draw anything, like `BuiltinFont`). A code point no font has draws as the
  first font's replacement glyph. Lines use the first font's ascent and line
  height (a fallback glyph is drawn at the same size on that baseline); kerning
  applies only between two glyphs from the same font.
- Each glyph is drawn with **its own font's** atlas texture and distance-field
  range (`Font::Source(codepoint)`), so one line can mix a bitmap font and
  an SDF one, several textures in one text run.
- `FontLibrary::Find("Roboto, NotoSansCJK, NotoEmoji")`: a comma-separated name
  is a chain of the named fonts (unknown names skipped, none known gives
  null so the default font is used). It works anywhere a font name does
  (`Text::font` in layout files, text styles). Chains are cached until a font
  is added or removed.
- `DecodeUtf8` now rejects overlong forms, surrogates and values past U+10FFFF
  (U+FFFD, the whole bad sequence consumed), so malformed text can't smuggle
  characters past a filter.
- No CJK or emoji font ships (a licensing decision), so the tests chain the
  shipped Roboto with test fonts and the built-in boxes.

Tests (`test_ui_fallback.cpp`): the first font with the glyph wins, metrics and
kerning, an empty chain; layout and drawing across fonts with their own
textures and SDF ranges; chains from library names, caching and rebuilding, and a
Text widget using one; Roboto then the boxes for a CJK character; and strict UTF-8.

Not done: a CJK line-break table (text still wraps at spaces and line breaks),
emoji sequences (ZWJ, variation selectors), colour emoji (needs RGBA atlases),
and per-run baseline alignment between fonts with different metrics.

#### Localized assets

A language's variant of an asset is the same path with the language before the
extension: `Textures/logo.png` has `Textures/logo.fr.png` and
`Textures/logo.pt-BR.png`. Variants are ordinary assets (they cook and ship like
any other), so nothing in the cooker or the manifest changed; only the lookup
knows the naming (`loc/localized_path.h`).

- `LocalizedCandidates(path, language)` is the list to try: the fallback chain of
  §29.1, most specific first (`logo.pt-BR.png`, `logo.pt.png`), then the base
  path, which is the default language's (a variant for the default language
  itself isn't tried).
- `GamePackage::LocalizedPath` is the first candidate that is a cooked asset (else
  the path unchanged); `ReadContentLocalized` reads it. `Game::LocalizedAsset(path)`
  uses the game's current language, so a script or system that asks again after a
  language change gets the new variant.
- `SplitVariant` / `OrphanedVariants` recognize a variant by its name (a 2 or 3
  letter lower-case language, optionally with subtags: `icon.large.png` and
  `v1.2.png` aren't variants). The player warns about a variant with no base asset
  (a typo in the language, or nothing to fall back to).

Tests (`test_loc_runtime.cpp`): the candidate lists, variant recognition and
orphans, and a cooked package resolving fr, a pt-BR falling back to pt, a language
with no variant falling back to the base, and the game following its language.

Not done: reloading what is already loaded when the language changes (the
`Localization` change listener is the hook), and per-asset-type policy.

#### Right-to-left

Enough for Hebrew and layout mirroring; **Arabic joining needs shaping and is not
done** (Arabic letters draw as separate, isolated forms, which reads wrong until
step 7 adds HarfBuzz).

- **Text order** (`ui/bidi.h`): `ReorderVisual` puts a line's code points in the order
  they are drawn, following the shape of UAX #9 without embeddings or isolates.
  The base direction is the first strong character's; Hebrew and Arabic letters
  run right to left, other letters and digits left to right (a number inside Hebrew
  keeps its order, sitting one level up); spaces and punctuation take the direction
  around them, else the base; runs are reversed from the deepest level up; brackets
  in a right-to-left run are mirrored. Text with no right-to-left character is
  untouched, so left-to-right text costs only a scan. `LayoutText` wraps in logical
  order and then orders each line, so measuring and drawing agree.
- **Layout mirroring**: `Viewport::direction` (`FlowDirection`). With
  `RightToLeft` the layers are laid out as usual and then flipped about the safe
  area's centre (`Widget::MirrorX`), so a box runs right to left, `Left`-aligned
  children sit on the right, and padding and margins mirror, with no change to any
  panel. Sizes are unchanged; hit testing uses the mirrored rectangles.
- **Start / End**: `TextAlign::Start` and `End` (layout files: "Start", "End") are
  the side a line starts and ends on in the reading direction; a Text resolves
  them against the viewport's direction. (`Left`, `Center`, `Right` are as before.)
- `loc::IsRtl(language)` says whether a language is written right to left (ar,
  he, fa, ur, ... or an Arabic/Hebrew script subtag), for setting the direction
  when the language changes.

Tests (`test_ui_fallback.cpp`): Hebrew reversal, numbers and Latin keeping
their order, a left-to-right base staying so, brackets mirrored; layout width and
draw order following the visual order; a viewport mirrored about its centre with
Start/End resolved; and `IsRtl`.

Not done: Arabic shaping, caret and selection in mixed-direction text (text
boxes still move by logical position), embeddings and isolates, and mirrored icons.

### 29.7 PR breakdown

1. The core (done, §29.1).
2. `.astrings` as a cooked asset type; a `Localization` service with a runtime
   language switch and a change event, wired to `GameSettings::language`
   (done, §29.2).
3. Runtime UI text takes a key beside its text; Blueprints set it with
   `UI.SetTextKey` (done, §29.3).
4. The gather step (every localizable string in scenes, prefabs, Blueprints
   and widgets), PO import and export, and a command-line tool (done,
   §29.4).
5. The editor's Localization dashboard (languages, completion, missing keys,
   pseudo-localization) (done, §29.5).
6. Font fallback (Latin, then CJK, then emoji) (done, §29.6); localized assets
   (a language's variant of an asset by file name) (done, §29.6); right-to-left layout (a
   run-reversing BiDi-lite for Hebrew, Start/End alignment, mirrored
   layout; Arabic joining needs shaping and waits for step 7) (done, §29.6).
7. Optional: HarfBuzz shaping behind a CMake option, and gender selectors.

---

## Phase 30: the gameplay ability system and RPG toolkit

### 30.1 Gameplay tags (`gameplay/`, `aether::gas`)

The foundation everything else in the phase uses: stats, effects and abilities
block, require and cancel each other with tags. A small, engine-only, headless
library (`Aether::Gameplay`).

- **`GameplayTag`**: a dotted name like `State.Stunned` or `Damage.Fire.Burning`;
  segments are non-empty and made of letters, digits and `_`. An invalid name is
  the empty tag, which matches nothing. A tag *matches* itself and every tag
  below it (`Damage.Fire.Burning` matches `Damage.Fire` and `Damage`; a parent
  doesn't match its child), and the match is on segment boundaries, so
  `State.Stunned` does not match `State.Stun` and `StateX` is not under `State`.
  `Parent()`, `Depth()`, `MatchesExact`.
- **`TagContainer`**: the tags something has now, each with a **reference count**
  (two effects that both grant `State.Stunned` keep an entity stunned until both
  are removed), kept sorted so it saves and compares deterministically. `Add`,
  `Remove` (the tag goes when its count reaches zero), `RemoveAll`, hierarchical
  `HasTag`, `HasTagExact`, `Count`, and `HasAny` / `HasAll` / `HasNone` over a
  list. It is a reflected struct and an ordinary component (an entity's tags).
- **`TagQuery`**: a condition tree for effects and abilities: `Any`, `All`,
  `None` over tags, and `And` / `Or` over child queries; saved as JSON
  (`{"op":"and","children":[{"op":"all","tags":["State.Alive"]},
  {"op":"none","tags":["State.Stunned"]}]}`); a bad op, tag name or shape is an
  error naming it, and nesting is limited to 32 levels.
- **Blueprint library** `GameplayTags` (`Call.Native:GameplayTags.Matches`,
  `MatchesExact`, `IsValid`, `GetParent`, `GetDepth`): tags are plain strings
  there. Tags on an entity, and Luau, come with the attribute and ability
  systems.

There is no tag registry yet: tags are free-form strings. A validated registry
(an editor-managed `.atags` list, with completion) can be added later if wanted.
Tag names are `std::string`s for now; interning them is an optimisation for when
profiling asks.

Tests (`test_gameplay_tags.cpp`): validation, parent and depth, hierarchical
matching and the prefix traps, reference counting, sorted queries, `TagQuery`
nesting, JSON round trips and every error, reflection and use as a component,
and the library as Blueprint nodes in a real graph.

### 30.2 Attributes

`AttributeSet` (a component) holds an entity's stats: Health, Mana, Stamina,
AttackPower... Each `Attribute` has a `base` (what the game sets and saves),
a `current` (what the rest of the game reads) and `min` / `max` bounds. Today
`current` is the clamped `base`; `AttributeSet::RecomputeCurrent` is the one
place step 3 applies effect modifiers, so nothing else changes then.

- The set is sorted by name, so it saves and compares deterministically, and it
  reflects like any component (`current` is saved; `Normalize` repairs
  hand-edited data: bounds in order, NaN removed, base and current re-derived).
- `Define(name, base, min, max)` creates or redefines (an upside-down range is
  the range the other way round); `SetBase` / `AddBase` clamp the **base** too, so
  a capped Health doesn't hide later damage. A NaN or an unknown attribute is
  refused and changes nothing.
- **`AttributeSystem`** (like the sequencer's: owns the world, can be the active
  one) is where changes go through, and queues an `AttributeEvent` (entity, name,
  old, new) **once per real change** to `current`, never for a write that leaves it
  as it was; a new attribute appears as a change from 0, and narrowing the bounds
  re-clamps and says so. The host drains the queue.
- **Blueprints**: the `Attributes` library (`GetAttribute`, `GetAttributeBase`,
  `GetAttributeMin`, `GetAttributeMax`, `HasAttribute`, `DefineAttribute`,
  `SetAttributeBase`, `AddAttributeBase`; entity-targeted, acting on the active system, defaults
  and no effect without one) and **`Event.OnAttributeChanged`** (name, old, new),
  declared generically in the Blueprint module, so Blueprint doesn't depend on gameplay.
- **The player** links `Aether::Gameplay`, registers the gameplay components,
  gives each loaded scene its own `AttributeSystem`, and its `Player.Attributes`
  stage (after `Player.Update` and `Player.Sequencer`, before `Player.Scripting`)
  sends each queued change to that entity's Blueprint. The queue is taken first, so a
  handler that changes an attribute is heard next frame, not in a loop.

Tests (`test_gameplay_attributes.cpp`): define, sorted order and bad input; clamping,
the capped-base case, NaN and repair; events once per real change, redefining,
refused writes, dead and attribute-less entities; reflection and a save round trip;
the library with and without a system and as nodes in a graph; and the player draining
the queue each frame and giving a new scene a fresh system.

Not done: Luau (with abilities, step 5), modifiers and effects (step 3), and
Blueprint access to an attribute set as a whole.

### 30.3 Gameplay effects (`gameplay/`, step 3a)

`GameplayEffect` (`.aeffect`, JSON, `EffectFromJson` / `EffectToJson` with named errors
`effect.name_empty`, `bad_duration`, `bad_modifier`, `bad_op`, `bad_period`,
`period_on_instant`, `bad_stack`, `bad_tag`, `bad_json`) has a duration policy
(instant, timed, infinite), modifiers (`add`, `multiply`, `override` on an attribute),
stacking (none, refresh, stack with `max_stacks`, optionally `per_source`), an optional
period with `execute_on_apply`, `require` / `blocked` tag queries on the target, tags it
grants while active, and tags that remove it. An `EffectLibrary` finds definitions by name.

`EffectSystem` (with the `AttributeSystem` and a library) applies them. Instant effects
change the base through the attribute system, so events fire. Timed and infinite effects
keep modifiers on `current` only: `current = clamp(latest override, or (base + sum add) *
product multiply)`, stacks scaling add linearly and multiply as a power. Periodic effects
change the base each period instead (poison of -5 per second for 3 s is -15; with
`execute_on_apply` it is -20). `Apply` is rejected for an unknown effect, a dead target, a
modifier on an attribute the target lacks, unmet tags, or an already-applied
`none`-stacking effect. Granted tags are reference-counted in the target's `TagContainer`.
Active effects live in a saved `EffectContainer` component; `Rebuild()` re-derives modifiers
after a load (the modifier fields on `Attribute` are not saved). `Update(dt)` is
deterministic (entities, then effects, in order).

Step 3b wires it in. `.aeffect` is an asset type (importer `GameplayEffect`) and the cooker
roots every one, since effects are applied by name and nothing references them. The player
loads them into its `EffectLibrary` once (a broken file is a warning naming the asset and the
error code), gives each scene an `EffectSystem`, and a `Player.Effects` stage ticks it before
`Player.Attributes`, sending `Event.OnEffectApplied` and `Event.OnEffectRemoved` (effect
name, handle) to the target's Blueprint. The `Effects` Blueprint library has `ApplyEffect`
(returns the handle, 0 for an instant or rejected effect), `RemoveEffect`,
`RemoveEffectsByTag`, `HasActiveEffect` and `GetActiveEffectCount`. Deferred: attribute-based conditions, magnitude curves, cues, effects that
change min / max, replication.

### 30.4 Abilities (`gameplay/`, step 4a)

`GameplayAbility` (`.aability`, JSON, `AbilityFromJson` / `AbilityToJson`, errors
`ability.name_empty`, `bad_json`, `bad_duration`, `bad_tag`) names the tags it has,
`activation_required` / `activation_blocked` queries on the owner,
`cancel_abilities_with_tags` and `block_abilities_with_tags` queries over other abilities'
tags, `activation_owned_tags` (on the owner while it runs), a `cost` (an instant effect), a
`cooldown` (a timed effect that grants a tag), a `max_duration`, and `commit_on_activate`.
`AbilityLibrary::CheckEffects` reports `ability.unknown_effect`, `cost_not_instant` and
`cooldown_not_timed` against an `EffectLibrary` (the library can't, at parse time).

`AbilitySystem` keeps a saved `AbilityContainer` (granted names and running instances).
`CanActivate` returns a `FailReason` (unknown, not granted, dead owner, already active,
missing required, blocked, ability-blocked, cooldown, cannot afford; the cost is a dry run).
`TryActivate` cancels what it cancels, adds the owned tags, commits and queues an Activated
event; `Commit` applies the cost and cooldown through the `EffectSystem` (once; callable
later when `commit_on_activate` is false); `End`, `Cancel`, `CancelByTag` and `Revoke`
release exactly what activation added. Blocking is evaluated against the running abilities
at activation, so nothing extra is stored. Owned tags are reference-counted with
effect-granted ones. `Update(dt)` ends an ability at its `max_duration` (event marked
`timed_out`). Events: Activated, Ended, Cancelled, Failed (with the reason).

Step 4b wires it in. `.aability` is an asset type (importer `GameplayAbility`) the cooker roots
automatically. The player loads abilities after effects (an ability naming a missing effect,
a non-instant cost or a cooldown that is not a timed tag-granting effect is a warning naming
the asset), gives each scene an `AbilitySystem`, and a `Player.Abilities` stage (after
`Player.Effects`) ticks it and sends the owner's Blueprint `Event.OnAbilityActivated`
(ability, handle), `Event.OnAbilityEnded` (ability, handle, cancelled) and
`Event.OnAbilityFailed` (ability, reason name). An ability's logic is the owner's graph: it
hears Activated, does its work (the latent nodes included) and calls `EndAbility`. The
`Abilities` Blueprint library has `GrantAbility`, `RevokeAbility`, `TryActivateAbility`
(the handle, 0 on failure), `CanActivateAbility`, `CommitAbility`, `EndAbility`,
`CancelAbility`, `IsAbilityActive` and `IsAbilityGranted`. The gameplay Blueprint libraries
(tags, attributes, effects, abilities) now register wherever the gameplay systems are linked.
Deferred: latent tasks (wait for delay,
attribute change or gameplay event; a Blueprint graph can already use its latent nodes),
a dedicated ability graph asset (an ability's logic runs in the owner's script graph),
replication.

### 30.5 Luau face and manual (step 5)

Luau gets `GameplayTags`, `Attributes`, `Effects` and `Abilities` tables
(`scripting/src/gameplay_api.cpp`, `InstallGameplayApi`), one for one with the Blueprint
libraries and acting on the same active systems; wrong-typed arguments raise script errors
and a missing system makes them inert. `NativeCall` gained `IsEntity` and `EntityArg`. The
player's `Player.Attributes`, `Player.Effects` and `Player.Abilities` stages also call the
entity's script method when it defines one: `OnAttributeChanged (name, old, new)`,
`OnEffectApplied` / `OnEffectRemoved (effect, handle)`, `OnAbilityActivated (ability, handle)`,
`OnAbilityEnded (ability, handle, cancelled)`, `OnAbilityFailed (ability, reason)`. The sample
is the manual's chapter `docs/manual/09-gameplay.md` (a poison effect, a fireball with cost and
cooldown, and a script). Deferred: Event-object subscriptions (`Attributes.OnChanged`),
Luau access to an entity's tag container.

### 30.6 The Gameplay Debugger (`editor/src/gameplay/`, step 6)

`GameplayDebuggerDocument` (no ImGui) builds the rows for the entities of a world it is pointed at
(`SetWorld`): for each entity with gameplay data, its attributes (base, current, bounds and the
add / multiply / override modifiers on them), active effects (kind, time left, stacks), granted
abilities (ready or running, elapsed, committed) and tags with counts. Order is deterministic
(entity index, then names, effects by handle). A case-insensitive filter matches entity labels,
attributes, effects, abilities and tags; `Select` narrows to one entity; at most 500 entities.
The actions return a message: set a base, remove an effect, cancel an ability, add and remove a
tag. They go through the gameplay systems the host gives it (`SetSystems`, so events fire and
modifiers follow) and edit the components directly when there are none. `GameplayDebuggerPanel`
draws it with tables and per-row buttons, and says so when there is no world. The workspace shows
it as **Gameplay Debugger** (Debug) on a built-in sample hero; `SetGameplayWorld` points it at the
world being played (or back at the sample). Deferred: live graphs and history, an event log,
editing effect and ability definitions, network views.

### 30.7 PR breakdown

1. Gameplay tags (done, §30.1).
2. `AttributeSet`: base and current values with clamps, change events
   (`Event.OnAttributeChanged`), save and Blueprint access (done, §30.2).
3. Gameplay Effects (`.aeffect`): 3a data, modifier math, stacking, periodic ticks, tags
   and the system (done, §30.3); 3b assets, the player stage and the Blueprint library
   (done, §30.3).
4. Abilities (`.aability`): cost, cooldown, required / blocked / cancel tags,
   activate, commit, end. 4a data and system, 4b assets, the player stage and the Blueprint
   library (done, §30.4); the latent tasks are deferred.
5. Luau face, script events, and the manual chapter with a sample (done, §30.5); the
   Blueprint face, player systems and cook roots came with steps 2-4.
6. The editor's attribute and effect debugger (done, §30.6).
7. Genre kits as optional plugins: inventory and items, the dialogue graph,
   quests, interaction.
8. Networking (prediction keys) waits for the networking integration; tag counts
   are the replication unit.
