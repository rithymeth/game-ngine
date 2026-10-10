# M8: Production Technology for Existing Systems

M8 is a quality and integration pass over capabilities Aether already has. Its
purpose is to make current game workflows more reliable, faster, easier to
debug and consistent across supported platforms. It does not add new engine
systems, user-facing feature families, platform targets or gameplay concepts.

M7's Aether 1.0 production gate remains authoritative for the 1.0 release, but
it is not the prerequisite for a narrower pre-1.0 proof release. Use the
AETHER-01 First Contact project as the representative workload for this
improvement pass. Keep work inside existing APIs and workflows; if an
improvement requires a new public subsystem or feature, record it as deferred
work outside this milestone. Passing the proof-release gates does not complete
M7 or certify Aether 1.0.

Every performance claim needs a repeatable benchmark or capture with hardware,
compiler, build configuration, workload and baseline recorded. Changes to
stable APIs or serialized formats are out of scope; any later-approved format
change still needs a version and migration.

## Scope rules

- Improve correctness, reliability, performance, diagnostics, usability and
  integration of behavior already implemented.
- Fix bugs and finish documented gaps in existing workflows where doing so
  does not create a new feature family.
- Prefer targeted changes with before/after evidence and a clear rollback path.
- Do not add a feature merely because it appears in a previous aspirational
  roadmap or in the deferred list below.
- If an existing capability is only partially present, first establish its
  current supported behavior. Do not silently broaden its contract.

## Workstreams

| Workstream | Improve existing capability | Evidence |
|---|---|---|
| Build and release reliability | Reproducibility, dependency caching, actionable diagnostics, artifact completeness and failure triage in the existing CI matrix | Repeated clean CI runs; artifact and package contents checked against the existing release contract |
| Performance and profiling | Existing benchmark harness, Tracy integration, profiler panel, ECS/system timing and scene/cook workloads | Stable before/after reports with runner variance recorded; seeded regression is detected |
| ECS and jobs | Existing archetype storage, queries, structural changes, scheduler ordering and job system | Existing workloads retain exact results; query, churn and job benchmarks improve or remain within an agreed budget |
| Rendering | Existing D3D12/Vulkan/Metal paths, PBR materials, shadows, GPU culling, frame graph and editor/player rendering | Existing scenes pass image and functional checks on supported backends; CPU/GPU captures identify regressions |
| Physics and gameplay | Existing Jolt integration, character movement, collision events, gameplay tags/effects/abilities and kits | Existing sample and unit scenarios remain deterministic within their contract; edge cases and step cost are documented |
| Networking | Existing transport, delta snapshots, relevancy, RPCs, prediction/interpolation, sessions and network profiler | Existing loopback and multiplayer scenarios pass seeded impairment runs; packet and bandwidth reports remain within current budgets |
| Content, scripting and editor | Existing asset database/importers/cooker/paks, hot reload, Blueprints, Luau, save/settings, project templates and authoring workflows | Existing project samples import, edit, save, cook, package and run without data loss; diagnostics point to actionable fixes |

## Proposed sequence

Steps are ordered to establish a trustworthy baseline before tuning. Work may
proceed in parallel only after the shared baseline and supported-platform
matrix are agreed.

| Step | Work | Acceptance evidence |
|---|---|---|
| **M8.1** | Inventory existing shipped behavior, known defects, supported configurations, flaky checks and performance baselines. Mark proposed new capabilities as deferred. | A reviewed baseline table links each existing subsystem to its current tests, benchmarks, CI job and known limitation; no deferred feature is represented as an M8 deliverable |
| **M8.2** | Stabilize existing build, test, package and release workflows across current Windows, Linux and macOS targets. Improve logs, cache keys and artifact checks only where the current workflow is ambiguous or unreliable. | Repeated CI runs are green; a clean checkout can produce the same expected existing artifacts; failures identify the owning step |
| **M8.3** | Tune the existing ECS, scheduler and job system using representative query, structural-change, scene-load and job workloads. Fix correctness issues before changing hot paths. | Before/after benchmark reports and unchanged result checks for existing engine and sample workloads |
| **M8.4** | Improve existing renderer and backend parity: fix rendering defects, reduce measured CPU/GPU costs, validate current frame-graph behavior and shader/material paths. | Existing visual/functional scenes pass across supported backends; target-hardware captures show the affected costs and image differences |
| **M8.5** | Harden existing physics, gameplay and multiplayer paths: edge cases, determinism expectations, packet impairment recovery, replication and save/reload interactions. | Existing gameplay and loopback scenarios pass under documented seeded conditions; no new replication or physics capability is required |
| **M8.6** | Polish current content, scripting, save and editor workflows: import/cook diagnostics, cache invalidation, hot-reload reliability, Blueprint/Luau errors, project templates and recovery from interrupted edits. | Existing sample projects complete their established edit, save, cook and run workflows without lost data; regression cases cover each fixed defect |
| **M8.7** | Set maintenance thresholds for the current supported feature set and publish a compatibility and known-limitations update. | CI, benchmark and sample-project results meet reviewed thresholds for sustained runs; docs accurately describe what is supported and verified |

