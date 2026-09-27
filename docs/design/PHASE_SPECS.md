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
   - **Not yet**: the impulse Jolt computes during solving isn't
     reported (approach speed stands in, as Jolt suggests for impact
     sounds). Events go to Blueprints through a handler the game sets up
     (the test shows one line); the player loop wires it with the other
     hookups.
4. Queries with Blueprint nodes.
5. `CharacterMovement` with tests for slopes, steps, and jump buffering.
6. Collider gizmos and the physics debug draw toggle.
