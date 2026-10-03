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