## Priority order

1. Correctness and data safety in existing workflows.
2. Reproducible builds, tests, packages and supported-platform behavior.
3. Performance work selected from profiler and benchmark evidence.
4. Editor usability and diagnostics for existing workflows.
5. Documentation and compatibility accuracy.

Do not optimize a subsystem without measurements, and do not accept a speedup
that weakens correctness, determinism, visual output or data compatibility.
Shared-runner performance thresholds should remain advisory until their noise
and repeatability are understood.

## Current engine update queue

This is the active, engine-only execution plan for the next refinement. It
tracks improvements to existing contracts; it does not authorize a new feature
family. Keep AETHER-01 and other game content out of this workstream. Update
the status and evidence here as each increment is completed.

| Order | Increment | Scope and acceptance | Status |
|---|---|---|---|
| 1 | D3D12 buffer ownership and validation | Make the existing buffer wrapper move-only; unmap persistent CPU mappings on destruction/replacement; reject zero-sized buffers and UAVs on unsupported heaps; guard moved-from GPU-address queries; diagnose invalid upload/readback operations and ranges. Build the engine and player and check the focused diff. | **Complete locally:** `aether_engine` and `aether_player` build successfully; `git diff --check` passes. Automated tests were not run for this increment. |
| 2 | GPU resource lifetime and synchronization audit | Trace how existing buffers, textures, descriptors and command submissions are owned across frames and scene/device teardown. Identify concrete lifetime or fence-ordering defects, then fix only demonstrated defects without introducing implicit GPU waits or a new resource-management subsystem. Acceptance: documented ownership/fence expectations and a repeatable reproduction or existing regression check for each fix; engine/player builds pass. | **Implemented locally; runtime fence behavior still needs verification.** D3D12 and Vulkan now reject unsubmitted fence waits and cross-queue waits; focused API contract documented; engine/player build succeeds. |
| 3 | Upload/readback call-site audit | Find current callers of buffer update/read APIs; ensure invalid operations are observable and callers respect existing heap and fence requirements. Keep public signatures stable unless the audit proves the current contract cannot be used safely. Acceptance: callers are classified, unsafe paths are corrected, and focused existing checks/builds pass. | **Implemented locally:** the editor now keeps dynamic upload buffers per swap-chain image and waits before rewriting them; player skinning already uses fence-matched frame slots; readback paths wait for GPU completion. D3D12/Vulkan reject empty initial buffer data and log invalid updates. Engine, player and editor targets build; GPU runtime behavior remains unverified. |
| 4 | Existing backend resource-path parity | Compare current D3D12 and Vulkan buffer/texture update, transition, and teardown paths for behavior mismatches that affect existing workloads. Fix only reproducible parity bugs. Acceptance: affected existing backend checks and representative workloads agree on the documented behavior; record untested backends explicitly. | **Implemented locally; runtime parity unverified:** the D3D12 RHI now rejects null/zero/overflowing texture inputs like Vulkan, and both paths release upload staging after the upload fence completes. The direct D3D12 AssetManager path now has an explicit fence-based release method. A follow-up parity fix reserves bindless slot 0 for the white fallback on Vulkan as D3D12 does, with bounded allocation and graceful exhaustion handling in the RHI and current loaders. `aether_engine`, `aether_player`, `aether_editor`, and `aether_editor_qt` build; automated tests and GPU runtime checks were not run. |
| 5 | Closeout and compatibility notes | Summarize the defects fixed, remaining limitations, affected public behavior, and build/verification evidence. Keep performance claims out unless a repeatable before/after capture exists. | **Complete locally:** Vulkan's first real sampled texture now receives index 1 instead of 0, matching D3D12; callers must pass the returned handle and slot 0 always means white fallback. Both backends cap real textures at 31 and log/return an invalid handle on exhaustion. The engine/player/editor targets compile in the Visual Studio developer environment. Runtime GPU checks and automated tests remain open; Metal and other unsupported backend behavior was not evaluated. No performance claim is made. |
| 6 | Vulkan partial-allocation cleanup | Give existing staged texture and host-visible buffer creation scoped ownership so Vulkan create/allocate/bind/map/view failures and container growth failures release handles already acquired. Preserve exact-fence waits during normal uploads and avoid destroying resources if submitted work may still reference them. Acceptance: engine/player/editor builds pass; failure-injection or validation-layer evidence covers partial creation and submitted-upload recovery. | **Implemented locally; runtime failure paths unverified:** local owners clean partially built buffer, image, memory, view, and staging records; failed timeline waits attempt device-idle recovery before cleanup, retaining resources until device teardown if recovery itself fails. `aether_engine`, `aether_player`, `aether_editor`, and `aether_editor_qt` build. No failure injection, validation-layer run, or GPU runtime check was performed. |
| 7 | Vulkan swap-chain resize handle stability | Reuse device texture handles for surviving swap-chain images, reclaim removed slots on shrink/destruction, and reset frame/image indices after recreation so changed image counts cannot index stale semaphore slots. Acceptance: repeated resize preserves the live handle set and leaves acquire indices in range; engine/player/editor builds pass. | **Implemented locally; resize runtime behavior unverified:** window swap chains now update existing handles, register only additional images, unregister removed images and release all owned handles at destruction. Resize resets both rotating indices after rebuilding image and semaphore vectors. Invalid registry operations are logged and ignored. The four engine/player/editor targets build; repeated window/offscreen resize and validation-layer checks were not run. |
| 8 | Vulkan screenshot/readback cleanup | Scope-own the existing temporary readback buffer, memory, mapping, and command pool so allocation, recording, mapping, or output-vector failures do not leak resources. If a submitted readback cannot complete its queue wait, attempt device-idle recovery before releasing resources. Acceptance: engine/player/editor builds pass; screenshot/readback behavior and failure recovery are checked on an offscreen target. | **Implemented locally; runtime readback unverified:** readback now validates its current image index/handle and uses scoped buffer and command-pool owners. A submitted queue-wait failure gets a device-idle recovery attempt; resources are retained until device teardown if that recovery fails. The four engine/player/editor targets build. Offscreen screenshot and failure-path checks were not run. |
| 9 | Vulkan command-list construction cleanup | Keep the existing command pool under scoped ownership until its primary command buffer has been allocated, so an allocation failure during construction releases the pool even though the command-list destructor cannot run. Acceptance: engine/player/editor builds pass; failure injection or validation-layer evidence confirms partial-construction cleanup. | **Implemented locally; allocation-failure path unverified:** command-list construction now transfers pool ownership only after command-buffer allocation succeeds. |
| 10 | Vulkan pipeline construction cleanup | Scope-own the existing pipeline layout, shader modules, and graphics pipeline through shader compilation, pipeline creation and registry insertion, so any failure before registry ownership transfers releases completed Vulkan objects. Acceptance: engine/player/editor builds pass; injected shader/pipeline/registry failures or validation-layer evidence confirm cleanup. | **Implemented locally; failure paths unverified:** temporary shader modules and the layout/pipeline record now clean themselves up unless pipeline insertion succeeds. |

