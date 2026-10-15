# The Aether Upgrade: PR-sized plan for M7 (Phases 37-48)

This is the working plan for M7 in [`../ROADMAP.md`](../ROADMAP.md). It breaks every phase into
PR-sized steps, says what each step needs first, and marks what can run **in parallel**. The
steps are numbered `phase.step` (so `37.4` is step 4 of Phase 37).
M8's [production and AETHER-01 plan](M8_PRODUCTION_PLAN.md) uses these engine results as
evidence; Phase 48 release waits for M8.12 product validation on the same candidate.

## Facts the plan rests on (checked against the repository)

- **Modules** are the top-level CMake targets: `Aether::Engine`, Blueprint, Renderer, Animation,
  Audio, UI, VFX, Terrain, Streaming, Sprite2D, Sequencer, Save, Loc, Gameplay, Inventory,
  Interaction, Quests, Net, Nav, AI, Scripting, Physics and Game. Jobs live in
  `engine/include/aether/job`, the RHI in `engine/gfx` (there is no `jobs/` or `rhi/` directory).
- **The module link graph has no cycles today** (83 targets, 201 edges; checked with
  `cmake --graphviz` and a strongly-connected-components pass). Step 37.2 therefore *locks the graph
  in* instead of cleaning it up. The graph covers link edges only; `#include` violations are not
  checked yet.
- `kMaxComponentTypes` is already 256 (`engine/include/aether/ecs/component.h`).
- **An MCP server exists** (`mcp/`, `AETHER_BUILD_MCP`, about 700 lines): scene tools, undo/redo,
  save/load, Play-in-Editor, stdio JSON-RPC, all edits through the editor command stack.
- **CI today** (`.github/workflows/ci.yml`): Linux GCC and Clang, a Linux ASan+UBSan job, a physics
  job, macOS (Apple Silicon, MoltenVK) and Windows (MSVC, D3D12) builds that run the tests, and a
  release job. Warnings are `-Wall -Wextra` (`/W4`), not errors. Only
  `blueprint/bench/blueprint_bench.cpp` is a benchmark, and CI doesn't run it.
- **Missing gates:** TSan, clang-tidy, cppcheck, `-Werror`, coverage, fuzzing, a benchmark suite,
  API annotations (`AETHER_DEPRECATED` and friends), a module-graph check.
- The kits (gameplay, inventory, interaction, quests) are wired into the player by hand
  (`player/src/game.cpp`: includes, `Runtime` members, `Load*` calls, construction, `Player.*`
  stages; the same `AETHER_KIT_*` defines in `player/CMakeLists.txt` and `scripting/CMakeLists.txt`;
  inert Luau installs in `scripting/src/script_api.cpp`). Plugins use `aether_module()` instead
  (`cmake/AetherModules.cmake`, `engine/include/aether/plugin/plugin.h`); kits need typed hooks that
  `IModule` doesn't have.
- **This cloud container** has Linux GCC/Clang, no GPU, no Windows or macOS and no devices. Steps
  that need those are marked *CI-only* or *needs hardware*; they are written and unit-tested as far
  as possible here and verified by the CI jobs.

## Rules

1. **Every step is measurable**: a test, a benchmark with a baseline, or a gate script.
2. **A gate starts advisory.** New CI gates (TSan, clang-tidy, `-Werror`, benchmark regression) run
   as non-blocking for about two weeks of PRs and become required once they are clean.
3. **Breaking changes carry a format version and a migration** (the Phase 6 rule). The big one is 42.1.
4. Steps marked `||` can run in parallel with their siblings; "needs X" lists real dependencies.

## Starting now, all independent

`37.1`, `37.2`, `37.3`, `37.6`, `38.1`, `39.1`, `40.1`, `40.2`, `40.4`, `41.2`, `43.1`, `44.1`,
`45.1`, `46.1`, `47.1`, `48.1`.

**Critical path:** `37.1 -> 37.4 -> 37.5 -> {44.5, 41.6}`. `38.1` gates every benchmark gate, `42.1`
gates the world work, `43.1` gates all editor work.

## Phase 37: Engine 2.0 Core

