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
