# M8: Production platform and AETHER-01

This proposed twelve-phase product validation track for the Aether 1.0 release uses the engine work in
[M7, Phases 37–48](UPGRADE_PLAN.md) to produce a game that can be built, played, updated and
debugged outside the source tree. It is distinct from the current
[improvement-only M8 plan](M8_PRODUCTION_TECHNOLOGY.md), whose M8.1–M8.7 numbering
describes a different scope. The M7 Phase 48 release gate remains authoritative;
the proposal's M8.12 would be part of a later product decision:
neither a feature list nor a playable prototype alone authorizes a 1.0 release.

This plan covers all twelve M8 phases from the supplied proposal. It is a scope and acceptance
plan, not a claim that the work is finished. The checkout now includes a
greybox AETHER-01 prototype under `games/AETHER-01` alongside the samples.
Its project source and proof-release records do not establish a polished
15–30 minute slice or the broader product gates.
The external GitHub link in the proposal names another repository and is not evidence for this one.

## Order and evidence

Editor workflows, the vertical slice and its content pipeline come first. Renderer, character,
audio and AI work should be driven by the slice's actual scenes. Core stability and Build & Ship
run alongside them. Multiplayer production remains a separate gate even if the first AETHER-01
slice is single player.

| Phase | Outcome | Builds on | Required evidence |
|---|---|---|---|
| M8.1 Core stabilization | Versioned Core API 1.0 | M7 37, 47, 48.2–48.3 | API inventory, compatibility corpus and CI matrix |
| M8.2 Rendering 2.0 | Finished game frame on target backends | M7 38–39 | Captures, image comparisons, frame and memory budgets |
| M8.3 Editor 2.0 | Complete first-game authoring path | M7 43 | Clean-machine task study and editor recovery results |
| M8.4 Gameplay Framework 2.0 | Reusable game architecture | M7 37, Phase 30 | AETHER-01 systems built through public APIs |
| M8.5 Asset & Content Pipeline | Deterministic, validated packages | Phases 8, 25; M7 47 | Repeat cook hashes, incremental cook and missing-asset checks |
| M8.6 Animation & Character | Production character path | Phases 16, 31 | Kael move set and import/retarget playback checks |
| M8.7 Audio & Cinematics | Finished presentation path | Phases 17, 27, 29 | Mixed, localized and skippable sequence in package |
| M8.8 AI & World Systems | Reusable enemy and boss archetypes | Phases 20–21; M7 40, 42 | Scenario and navigation regression runs |
| M8.9 Multiplayer Production | Supported client/server workflow | Phase 22; M7 41 | Dedicated-server package, soak and reconnect evidence |
| M8.10 Build & Ship | Repeatable release candidate | Phases 24–25; M7 46–48 | Install, launch, patch and rollback from clean machines |
| M8.11 AETHER-01 vertical slice | Polished 15–30 minute game | M8.2–M8.8, M8.10 | Complete playthrough and packaged build |
| M8.12 Aether 1.0 gate | Release decision | M8.1–M8.11; M7 48 | External developer task study and signed gate report |

Statuses are tracked in [m8_gates.json](m8_gates.json). `n/a yet` means evidence has not been
collected; it never counts as a pass. CTest validates the M8 table format on ordinary builds.
At release time, the candidate must pass both tables in strict mode:

```sh
python3 tools/gate.py --table docs/design/m8_gates.json --require-complete
python3 tools/gate.py --table docs/design/production_gates.json --require-complete
```

## M8.1 — Engine Core Stabilization

Inventory the public ECS, job, logging, error, serialization, reflection and plugin APIs. Mark
each stable, experimental or internal; publish ownership and threading contracts. Stable APIs
need versioned migration or deprecation before a breaking change. Audit hot-path allocations,
race-prone callbacks and serialized formats. Consolidate CMake targets and run GCC, Clang and
MSVC across Linux and Windows, adding macOS when its supported runner is available. CI needs
unit, integration, functional, fuzz, stress and sanitizer lanes with named owners for failures.