| Step | Work | Needs |
|---|---|---|
| **37.1** | `IKit` interface and registry (`engine/include/aether/kit/kit.h`, `engine/src/kit/kit_registry.cpp`): `Name`, `Deps`, `RegisterComponents`, `LoadAssets(ctx)` (reads the package, reports problems), `CreateRuntime(ctx)`, `Systems(scheduler)` (stages with `after` from the kit's names), `InstallBlueprintNodes`, `InstallScriptApi`. Cross-kit coupling goes through a typed `KitEventBus` (inventory publishes `ItemEvent`, quests subscribes) instead of kit-to-kit includes. `AETHER_KIT(Type)` static registrar and an `aether_kit()` CMake function beside `aether_module()` that makes the `AETHER_KIT_<NAME>` option, links the kit and exposes `AETHER_KITS` to the player, scripting and the editor. Design and registry only, no migration. Test: `tests/test_kit_registry.cpp` (dependency order, duplicate name, missing dependency, deterministic stage order). | - |

> **37.1 status: registry done (this step), the rest stays open.** `aether/kit/kit.h` and `kit_registry.cpp` provide `IKit` (`Name`, `Deps`, `RegisterComponents`, `LoadAssets(ctx)`, `CreateRuntime(ctx)`, `InstallBlueprintNodes`, `InstallScriptApi(void*)`, `Stages`) and `KitRegistry` (`Add`, `Resolve` with a deterministic dependency order, duplicate, missing-dependency and cycle errors, install hooks, scene runtime creation and stage list with dependency `after`). Decision: kits are installed from an explicit list (`RegisterXKit(registry)`), not a static registrar, because the kits are static libraries and the linker can drop a registrar object. Steps 37.4 and 37.5 now use `LoadAssets`, registry script API hooks and `KitEventBus`; still to do: the `aether_kit()` CMake function and remaining kit migrations.
| **37.2** *(done)* | Module dependency check: `tools/check_module_graph.py` plus a `check_module_graph` target and a CI step. It reads `cmake --graphviz`, fails on a cycle or on an edge not in the checked-in `docs/design/module_layers.txt`, and scans `#include "aether/<module>/..."` against the same layer file. Acceptance: clean today; a seeded-cycle fixture fails. | - |
| **37.3** *(done)* | Stability annotations: `engine/include/aether/core/api.h` (`AETHER_DEPRECATED(version, replacement)` as a real `[[deprecated]]`, `AETHER_STABLE`, `AETHER_EXPERIMENTAL`, `AETHER_INTERNAL`); a level per public module in `docs/design/api_stability.txt` (everything experimental until the 48.3 freeze) with an optional `// @stability:` override per header; `tools/check_api_stability.py` as a test; `docs/manual/api_stability.md`. (`tools/docgen` documents reflected types, not headers, so levels are per module and header, not in the generated reference.) | - |
| **37.4** *(started: a stage-order snapshot test (`Player_StageOrderSnapshot`) and `MakeGameplayKit`/`MakeInventoryKit`/`MakeInteractionKit`/`MakeQuestsKit` now source kit registration and stage dependencies from the registry; optional kit composition, stage application, package-backed asset setup and scheduler callbacks live in `player/src/game_kits.cpp`. `KitEventBus` carries inventory events to quests. Gameplay effect/ability loading, Inventory item loading and Quest definition loading/validation run through `KitRegistry::LoadAssets(context)`. Inventory, Interaction and Quests scene systems are now owned by kit-created `IKitRuntime` instances retained by the scene runtime; kit libraries construct those systems from a `KitRuntimeContext` service resolver. Still to do: lazy asset loading, remove remaining kit `#if`s from `game.h`, and complete downstream functional verification)* | Migrate gameplay, inventory, interaction and quests to `IKit` (a `*_kit.cpp` each), keep optional-kit selection out of `game.cpp`, move the `quests->Notify` bridge behind the kit event flow. Acceptance: `tests/test_coin_run.cpp` and the functional scenarios pass unchanged, a stage-order snapshot test matches today's order, and dropping then re-adding a kit option doesn't touch `game.cpp`. **Highest risk in the phase** (stage order, lazy loading). | 37.1 |
| **37.5** *(started: the editor Gameplay sample and Qt editor now register gameplay components through `KitRegistry`; the Qt editor links and registers each enabled Inventory, Interaction and Quests kit so editor schemas follow the selected build. Player installs kit script APIs through registry hooks, while standalone `ScriptSystem` keeps its existing default installers. Registry hook ordering and Player scripting checks pass; test consumers and remaining editor migration work remain)* | The editor, scripting and tests consume the registry (`editor/main.cpp`, `editor/src/workspace`, `tests/CMakeLists.txt`). | 37.4 |
| **37.6** *(done, advisory)* | Analysis gates: an advisory TSan CI job that runs the threaded tests (`AETHER_TEST_FILTER=JobSystem_,Audio_,Net_,NetPlay_,Streaming_`, a new comma-separated name filter in the test runner), an advisory clang-tidy job on the engine and kit sources a PR changes (`.clang-tidy`: `bugprone-*`, `performance-*`, `clang-analyzer-*`), and the `AETHER_WARNINGS_AS_ERRORS` option (off by default; the four gameplay libraries build clean under it with GCC; it is not on in CI yet because Clang and MSVC haven't been checked). Promote each to required after about two weeks clean. | - |
| **37.7** *(started: `[[nodiscard]]` on the scene save/load family (binary and JSON, file and memory), prefab save/load, glTF load and image decode, plus `SaveResult`; the plugin, project, input-binding and asset-metadata load/save functions, `ParseAssetGuid`, `PakKey::FromHex` and `PakWriter::Write`, with the dropped results fixed (an asset-metadata write failure during a scan is now logged) (every save, settings and envelope call), `cook::LoadAtex`, `ParseTextureFormat` and `ParseConfiguration`; the five call sites that ignored results were fixed. Still to do: the other modules, the error-return convention decision, naming and const passes)* | API audit sweep, one PR per module group: naming, `const`-ness, `[[nodiscard]]`, an error-return convention, a narrower public surface. `||` across groups. | 37.3 |

**Decisions for the user:** the error-handling convention (`std::expected` against the current
`bool` + `std::string*`), whether kits stay compile-time options (this plan assumes yes), and
when advisory gates become required.

## Phase 38: Performance

| Step | Work | Needs |
|---|---|---|
| **38.1** *(done)* | Benchmark harness (`bench/`, `Aether::Bench`): generalises `blueprint_bench.cpp`; JSON reports and a baseline format; frame-time histograms and a per-system memory table. | - |
| **38.2** *(started: iteration, create/destroy, add/remove component benches, plus `ecs/query_50k_of_100k` across mixed archetypes with a 50k-match self-check)* | ECS bench: iteration, structural change, queries; decide whether 256 component types is enough or make it dynamic. `||` | 38.1 |
| **38.3** *(started: a 1024-job batch bench that now aborts if endpoint jobs did not run)* | Job-system bench. `||` | 38.1 |
| **38.4** *(started: binary and JSON scene save/load benches at 10k entities now verify non-empty saves and exact loaded entity counts, with separate save worlds to avoid cross-benchmark accumulation; current environment run passed all four: binary save 13.2 ms/load 15.7 ms, JSON save 18.6 ms/load 29.2 ms. Prior Debug GCC 10k-entity under-2s test and cook measurements remain)* | Scene load, save/load and cook benches, including the 10k-entity under 2 s budget. `||` | 38.1 |
| **38.5** *(done except physics: Blueprint VM loop (10k iterations) and 1k-instance tick, Luau 10k calls and a 100k-iteration loop; Debug GCC here: 0.45 ms, 0.47 ms, 6.7 ms, 1.8 ms; the physics bench waits for a physics-ON build)* | Physics, Blueprint VM and Luau call benches. `||` | 38.1 |
| **38.6** *(started: `bench/baseline.json` checked in and compared in the GCC job with a 3x tolerance, advisory; comparisons reject invalid or duplicate entries, fail when an established benchmark is missing, and allow new names alongside the complete baseline; promote it to required after about two weeks of clean runs and regenerate the baseline from a CI artifact)* | Baselines and the CI regression gate. Shared runners are noisy, so gate on instruction counts or on a ratio against a calibration loop. | 38.2-38.5 |
| **38.7** | *(started: `core/alloc_track.h` (`AllocScope`, per-thread counts fed by a program's own operator new) and `tools/alloc_audit` (`aether_alloc_audit`, ctest `alloc_audit`), which replaces operator new for itself and fails if ECS iteration over 10k entities, blackboard reads and writes, a utility-AI choice or a state-tree tick allocates in steady state, and checks that the counter does see real allocations. Still to add: behavior-tree ticks, navmesh queries and the replication send path)* Allocation tracking and a per-frame allocation audit (a frame in a sample allocates zero bytes in hot paths). `||` | 38.1 |
| **38.8** | Renderer: render thread, draw submission, culling, pipeline and shader caches, Tracy zones. *Needs hardware* for GPU timing. | 38.1 |

## Phase 39: Graphics 2.0

| Step | Work | Needs |
|---|---|---|
| **39.1** *(done: `FrameGraph::Validate` checks compiled pass culling/order, read hazards, dependencies and queue waits, transient lifetimes and placements; the forward-plus, error and 200 randomized graph tests validate their compiled plans)* | Render-graph validator (`renderer/`): hazards, unused passes, lifetimes; CPU unit tests. | - |
| **39.2** | Backend parity harness: same scene on every backend, compared with a tolerance (`tools/functional`); lavapipe covers Vulkan in CI, D3D12 and Metal on the Windows and macOS jobs. | the screenshot test |
| **39.3** | Advanced PBR terms (clear coat, sheen, anisotropy) in material codegen and shaders. | 39.2 to verify |
| **39.4** | Subsurface, hair, cloth, decals. | 39.3 |
| **39.5** | *(done: `aether/cook/meshlet_cook.h` `BuildMeshlets` on meshoptimizer (fetched at configure time, MIT, hidden behind the .cpp): meshlets of at most 64 vertices and 124 triangles by default, each with a bounding sphere and normal cone; deterministic; tests check every triangle is in exactly one meshlet. Written into the cooked mesh and the pak by 39.6a)*  Meshlet cluster builder in the asset pipeline (meshoptimizer): an offline CPU step, testable here. `||` | - |
| **39.6** | *(39.6a done: `aether/cook/mesh_cook.h` `CookMesh`/`SaveAmesh`/`LoadAmesh`, an "AMSC" v1 container with each primitive's vertices, LOD chain and per-LOD meshlets and a CRC-32 trailer; the cooker writes `Cooked/<guid>.amesh` for every Mesh sub-asset (`cook_meshes`); skinned primitives keep level 0 only. The runtime below is still open.)* Virtual-geometry runtime and its fallback for hardware without mesh shaders. *Needs hardware.* | 39.5 |
| **39.7** | Virtual texturing. *Needs hardware.* | - |
| **39.8** | GI, shadow and upscaler maturation. *Needs hardware.* | 39.2 |

## Phase 40: AI Engine

| Step | Work | Needs |
|---|---|---|
| **40.1** | Navmesh tiles streamed in and out, dynamic obstacles (`nav/`). `||` | - |
| **40.2** | Hierarchical pathfinding (`nav/`). **Done**: `HierarchicalPathfinder` (tile-border portal graph, A*, short Detour legs; within 1.2x of the full path in tests). Not yet rebuilt per tile after dynamic edits. `||` | - |
| **40.3** | *(started: `NavCrowd_ThousandAgentsCrossAnOpenField` runs 1000 agents as a stream across an open field, about 7 ms per frame on this machine with 96% arrived and none failed in 30 s; a head-on swap through the middle jams (only 69% arrived), which is the case to improve; `nav/crowd_1000_agents_30_frames` is in the bench harness at about 8 ms per frame; `nav/path_across_100_tiles_*` shows the hierarchical pathfinder is about 8x slower than plain Detour on an open floor (4.8 ms vs 0.6 ms for 20 queries), the time is in the many short Detour legs (each re-finds its start and goal polygon), not the portal graph, which is already thinned to one portal per stretch of border; its value is on meshes too big for one Detour search, and I could not make plain FindPath return Partial in this container (a 300 x 300 serpentine of 9 walls, 2272 units of path, still completes), so the case where it beats plain Detour is unproven here; a serpentine test checks it stays within 1.15x of the shortest)* Crowds of 1000 agents plus a bench in the 38.1 harness. | 38.1, 40.1 |
| **40.4** | *(done: `ai/utility.h` (`UtilityBrain`: response curves, considerations over blackboard numbers, compensated scores, inertia against flip-flopping), `ai/state_tree.h` (`StateTree`: hierarchical states, ordered transitions, enter/exit effects on the blackboard, a per-tick transition cap) and `ai/goap.h` (`PlanGoap`: cheapest plan by uniform-cost search over up to 64 facts, with a node budget); utility brains and state trees save as versioned JSON and have a fuzz target. Hooking them into `bt_runtime` ticking and the behavior debugger (40.6) is still to do)* Utility AI, GOAP and state trees next to behavior trees (`ai/`). `||` | - |
| **40.5** | Perception 2.0 (`ai/`). | - |
| **40.6** | Behavior debugger and visualization (`editor/src/ai`). | 40.4 |
| **40.7** | Optional ONNX runtime layer (`AETHER_AI_ONNX`, deterministic fallback, licence review). Windows and macOS are CI checks. | - |
| **40.8** *(done: `docs/design/agent_gameplay_ai_boundary.md` documents runtime/editor responsibilities, dependency direction and deterministic simulation constraints; the module graph gate checks that MCP does not link AI)* | Document the agent/gameplay-AI boundary; `mcp/` links nothing from `ai/` (checked by 37.2). | 37.2 |

## Phase 41: Multiplayer 2.0

| Step | Work | Needs |
|---|---|---|
| **41.1** | Headless dedicated-server target (`net/`, `player/`): no window, RHI or audio. | - |
| **41.2** *(done: `LoopbackNetwork` simulates seeded loss, latency, jitter and duplication; `Net_LoopbackNetworkAppliesJitterAndDuplication` checks delay bounds and duplicate delivery, and `Net_ReliableUnderBadConditions` exercises recovery under all four conditions)* | Network conditioner (loss, latency, jitter) in `net/`. `||` | - |
| **41.3** | Replication priority, relevancy and a bandwidth budget. **Done**: `SetRelevancy`, `SetPriority`, `ReplicationConfig::bytes_per_second` (fair share per client); defaults unchanged. | 41.2 |
| **41.4** | Session and matchmaking interface with a local backend. `||` | - |
| **41.5** | Server-authority and input-validation audit. **Done**: ownership, kind and argument checks were already enforced and are now tested with hostile input; added a per-client RPC token bucket (`RpcConfig::calls_per_second`, `RpcRouter::Tick`); movement floods were already capped. `||` | - |
| **41.6** | Ability prediction and rollback (`gameplay/`). | 41.3, 37.4 |
| **41.7** | *(done on loopback: `Replication_SixtyFourPlayersConvergeThroughLoss`; it found a real bug, fixed: with a tight per-client budget an entity bigger than the budget was never sent, so the snapshot budget is now soft by one entity)* 64-player soak sample on the harness. Loopback only here; real networks and platform services are not testable in this container. | 41.1-41.3 |

## Phase 42: World Streaming 2.0

| Step | Work | Needs |
|---|---|---|
| **42.1** | Large-world coordinates (a double-precision origin) in `engine/math` and the ECS. A breaking change to `Transform` and serialization: **format version and migration required**. Touches physics, rendering, audio and networking. Highest-risk step. | - |
| **42.2** | Async cell streaming with budgets (`streaming/`). **Done**: `WorldStreamer::SetExecutor` (reads off-thread, world applied in `Update`), in-flight cap, entity and time apply budgets, stale reads dropped. `||` | - |
| **42.3** | *(done: `aether/scene/test_world.h` `GenerateTestWorld`: a deterministic grid of buildings and scattered props from a seed, sized by its parameters; a library function, not a `tools/` program, so the flythrough test can call it directly.)* A generated test world, needed for the flythrough test. `||` | - |
| **42.4** | *(started: `GenerateLods` in the same header gives a chain of simplified index buffers with a relative error each, never growing from one level to the next. HLOD merging and writing LODs into the cooked mesh remain)*  LOD and HLOD generation (offline, `tools/cook`). `||` | - |
| **42.5** | Stream audio, navmesh and gameplay data with the cells. | 42.2, 40.1 |
| **42.6** | World-partition authoring: data layers, level instances at scale (`editor/src/world`). | 43.1 |
| **42.7** | *(started: the `world/flythrough_120_frames` benchmark crosses the generated test world and gathers what is in range each frame; `Streaming_FlythroughStaysInsideTheHitchBudget` now flies the world through the async streamer with deterministic budgets on entities applied per frame and entities alive, plus a loose frame-time bound; a worst Update of about 35 ms for 400 entities was measured in an unoptimised Debug build, where decoding the cell dominates (16 ms for 214 entities in the binary scene loader, which allocates a name string and a data vector per component); it needs a Release measurement before anyone optimises it)* Automated flythrough with a hitch budget. Memory and CPU hitches run here; render hitches need hardware. | 42.1-42.3, 38.1 |

## Phase 43: Editor 2.0

| Step | Work | Needs |
|---|---|---|
| **43.1** | Split `editor/main.cpp` (about 2200 lines) into panels under `editor/src/workspace`. Blocks the rest. | - |
| **43.2** | ImGui docking and layout presets. | 43.1 |
| **43.3** | Gizmos (rotate/scale, snapping), multi-select, copy/paste, Simulate mode. `||` | 43.1 |
| **43.4** | New/Open Project UI. `||` | - |
| **43.5** | Command palette and global search. `||` | 43.1 |
| **43.6** | Asset reference viewer, bulk rename/move, validation (`editor/src/content`). `||` | - |
| **43.7** | Source-control integration (a git CLI wrapper). `||` | - |
| **43.8** | Task walkthrough scripts and an editor soak test. D3D12 and Metal need the Windows and macOS jobs; the portable shell runs under Xvfb here. | 43.1-43.5 |

## Phase 44: AI-Native Editor

| Step | Work | Needs |
|---|---|---|
| **44.1** *(done for the protocol audit: malformed fields, required arguments, schema property types, handler exceptions and message/result limits are tested; `tests/test_mcp.cpp` drives initialize and tool calls through a stub stdio client; `tools/list` reports the schema version)* | Audit and harden `mcp/` (`editor_tools.cpp`, `mcp_server.cpp`): version the tool schemas, test the protocol with a stub client. | - |
| **44.2** *(done for the existing authoring surfaces: `ICommand` history entries retain their source through undo/redo; Qt scene edits are tagged `user`, MCP edits are tagged `agent`, and Qt undo/redo tooltips plus MCP history responses expose the source. MCP prefab revert, instantiation and apply use the same attributed history; apply restores both asset data and instance overrides. `kit_put`, `asset_set_import_settings`, `asset_import`, `content_write`, `asset_add_files`, `asset_create_folder`, `project_set`, `asset_move` and `asset_delete` use this history; source, metadata and GUID sidecars restore on undo/redo, add-files backups live under `Saved/` only while history retains them, folder undo is allowed only while empty, import metadata is restored while immutable derived-cache entries remain reusable, and import settings are reimported when requested.)* | Every edit through the undo stack with agent attribution. | 44.1 |
| **44.3** | *(done: `mcp/permissions.h` `PermissionPolicy`, read from `<scene folder>/.aether/agent_permissions.json` (allow, confirm or deny per tool, a default, 1 MiB cap, strict parsing); every tool call is recorded in `.aether/agent_session.jsonl` with the tool, outcome and a hash of the arguments, never the arguments, capped at 8 MiB)* A permission file in the project and a session log. | 44.1 |
| **44.4** | *(done: `Tool::destructive` marks `destroy_entity`, `destroy_entities`, `remove_component`, `save_scene` and `load_scene`; the built-in rule answers "confirm" with a single-use token that expires after 5 minutes (at most 32 outstanding) and runs the call only when it is repeated with the same arguments plus `confirm_token`)* Confirmation flow for destructive tools. | 44.3 |
| **44.5** *(in progress: the existing kit type table generates `kit_validate_<type>` and `kit_put_<type>` alongside generic endpoints; per-type writes use the agent-attributed undo stack. Scene entity discovery supports exact name and tag plus component filters and reports hierarchy parents; `duplicate_entity` copies reflected components as one undoable action; `duplicate_entities` copies up to 64 unique entities into one undo transaction after validating their sources; `create_entities` creates up to 64 fully validated entity definitions in one undo transaction; `destroy_entities` deletes up to 64 unique entities as one confirmed, undoable action after validating the selection; `set_components_many` applies partial field patches to up to 64 unique entities as one agent-attributed undo action after validating all entities and component values; `add_components_many` adds validated components with optional initial fields to up to 64 unique entities as one agent-attributed undo action; `reparent_many` applies up to 64 hierarchy changes in one undo action after validating the final graph and ordering attachments safely. Asset imports can target one asset, all pending imports or a selected batch as one undoable group; `asset_set_import_settings_many` applies settings/label changes to up to 64 unique assets as one undoable group with optional batch reimport; `asset_move_many` moves up to 64 independent assets/folders as one undoable group after rejecting overlapping paths; `asset_create_folders` creates up to 64 folders under existing parents as one undoable group after validating every path; `asset_delete_folders` removes up to 64 empty Content folders as one undoable group, refusing paths outside Content; `asset_delete_many` deletes up to 64 selected assets in one undo group after checking external references for the entire selection; `content_write_many` writes up to 64 text assets as one undo group after validating all paths and source/metadata snapshots; `content_read_many` reads up to 64 unique content files with a bounded response size; `asset_info_many` returns full details for up to 32 unique assets with a bounded response; `asset_references_many` queries bounded dependency/referencer graphs for up to 32 unique assets. Broader scene and asset coverage remains.)* | Tools for scenes, assets and kits, generated per kit. | 37.1, 44.2 |
| **44.6** | Build, test and bounded Play-in-Editor capture tools (capture needs a GPU). | 44.2 |
| **44.7** | Reference agents (code, asset, level/game design), outside the engine. | 44.5, 44.6 |

## Phase 45: Mobile and Console

| Step | Work | Needs |
|---|---|---|
| **45.1** | *(already done before this plan was written: actions, mapping contexts and saved rebinds; only a remapping UI is missing)*  Input abstraction and remapping UI (`engine/input`). `||` | - |
| **45.2** | *(started: `aether/platform/lifecycle.h` has `PlatformLifecycle` (suspend, resume and memory-pressure events, permission requests asked once) and a `MockLifecycleBackend`; wiring a real backend through a platform plugin remains)*  Lifecycle, permissions and memory-pressure events in `platforms/` (the interface and a mock backend are testable here). `||` | - |
| **45.3** | Android build: CMake toolchain, Gradle, signing. *CI-only* (NDK). | 45.2 |
| **45.4** | iOS build. *CI-only* (macOS runners). | 45.2 |
| **45.5** | Touch and haptics. | 45.1 |
| **45.6** | Console certification checklists, storage and suspend/resume through the platform-plugin interface. `||` | 45.2 |
| **45.7** | Device-farm job. *Needs an account for a paid service: the user's decision.* | 45.3, 45.4 |

## Phase 46: Developer Ecosystem

| Step | Work | Needs |
|---|---|---|
| **46.1** | CMake package config (`install(EXPORT)`, `AetherConfig.cmake`) for all modules. | - |
| **46.2** | *(done: `kPluginAbi` and `PluginDescriptor::abi`; a plugin built for another ABI is refused with a clear message when it is enabled, a descriptor without the field is accepted, new scaffolds carry the current ABI)* A versioned plugin ABI (an ABI integer in the descriptor and the load path). | 37.3 |
| **46.3** | Plugin packaging and dependency resolution. **Done** (`aether/plugin/package.h`): `PackagePlugin` to one `.apkg`, `InspectPluginPackage`, `UnmetDependencies`, and a staged `InstallPluginPackage` that refuses mismatched names, several folders, newer ABIs and damaged entries. Not yet: a CLI or editor command, and installing dependencies automatically. | 46.2 |
| **46.4** | Templates built in CI against the installed SDK. | 46.1 |
| **46.5** | A clean-machine SDK test job: install into a fresh prefix, build a sample in a container. Runs on Linux here. | 46.1 |
| **46.6** | Marketplace specification and a local registry. *The hosting and signing model is the user's decision.* | 46.3 |
| **46.7** | Documentation site generated from the code and the manual (extends `tools/docgen`). `||` | - |

## Phase 47: Production QA

| Step | Work | Needs |
|---|---|---|
| **47.1** *(started: pak, scene, Blueprint, gameplay-data, prefab, material, sequence, animation, manifest, save-envelope, session-info, asset metadata, bounded texture import and bounded self-contained glTF parsing have fuzz targets. Metadata has wrong-type coverage; image decoding enforces a pixel budget; glTF memory parsing refuses external file URIs. Still to do: full model-import result generation and richer valid seeds for the other loaders)* | libFuzzer harnesses (`tools/fuzz/`, `AETHER_BUILD_FUZZERS`, Clang): pak reader, scene, Blueprint and save loaders, the net protocol, importers. One PR per target, runnable here. `||` | - |
| **47.2** | A corpus and a fixed-budget fuzz job in CI. | 47.1 |
| **47.3** | Editor autosave and crash recovery (extends the crash reporter). | - |
| **47.4** | Crash-dump symbolication. The Windows dump path is Windows-CI-only. | - |
| **47.5** | Soak and leak gates. **Started**: `tools/soak` (ctest `soak`, `AETHER_SOAK_SCALE`) gates exact heap growth over entity churn, streaming and replication; it found the replication profile table growing without bound (now capped by `max_profiled_entities`). Still to do: long runs in CI, GPU and audio, editor sessions. | 38.7 |
| **47.6** | A screenshot and functional test farm per platform. | - |
| **47.7** | Accessibility and localization regression checks. | - |
| **47.8** | *(started: the MCP scene tools are confined to a scene folder; the plugin descriptor loader reads from bytes with a 1 MiB cap and no longer throws on a non-string `$type`, with a fuzz target and corpus. Every other loader now reads `$type` through `JsonString`, so a non-string `$type` fails instead of throwing (input assets, project, scene, sprite atlas and tilemaps, save envelope), and input assets and the 2D loaders (sprite atlas, tileset, tilemap) have fuzz targets; `tools/fuzz/mutate_corpus.py` mutates a corpus and replays each mutant, a stand-in for libFuzzer on a machine without Clang (about 60,000 mutants over every corpus found the two crashes below and nothing else); the project file and crash report parser have a fuzz target; UI layouts, themes and animations have a fuzz target too (the UI animation loader and a Blueprint node whose `config` is not an object were crashes the local mutation run found: the first now fails cleanly, the second is a load error, and the Blueprint compiler turns any leftover wrong-typed node setting into a compile error); the 2D loaders now fail on wrong-typed fields, a malformed size array and a solid-tile index that would allocate gigabytes. Still to review: plugin Content mounts and symlinks, and the MCP simulation tools)* Security review of the plugin and MCP surfaces. | 44.3 |

## Phase 48: Aether 1.0

| Step | Work | Needs |
|---|---|---|
| **48.1** *(started: `tools/gate.py` validates a versioned production-gates table, reports pass/fail/n-a statuses, and supports a strict `--require-complete` release check; initial rows are honestly marked `n/a yet`; the table is a CTest check)* | `tools/gate.py`: checks each row of the production gate table and reports pass or fail; rows start as "n/a yet". | - |
| **48.2** | *(started: `docs/design/format_versions.txt` lists every serialized format with its version and what a loader does with an older or newer file; `tests/test_format_versions.cpp` checks that each JSON loader refuses a file from a newer engine and that the default saved documents carry a version and round-trip. The audit found three loaders that accepted any version (sprite atlas, tileset, tilemap); they now refuse a newer one. No format has a second version yet, so the old-project corpus and migration tools wait for the first bump)* Format-version audit; migration tools for every serialized format; a corpus of old projects in the tests. | - |
| **48.3** | *(first freeze done: 27 headers of `engine/` math, core, memory, ecs (entity, world, component), reflection and pak are `stable`; `tools/check_api_stability.py` fails when a name disappears from one, baseline in `docs/design/api_frozen.txt`, accepted removals in `docs/design/api_removed.txt`. The name scan is coarse, it sees removals and renames but not changed signatures; later freezes are per-module decisions. `samples/frozen_api_game` is a headless game written only against stable headers, `--consumer` fails CI if a game names an unfrozen header, a stable header may not include an experimental one (`ecs/archetype.h` became `internal` for this), and `tests/check_api_freeze.py` proves each check fails for the right reason.)* API freeze: promote the stable annotations and block removals in CI. | 37.3 |
| **48.4** | Three sample games (the existing coin run and gem hop, plus one new). | - |
| **48.5** | Release builds with checksums and signing (extends the release job). *Signing needs certificates: the user's decision.* | - |
| **48.6** | *(started: `docs/releases/NEXT.md` drafts the notes since v0.27.6, `docs/manual/13-upgrading.md` is the upgrade guide, and `docs/manual/support_policy.md` is a proposal for the support policy, waiting on the maintainer's decisions)* Release notes, an upgrade guide and the long-term-support policy. | - |
| **48.7** | Tag a release candidate and run the gate. | all |

## What this container can and can't do

Fine here: everything on Linux GCC/Clang (unit tests, benchmarks on the CPU, the module-graph
script, fuzzers with Clang, the SDK clean-machine test, loopback networking, the portable editor
under Xvfb). Needs hardware or other platforms (verified by CI, or deferred): GPU timing and
rendering parity beyond lavapipe, D3D12 and Metal, Android and iOS builds and devices, Windows
crash dumps, signing certificates, a device farm and marketplace hosting.