### Guardrails for this update

- Preserve the explicit synchronization model: callers submit work and wait on
  fences where needed; resource wrappers must not hide queue-wide waits.
- Do not treat move-only CPU ownership as proof that an in-flight GPU resource
  is safe to release. The next increment must verify submission and teardown
  ordering at the actual owners.
- Keep existing serialized formats and public API behavior stable. If a
  proposed fix needs a contract change, record the reason and migration impact
  before changing it.
- Build focused targets after implementation; distinguish compile evidence
  from runtime or automated-test evidence, and record checks that remain open.
- Make no changes to game content, game scripts, levels, or campaign assets as
  part of this queue.

## Deferred from M8

These may be considered in a later feature milestone, but are not M8 scope:

- New AI approaches such as GOAP, utility AI or state trees.
- A dedicated-server target, matchmaking, rollback netcode or lag compensation.
- Motion matching, new animation systems, animation LOD or new procedural tools.
- Virtual geometry, virtual texturing, temporal upscaling or new global
  illumination techniques.
- A new streaming subsystem, replication graph, memory-management subsystem or
  profiling product. Improve current loading, replication, allocation behavior
  and profiling where they already exist.
- New Android, iOS, console, XR or web targets.
- New gameplay framework concepts beyond hardening the current gameplay kits,
  save system and player runtime.

## Exit criteria

The [package reliability increment](M8_PACKAGE_RELIABILITY.md) records the
current targeted work on destination safety, cancellation, manifest validation,
rollback evidence and release-note completeness. Its checks contribute to M8.2
and M8.6 without marking the wider milestone complete.

M8 is complete when the existing supported feature set has a reviewed baseline,
the current platform matrix and sample workflows are reliable, prioritized
correctness and data-loss issues are resolved, performance changes are backed
by repeatable measurements, and the manual and known-limitations list match
verified behavior. Completion does not imply that deferred capabilities have
been implemented.
