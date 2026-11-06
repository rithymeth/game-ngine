# Aether Engine — unreleased changes since v0.27.6

A working draft of the next release's notes. It is rewritten into `vX.Y.Z.md` when a version is tagged; it
describes what is on `master`, not a promise of a version number or a date.

- Added an offline documentation site generated from the manual and reflected engine API (`aether_docgen --site ...` or the CMake `aether_docs_site` target).

## Vulkan regression checks

- Linux Vulkan CI now requires a working device and display on its graphics lanes, and the GCC lane checks
  Khronos validation diagnostics. Added repeated resize/readback and texture-handle reuse regressions, a
  compute descriptor-pool rollover/reset regression, and corrected the textured-quad test's SPIR-V push-constant
  padding. Optional local runs and the sanitizer lane can still skip unavailable Vulkan devices.

## API compatibility

- The 1.0 preparation freezes the public names and normalized function-like declaration signatures in selected
  stable headers. CI checks those baselines. Other public APIs remain experimental; this is not a binary ABI or
  semantic compatibility guarantee. See [API stability](../manual/api_stability.md).

## Samples

- Added Beacon Run, a third playable sample. Its fixed-step ECS simulation is portable; the Windows sample uses a
  small Win32/GDI renderer. Collect eight beacons while avoiding patrol drones.

## Release artifacts

- The release workflow checks Windows editor and AETHER-01 archives against `SHA256SUMS.txt` and attaches a Sigstore
  build-provenance attestation. When both `AETHER_WINDOWS_SIGNING_PFX_BASE64` and
  `AETHER_WINDOWS_SIGNING_PFX_PASSWORD` are configured, the release job Authenticode-signs the packaged EXEs and DLLs
  and verifies those signatures. Without the certificate secrets, it publishes unsigned archives and records
  `signed: false` in `RELEASE_MANIFEST.json`.

## Agent and MCP tools

- The MCP server has tools for UI layouts, themes and animations, saves and settings, world partitions,
  materials, prefabs, projects and templates, and for a running game (log, UI and entities). A new sample game,
  Stardust Hop, was built entirely through them.
- Destructive tools (`destroy_entity`, `remove_component`, `save_scene`, `load_scene`) ask first: the first call
  returns a confirmation token, and a second call with `confirm_token` runs it. A project can set allow / confirm /
  deny per tool in `<scene folder>/.aether/agent_permissions.json`. Every call is logged, with arguments hashed, to
  `.aether/agent_session.jsonl` (8 MiB cap). Scene tools stay inside the scene folder (`--scene-root`).

## Gameplay systems

- Animation: `AnimationLookAt` and `AnimationTwoBoneIK` are serializable ECS components that
  apply aim and arm/leg IK after an animator graph evaluates. They accept world-space targets,
  support characters and NPCs, and report invalid configured bones or chains once without
  interrupting animation.
- Animation: `AnimationFootPlacement` applies terrain-aware foot IK after graph evaluation. The
  animation host supplies its world-space ground query, so projects can use physics, navigation,
  or procedural terrain without adding a dependency to the animation module.
- AI decisions: utility brains, state trees and a GOAP planner, with JSON save and load.
- Navigation: a hierarchical path finder over tile borders, and 1000-agent crowd coverage. `NavMesh::FindPath`
  now reuses the path buffers you pass, so a reused `NavPath` does not allocate.
- Navigation: `NavAnimation` is an opt-in ECS bridge that drives named animation graph speed and
  moving parameters from an NPC's measured crowd velocity, including the transition to rest.
- Player: data-driven UI, script UI tables, scene lights, live quality presets, touch and pointer input, on-screen
  controls and platform lifecycle events.
- Data layers: a `DataLayer` component names the layer an entity belongs to, and
  `SetDataLayerActive` switches a whole layer on or off together. It reuses `Active` rather than inventing a
  second notion of enabled, and switching a layer off records each entity's own state so switching it back
  restores what each one had — an entity that was already off does not come back on. Layers survive save and
  load.

## Networking

- Replication gains `SetRelevancy`, `SetPriority` and a shared `ReplicationConfig::bytes_per_second` cap.
- RPCs from a client are rate limited per client (`RpcConfig::calls_per_second`, active once `RpcRouter::Tick` is
  called each frame).
- The replication profile table is bounded (`ReplicationConfig::max_profiled_entities`); costs of despawned
  entities are folded into `NetProfile::retired_*`.
- Predicted abilities: `AbilityPredictor` runs an ability the moment the player presses it and applies the
  server's answer afterwards — acceptance commits the cost, refusal cancels it as if the press never
  happened, and a reply that arrives twice cannot charge twice. It drives abilities authored with
  `commit_on_activate = false`, which the ability system already runs without payment.
- The wire half is `AbilityServer`/`AbilityClient` in `net/`: they move requests and replies only, taking the
  three ability operations as callbacks so networking stays independent of the gameplay module.

## World streaming

