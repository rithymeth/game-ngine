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

> **37.1 status: registry done (this step), the rest stays open.** `aether/kit/kit.h` and `kit_registry.cpp` provide `IKit` (`Name`, `Deps`, `RegisterComponents`, `LoadAssets(ctx)`, `InstallBlueprintNodes`, `InstallScriptApi(void*)`, `Stages`) and `KitRegistry` (`Add`, `Resolve` with a deterministic dependency order, duplicate, missing-dependency and cycle errors, install hooks, stage list with dependency `after`). Decision: kits are installed from an explicit list (`RegisterXKit(registry)`), not a static registrar, because the kits are static libraries and the linker can drop a registrar object. Steps 37.4 and 37.5 now use `LoadAssets`, registry script API hooks and `KitEventBus`; still to do: a `CreateRuntime` hook, the `aether_kit()` CMake function, and the remaining kit migrations and runtime ownership.
| **37.2** *(done)* | Module dependency check: `tools/check_module_graph.py` plus a `check_module_graph` target and a CI step. It reads `cmake --graphviz`, fails on a cycle or on an edge not in the checked-in `docs/design/module_layers.txt`, and scans `#include "aether/<module>/..."` against the same layer file. Acceptance: clean today; a seeded-cycle fixture fails. | - |
| **37.3** *(done)* | Stability annotations: `engine/include/aether/core/api.h` (`AETHER_DEPRECATED(version, replacement)` as a real `[[deprecated]]`, `AETHER_STABLE`, `AETHER_EXPERIMENTAL`, `AETHER_INTERNAL`); a level per public module in `docs/design/api_stability.txt` (everything experimental until the 48.3 freeze) with an optional `// @stability:` override per header; `tools/check_api_stability.py` as a test; `docs/manual/api_stability.md`. (`tools/docgen` documents reflected types, not headers, so levels are per module and header, not in the generated reference.) | - |
| **37.4** *(started: a stage-order snapshot test (`Player_StageOrderSnapshot`) and `MakeGameplayKit`/`MakeInventoryKit`/`MakeInteractionKit`/`MakeQuestsKit` now source kit registration and stage dependencies from the registry; optional kit composition, stage application, package-backed asset setup, per-scene kit system creation, and kit scheduler callbacks now live in `player/src/game_kits.cpp`, separate from the main runtime implementation. `KitEventBus` carries inventory events to quests. Inventory, Interaction and Quests scheduler wrappers live in their kit libraries with typed host callbacks. Gameplay owns the Effects, Abilities and Attributes stage declarations and system wrappers. Gameplay effect/ability loading and validation, Inventory item loading, and Quest definition loading/validation now run through `KitRegistry::LoadAssets(context)` in dependency order; the host provides package reads and routes diagnostics. The Gameplay dependency barrier is set to Effects so dependent kits can run before attribute notifications; the original stage snapshot passes. Focused verification passed: 70 kit, Gameplay, Inventory, Quest and Player checks after registry-owned asset loading. Still to do: move per-scene kit system ownership behind an `IKit::CreateRuntime` context, lazy asset loading, remove remaining kit `#if`s from `game.h`, and complete downstream functional verification)* | Migrate gameplay, inventory, interaction and quests to `IKit` (a `*_kit.cpp` each), keep optional-kit selection out of `game.cpp`, move the `quests->Notify` bridge behind the kit event flow. Acceptance: `tests/test_coin_run.cpp` and the functional scenarios pass unchanged, a stage-order snapshot test matches today's order, and dropping then re-adding a kit option doesn't touch `game.cpp`. **Highest risk in the phase** (stage order, lazy loading). | 37.1 |
| **37.5** *(started: the editor Gameplay sample registers components through `KitRegistry`; Player installs kit script APIs through registry hooks, while standalone `ScriptSystem` keeps its existing default installers. Registry hook ordering and Player scripting checks pass; broader editor/test consumers remain)* | The editor, scripting and tests consume the registry (`editor/main.cpp`, `editor/src/workspace`, `tests/CMakeLists.txt`). | 37.4 |
| **37.6** *(done, advisory)* | Analysis gates: an advisory TSan CI job that runs the threaded tests (`AETHER_TEST_FILTER=JobSystem_,Audio_,Net_,NetPlay_,Streaming_`, a new comma-separated name filter in the test runner), an advisory clang-tidy job on the engine and kit sources a PR changes (`.clang-tidy`: `bugprone-*`, `performance-*`, `clang-analyzer-*`), and the `AETHER_WARNINGS_AS_ERRORS` option (off by default; the four gameplay libraries build clean under it with GCC; it is not on in CI yet because Clang and MSVC haven't been checked). Promote each to required after about two weeks clean. | - |
| **37.7** *(started: `[[nodiscard]]` on `SaveResult` (every save, settings and envelope call), `cook::LoadAtex`, `ParseTextureFormat` and `ParseConfiguration`; the five call sites that ignored results were fixed. Still to do: the other modules, the error-return convention decision, naming and const passes)* | API audit sweep, one PR per module group: naming, `const`-ness, `[[nodiscard]]`, an error-return convention, a narrower public surface. `||` across groups. | 37.3 |

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
| **38.7** | Allocation tracking and a per-frame allocation audit (a frame in a sample allocates zero bytes in hot paths). `||` | 38.1 |
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
| **40.2** | Hierarchical pathfinding (`nav/`). `||` | - |
| **40.3** | Crowds of 1000 agents plus a bench in the 38.1 harness. | 38.1, 40.1 |
| **40.4** | Utility AI, GOAP and state trees next to behavior trees (`ai/`). `||` | - |
| **40.5** | Perception 2.0 (`ai/`). | - |
| **40.6** | Behavior debugger and visualization (`editor/src/ai`). | 40.4 |
| **40.7** | Optional ONNX runtime layer (`AETHER_AI_ONNX`, deterministic fallback, licence review). Windows and macOS are CI checks. | - |
| **40.8** *(done: `docs/design/agent_gameplay_ai_boundary.md` documents runtime/editor responsibilities, dependency direction and deterministic simulation constraints; the module graph gate checks that MCP does not link AI)* | Document the agent/gameplay-AI boundary; `mcp/` links nothing from `ai/` (checked by 37.2). | 37.2 |

## Phase 41: Multiplayer 2.0

| Step | Work | Needs |
|---|---|---|
| **41.1** | Headless dedicated-server target (`net/`, `player/`): no window, RHI or audio. | - |
| **41.2** *(done: `LoopbackNetwork` simulates seeded loss, latency, jitter and duplication; `Net_LoopbackNetworkAppliesJitterAndDuplication` checks delay bounds and duplicate delivery, and `Net_ReliableUnderBadConditions` exercises recovery under all four conditions)* | Network conditioner (loss, latency, jitter) in `net/`. `||` | - |
| **41.3** | Replication priority, relevancy and a bandwidth budget. | 41.2 |
| **41.4** | Session and matchmaking interface with a local backend. `||` | - |
| **41.5** | Server-authority and input-validation audit. `||` | - |
| **41.6** | Ability prediction and rollback (`gameplay/`). | 41.3, 37.4 |
| **41.7** | 64-player soak sample on the harness. Loopback only here; real networks and platform services are not testable in this container. | 41.1-41.3 |

## Phase 42: World Streaming 2.0

| Step | Work | Needs |
|---|---|---|
| **42.1** | Large-world coordinates (a double-precision origin) in `engine/math` and the ECS. A breaking change to `Transform` and serialization: **format version and migration required**. Touches physics, rendering, audio and networking. Highest-risk step. | - |
| **42.2** | Async cell streaming with budgets (`streaming/`). `||` | - |
| **42.3** | *(done: `aether/scene/test_world.h` `GenerateTestWorld`: a deterministic grid of buildings and scattered props from a seed, sized by its parameters; a library function, not a `tools/` program, so the flythrough test can call it directly.)* A generated test world, needed for the flythrough test. `||` | - |
| **42.4** | *(started: `GenerateLods` in the same header gives a chain of simplified index buffers with a relative error each, never growing from one level to the next. HLOD merging and writing LODs into the cooked mesh remain)*  LOD and HLOD generation (offline, `tools/cook`). `||` | - |
| **42.5** | Stream audio, navmesh and gameplay data with the cells. | 42.2, 40.1 |
| **42.6** | World-partition authoring: data layers, level instances at scale (`editor/src/world`). | 43.1 |
| **42.7** | *(started: the `world/flythrough_120_frames` benchmark crosses the generated test world and gathers what is in range each frame; the hitch budget over streaming and memory is still to come)* Automated flythrough with a hitch budget. Memory and CPU hitches run here; render hitches need hardware. | 42.1-42.3, 38.1 |

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
| **44.2** | Every edit through the undo stack with agent attribution. | 44.1 |
| **44.3** | A permission file in the project and a session log. | 44.1 |
| **44.4** | Confirmation flow for destructive tools. | 44.3 |
| **44.5** | Tools for scenes, assets and kits, generated per kit. | 37.1, 44.2 |
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
| **46.3** | Plugin packaging and dependency resolution. | 46.2 |
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
| **47.5** | Soak and leak gates. | 38.7 |
| **47.6** | A screenshot and functional test farm per platform. | - |
| **47.7** | Accessibility and localization regression checks. | - |
| **47.8** | Security review of the plugin and MCP surfaces. | 44.3 |

## Phase 48: Aether 1.0

| Step | Work | Needs |
|---|---|---|
| **48.1** *(started: `tools/gate.py` validates a versioned production-gates table, reports pass/fail/n-a statuses, and supports a strict `--require-complete` release check; initial rows are honestly marked `n/a yet`; the table is a CTest check)* | `tools/gate.py`: checks each row of the production gate table and reports pass or fail; rows start as "n/a yet". | - |
| **48.2** | Format-version audit; migration tools for every serialized format; a corpus of old projects in the tests. | - |
| **48.3** | API freeze: promote the stable annotations and block removals in CI. | 37.3 |
| **48.4** | Three sample games (the existing coin run and gem hop, plus one new). | - |
| **48.5** | Release builds with checksums and signing (extends the release job). *Signing needs certificates: the user's decision.* | - |
| **48.6** | Release notes, an upgrade guide and the long-term-support policy. | - |
| **48.7** | Tag a release candidate and run the gate. | all |

## What this container can and can't do

Fine here: everything on Linux GCC/Clang (unit tests, benchmarks on the CPU, the module-graph
script, fuzzers with Clang, the SDK clean-machine test, loopback networking, the portable editor
under Xvfb). Needs hardware or other platforms (verified by CI, or deferred): GPU timing and
rendering parity beyond lavapipe, D3D12 and Metal, Android and iOS builds and devices, Windows
crash dumps, signing certificates, a device farm and marketplace hosting.