**Deliverable:** Core API 1.0 reference and compatibility policy. **Pass:** a previous-project
corpus loads, stable API changes are detected, supported build lanes pass, and every unresolved
race or format break has a tracked disposition. M7 37.3 annotations and 48.3 freeze are inputs,
not substitutes for this evidence.
The [Core API inventory](../manual/core_api_inventory.md) is a first contract
record. It leaves every module experimental and lists compatibility work still
needed for the 1.0 gate.

## M8.2 — Rendering 2.0

Ship a coherent D3D12 and Vulkan path for PBR, IBL, HDR, exposure, tone mapping, bloom, SSAO,
SSR, TAA/FXAA, decals and improved directional, point and spot shadows. Complete instancing,
LOD, culling, material quality levels and GPU particle integration. GPU-driven drawing and
virtualized geometry are performance options with fallbacks; Metal is a later backend unless a
supported Mac release is in scope. The editor should expose material, lighting, environment and
post-process choices without source edits.

**Pass:** capture the same representative AETHER-01 scene on each supported backend; compare
output with approved images, record GPU/CPU frame times and memory on named hardware, and meet
the project frame budget. Experimental paths cannot be the only way to meet it.

## M8.3 — Editor 2.0

Finish the World, Blueprint, Script, Animation, Material, AI, Audio, UI, 2D, Sequence, Network,
Profiler, Debugger and Build & Ship workspaces around shared selection, Undo/Redo, search,
drag-and-drop, notifications, diagnostics, shortcuts, layouts and project management. A complete
workflow is: create project → import content → assemble a scene → author logic → play/debug →
cook/package. Resolve the desktop shell choice through
[the UI toolkit ADR](EDITOR_UI_TOOLKIT_ADR.md) before committing to a Qt 6 migration.

**Pass:** a developer unfamiliar with Aether completes that workflow on a clean machine using
the editor; edits survive restart and crash recovery; screenshot and keyboard-access checks pass
on supported desktop platforms. This extends M7 43 rather than creating a second editor shell.

## M8.4 — Gameplay Framework 2.0

Define public GameMode, GameState, Player, Controller and Character roles around components,
input actions, tags, abilities, attributes, effects, Inventory, Interaction, Dialogue, Quests,
Save and Checkpoints. Reuse the Phase 30 kits and M7 kit registry. Provide project templates and
data-driven examples so game rules do not require editing engine internals.

**Pass:** implement one AETHER-01 checkpoint, combat ability and quest objective through the
documented framework; hot reload or reopen the project; cook the result with no engine fork.
Record the steps and time needed by a new developer.

## M8.5 — Asset & Content Pipeline

Support the project's required FBX, glTF, PNG/JPG/HDR and WAV/OGG/MP3 imports with explicit
settings and validation. Retain GUIDs and dependency graphs through reimport and moves. Add
incremental cooking, DDC/cook caches, platform texture variants, compression, and distinct
development and shipping manifests. The cooker must report the source path and actionable reason
for missing or incompatible content.

**Pass:** the same source, settings and engine revision produce byte-identical cooked output on
repeat runs; an unchanged incremental cook does no work; a changed dependency rebuilds only its
dependents; the `.apak` loads in a standalone player. Record hashes and toolchain revisions.
The opt-in strict cook (`aether_cook --strict`, or **Fail on cook warnings** in
Build and Package) is groundwork for missing-content validation; it does not
pass this phase. A regression test now compares both archive and cook-manifest
bytes from two shipping cooks of the same project into separate output folders.
Incremental dependency rebuilds and broader toolchain/platform reproducibility
evidence remain open.

## M8.6 — Animation & Character

Connect model and skeleton import, clips, graph/state machine, blends, IK, aim and foot offsets,
root motion, events, compression, retargeting and preview/editor tools. Kael's slice set is idle,
walk, run, sprint, jump, fall, shoot, reload, hit, death and interact. Animation events must be
stable through cook and save/load.

**Pass:** the complete move set plays in editor and packaged player, transitions and root motion
remain consistent at varied frame rates, and retargeting reports incompatible skeletons clearly.

## M8.7 — Audio & Cinematics

Complete sound effects, music, spatial attenuation, buses, mixing, reverb, occlusion, cues and
runtime parameters. Sequence tracks cover cameras, characters, animation, audio, VFX, dialogue,
events and subtitles, with pause and skip semantics. Localized subtitles and audio routing must
survive package cooking.