- `WorldStreamer` can read cells through an executor (a job system, say) with in-flight, entity and time budgets;
  only `Update()` touches the world.
- **Level instances** place one scene inside another by the prefab rule: the scene is loaded into a temporary
  world, given a synthetic root because a prefab has one root and a scene has as many as it has, and instantiated
  at the placement. `ExpandLevelInstanceCommand` does it as one undoable edit and destroys exactly what its
  instantiation reported creating, and the `expand_level_instance` MCP tool resolves the scene path through the
  same containment `save_scene` and `load_scene` use.

## Assets and rendering

- ASTC texture cooking and GPU compressed textures, block-compressed textures on Vulkan, `DestroyTexture`, and
  memory-pressure trimming for mobile.
- Cooked meshes (`.amesh`) carry LODs and meshlets.
- Asset validation reports what is wrong without importing or opening anything: sources that have gone, and
  references to GUIDs that no longer exist (`assets::ValidateAssets`, the `asset_validate` MCP tool, and the content
  browser's `Issues()`). It reports and repairs nothing, so a person decides what to do.

## Plugins

- Plugins can be packed into one `.apkg`, inspected without unpacking, checked for unmet dependencies, and
  installed with a staged, all-or-nothing install.
- Plugin descriptors carry an ABI number; a plugin built for another ABI is refused with a clear message.
- An offline local plugin registry stores immutable package versions and installs an exact selected version through the staged installer.

## Editor

- A command palette searches the workspace's tools and, once a project is open, its assets. Choosing an
  asset focuses the content browser at it; what "opening" an asset means stays a host decision
  (`AssetOpener`), so the palette itself is usable headless.
- The scene document keeps a **multi-selection** — deduplicated, order-preserving, and not itself an
  undoable edit — and copies it through a **snapshot clipboard**, so a paste still works after the
  originals are deleted or the whole scene is replaced.
- Gizmo **snapping** rounds translation, rotation and scale to a step. A zero step means "snapping off"
  rather than "snap to zero", and scale never collapses to zero: a drag to almost nothing lands on one
  step and keeps its sign.
- The content browser can **stage and commit** the project's `Content/` from its toolbar, reporting git's
  own message when it refuses (nothing to commit, a hook, a missing identity) rather than a generic error.

## Robustness and quality gates

- Every loader reads `$type` safely, rejects files from a newer engine and fails instead of throwing on
  wrong-typed data. 25 fuzz targets cover every in-memory decoder, each with a corpus that includes a valid
  input: the pak reader, scenes, Blueprints, saves, input, UI, sprite data, plugins, the project file and crash
  reports, the AI data, glTF and model import, prefabs, sequences, animation, materials, session datagrams, and
  the four cooked asset containers (`AMSH`, `ATEX`, and the `.amesh`/`.atex` the cooker ships in a pak). The
  cooked-container seeds are generated by `aether_fuzz_seedgen`, so they survive a format change.
- `aether_alloc_audit` (hot paths must not allocate) and `aether_soak` (heap growth over churn, streaming and
  replication) are CTest checks. The soak found and fixed an unbounded replication profile table.
- Load and save results on scenes, prefabs, glTF, images, plugins, projects, input and pak are `[[nodiscard]]`.- `RevertPrefabOverrideCommand` used to dereference a `PrefabInstance` that was not there, and to walk for the
  instance root with whatever entity it had resolved. Both paths are for an entity that is not part of an
  instance (a plain one, or a GUID that names nothing) and both crashed the process; it now refuses and says why,
  which matters because `prefab_revert` calls the same command.
- A gate checks that no MCP tool name is registered twice. Two files registering one name is neither a compile
  error nor a test failure -- only the server sees both -- and a duplicate was in fact written and caught this
  way. Its fixture proves the check rejects a duplicate rather than only passing on the repository.

## Known gaps

- Three renderer pieces are built and tested but **not wired**: `gfx/shader_cache.h`, `gfx/pipeline_cache.h` and
  `gfx/render_thread.h`. The sandbox draws through a GPU-driven path -- a compute pass culls and one
  `ExecuteIndirect` issues the frame -- so a CPU submission path is not a drop-in for it. The caches have no
  caller in `renderer/` either, which is CPU-side only. They stay because they are tested and requested, not
  because anything uses them.

- The hierarchical path finder is **slower than plain Detour everywhere it has been measured**, including the
  dead-end serpentine it was designed for (4.7x), a 1 600-tile mesh (3.6x) and a ~13 700-tile one (4.5x; 80 ms
  against 18 ms). It is marked experimental and nothing in the engine uses it -- build paths with
  `NavMesh::FindPath`. See "The hierarchical pathfinder does not scale" in `docs/design/PERFORMANCE.md`; the
  collinear-waypoint fix that made it 7.3x faster is kept, and a line-of-sight merge was tried and reverted.
- A head-on crowd swap through a narrow middle jams.
- Cell decode time was measured only in a Debug build.
- The Qt editor builds only on Windows; most of the above is verified by the engine's tests, not in the editor.
