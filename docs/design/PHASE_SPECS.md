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
2. Override recording, bold UI, Apply/Revert.
3. Nesting, variants, cycle detection.
4. Propagation on prefab save and orphaned override warnings.
5. Gameplay framework components (Camera, Tags, Layers) and lifecycle
   events.
6. `SystemDesc` scheduler and fixed timestep with interpolation.

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

1. Fetch Luau (MIT) via `FetchContent`, a `LuauHost` that runs a script
   file, tests.
2. Reflection-driven bindings for fields and functions, with the typed
   fast paths and handle safety.
3. `ScriptComponent`, lifecycle, exposed variables in the Inspector.
4. Events (`:Connect`) and Input/Physics/Timer APIs.
5. Hot reload and the error overlay.
6. Code editor panel with completion from reflection; DAP debugger.

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

1. Collider components (box, sphere, capsule, convex, mesh) and the
   `RigidBody` split, with the scene migration.
2. Layers and the collision matrix.
3. Contact listener queue and event dispatch, with a determinism test (same
   scene, same events in the same order across 10 runs).
4. Queries with Blueprint nodes.
5. `CharacterMovement` with tests for slopes, steps, and jump buffering.
6. Collider gizmos and the physics debug draw toggle.