**Pass:** one AETHER-01 sequence runs, pauses and skips without losing gameplay state; its
dialogue, effects and subtitles are synchronized in the standalone build; mixer levels and
accessibility settings persist.
The sequencer now exposes Skip on players and sequence components: it applies
the final pose, releases temporary spawns/camera cuts/fade, stops active audio
cues and animation montages through host callbacks, and reports a skipped
completion without firing intermediate cue keys. The packaged player now
handles audio sequence events and the AETHER-01 prototype includes an Archive
sequence; animation playback, localized dialogue/subtitles and a packaged
skip/playthrough acceptance run remain open.

## M8.8 — AI & World Systems

Build reusable Scout, Sentinel, Hunter and Warden archetypes with perception, blackboard,
behavior trees, navigation and combat. Add patrol, search, alert, cover, squad and boss behavior
where required by the slice. Expose decisions and navigation state in the editor debugger.

**Pass:** repeatable encounter scenarios exercise detection, loss of target, path rebuild,
checkpoint reload and boss phase transitions; no archetype depends on hard-coded mission paths.
Profile the scene with the planned enemy count.

## M8.9 — Multiplayer Production

Turn replication, prediction, reconciliation, RPC and sessions into a documented developer
workflow. Add dedicated-server packaging, server configuration, LAN/lobby discovery,
authentication hooks, interest management and a network profiler. Keep gameplay authority and
reconnect behavior explicit.

**Pass:** a clean machine can host a packaged server and connect clients; soak, packet-loss,
latency, reconnect and security checks meet recorded thresholds. A single-player slice does not
waive this gate if multiplayer ships as a supported 1.0 feature.

## M8.10 — Build & Ship

Create one documented Validate → Cook → Package → Test → Release path across development, debug,
test, release, shipping and dedicated-server profiles. `aether_editor`, `aether_cook`,
`aether_pak`, `aether_player`, `aether_server` and `aether_build` should have clear entry points,
even if some are wrappers around existing targets. Start with the supported Windows release,
retain Linux CI, then add macOS when its build and signing lane is ready. Include manifests,
checksums, symbols, license notices, signing, patching and rollback.

**Pass:** a clean machine builds an installable release candidate from a tag; the package runs
outside the checkout, upgrades from the prior candidate, and can roll back without losing saves.
The same manifest records binaries, content, toolchain and dependencies.
`PackageManifest.json` and `aether_pak verify-package` now provide file size and
CRC-32 checks for the staged player and content. Package installation now
backs up and restores the previous artifacts if a later install step fails.
Toolchain provenance,
cryptographic signatures, install/upgrade and rollback evidence remain open.

## M8.11 — AETHER-01 Vertical Slice

The target is a polished 15–30 minute slice, not a complete 3–5 hour game. Its critical path is
Title → Cryo Bay → Aegis Rifle → Scout → Service Route → Sentinel → Hunter → Warden → Archive →
Ending. Add final-quality representative environment and character art, animation, lighting,
VFX, sound/music, UI, cinematics, checkpoints, save/load and settings. Keep encounter and mission
logic as reusable game assets and framework code.

**Pass:** a new player completes that path in a packaged build without developer intervention;
checkpoint restore and settings survive restart; missing assets are zero; capture frame-time,
crash and playtest results. The checked-in greybox prototype is an input to this work, not
evidence that the full slice has passed.

## M8.12 — Aether 1.0 Production Gate

Ask an external developer to create a project, import assets, build a level, author gameplay,
UI, materials, animation, AI and audio, add networking, save/load, cook, package, run outside the
source tree, debug, profile and prepare a release. Collect a recording or task log for each step.
The engine gate covers stable APIs, supported platforms, performance, QA, packaging and SDK;
the game gate covers the AETHER-01 slice's playthrough, polish, content and stability.

**Pass:** both gates and the existing [Phase 48 production gate](../ROADMAP.md#the-aether-10-production-gate)
are green on the same tagged release candidate. Publish a signed report listing evidence,
supported platform/backend versions and remaining exclusions before calling it Aether 1.0.
