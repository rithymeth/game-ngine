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

**Evidence so far (not passed).** `tools/api_inventory.py` regenerates the
machine inventory of installed C++ headers
([api_inventory_cpp.txt](api_inventory_cpp.txt)) and every Luau global, kit
table and handle method ([api_inventory_luau.txt](api_inventory_luau.txt)); the
Linux Physics CI lane and the `api_inventory` ctest fail on an unreviewed
difference. `tests/compat/luau` is a script compatibility corpus run by
`CompatCorpus_*` in `aether_tests`, next to the existing format corpus
(`tests/fixtures/legacy`) and C++ consumer (`samples/frozen_api_game`). Still
open: choosing the stable Core API 1.0 surface (all modules remain
experimental, so detection is a review gate, not a compatibility guarantee),
a corpus captured from older engine releases run on Windows and macOS lanes,
named owners for fuzz/stress/sanitizer failures, and tracked dispositions for
races and format breaks.

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

**Status (partial):** AETHER-01's mission is the cooked quest `Content/Gameplay/FirstContact.aquest`
(one flag objective per checkpoint stage; `MissionStage` is derived from Quests progress, and the
quest log is rebuilt from the `first-contact` save on reopen). Firing is the cooked
`FireWeapon.aability`, whose `AmmoCost.aeffect` spends a round, so an empty magazine fails with
`cannot_afford`. The game drives both from Luau through the public Quests, Abilities, Attributes and
SaveGames APIs; `games/AETHER-01` needs no engine source changes. Evidence:
`Aether01_CookedQuestAndFireAbilityUseThePublicKits` on the cooked pak, plus the existing Aether01
checkpoint and combat tests. Open: the new-developer steps and time are not recorded, per-weapon fire
cooldowns are still script timers, and kit-asset hot reload is not exercised (reopen is).

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
Build and Package) now includes importer-setting warnings with source paths.
It is groundwork for missing-content validation; it does not
pass this phase. A regression test now compares both archive and cook-manifest
bytes from two shipping cooks of the same project into separate output folders.
Incremental dependency rebuilds and broader toolchain/platform reproducibility
evidence remain open. Texture cooking now reuses cached output for unchanged
sources and settings; a focused regression confirms that changing one texture
recooks only that texture. This is a partial incremental-cook result, not the
full dependent-asset rebuild gate.

**Status: pass** (green on CI in PR #691 on Linux, macOS, Windows, Android and iOS). Fresh cooks
(no `Intermediate/`, from two separate copies) are now also byte-identical: sub-asset GUIDs missing from
committed `.ameta` files are derived, not random (`Cook_FreshCooksFromCleanCopiesAreByteIdentical`;
verified on CI in PR #695: the Linux GCC step "Content pipeline evidence (M8.5)" cooks two clean
copies and their paks and manifests are byte-identical). The Linux GCC step
"Content pipeline evidence (M8.5)" cooks AETHER-01 twice and checks the paks are byte-identical,
with no imports and no texture cooks the second time. It then edits `test_cube.bin` and checks that
exactly the 22 models that use it are re-imported. The step logs the sha256s with the git revision
and the gcc and cmake versions. Finally it runs the first pak headless in `aether_player` from an
empty directory, with the project sources moved away, and requires no errors and no kit-asset
warnings. The Windows package check also loads the packaged game with the source tree hidden.
`Cook_InvalidSourcesAreReportedByPathWithTheReason` covers importer validation. Corrupt or
truncated PNGs, malformed glTF, a glTF with a missing buffer, a WAV with no fmt or data chunk,
invalid prefab JSON and a missing `always_cook` path are each reported with their content path and
reason, the good content still cooks, and a strict cook writes no archive. glTF import errors now
carry the loader's first reason (`LoadGltf(path, scene, &error)`), and the cooker checks sound
container headers. Open: these additions have not run on CI yet. Kit assets (`.aquest`,
`.aability`, `.aeffect`, `.aitem`) are validated by the player when it loads them, not by the cooker.

## M8.6 — Animation & Character

Connect model and skeleton import, clips, graph/state machine, blends, IK, aim and foot offsets,
root motion, events, compression, retargeting and preview/editor tools. Kael's slice set is idle,
walk, run, sprint, jump, fall, shoot, reload, hit, death and interact. Animation events must be
stable through cook and save/load.

**Pass:** the complete move set plays in editor and packaged player, transitions and root motion
remain consistent at varied frame rates, and retargeting reports incompatible skeletons clearly.

**Evidence so far (not passed).**

- *Content.* Kael's production model (`Characters/Kael/Kael_Recon.gltf`) is rigid parts with no
  skin or clips, so the gate runs on a procedurally generated stand-in:
  `tools/generate_aether01_kael_rig.py` (stdlib, byte-reproducible) writes `Kael_Rig.gltf` (13
  bones, one skinned box per bone, embedded buffer) with the slice clips idle, walk, run, sprint,
  jump, fall, land, aim, fire (shoot), reload, hit, death and interact (walk/run/sprint carry root
  translation at 2/6/8 m/s), the graph `Kael.aanim` (one `Locomotion` machine, a montage slot),
  `Kael_ArchiveListen.amontage`, and `Characters/Warden/Warden_Rig.gltf` (other bone names, 1.35x
  proportions, A-pose arms, an extra jaw bone) as a retarget target. The rig is not drawn: the
  player still renders the greybox player model, so what is proven is the animation state and
  pose, not the skinned image.
- *Engine.* `anim::BuildCharacterFromGltf` builds a skeleton and clips from a loaded glTF scene;
  the player has an animation host (`Game::Animation()`, system `Player.Animation` after the
  sequencer) that loads `.aanim`/`.amontage`/rigs from the package by path, runs every `Animator`,
  dispatches its events to Blueprints and logs `Anim state: <entity> <machine>: <from> -> <to>`
  and montage start/end markers. `.aanim`/`.amontage`/`.ablend` are known asset types, so
  `always_cook` folders ship them. `AnimGraphInstance::Blend` reports a machine's crossfade
  weights. State crossfades now weight root motion (and notifies) by their average over the
  frame, which made blended root motion frame-rate independent (at 30 Hz it was 0.10 m off the
  144 Hz result over the walk/run/sprint segment, now 1.8 mm). `ValidateRetarget` reports
  incompatible skeletons with codes RT001-RT009 (invalid skeleton, unknown bone, nothing mapped,
  unmapped root, crossed hierarchy, missing required bone, two sources on one bone; warnings for
  rest-pose bones and unscaled roots); a root at the feet now scales translation by the hips'
  height. `CharacterMovement` exposes read-only `velocity`/`grounded` to scripts.
- *Game.* `ThirdPersonController.luau` feeds the graph through the Animator's public methods
  (Speed, Grounded, Aiming, Dead, Jump/Fire/Reload/Hit/Interact triggers); a new `Aim` action
  (right mouse, left trigger). It also keeps the frame's movement input for every physics step:
  at 30 Hz Kael previously moved 0.25 m where 60 and 144 Hz moved 6.2 m, because the second fixed
  step of a frame had no input.
- *Tests.* `Kael_GeneratedRigAndGraphAreValid`; `Kael_MoveSetTransitionsMatchAt30_60_144HzAndVariableDt`
  (14 s script through all 13 states at 30/60/144 Hz and a 30/60/40/120 Hz variable sequence:
  same 22 transitions, parameter-driven ones within the first frame after the change, clip-end
  exits after the authored state time within one frame, crossfade weights sum to 1 within 1e-5,
  poses within 2e-3 rad and root motion within 5 mm of 144 Hz at common times);
  `Kael_RetargetsOntoTheWardenRigWithBoundedError` (every clip, 21 samples each: bone turn error
  < 0.02 rad (measured 0.007), bone lengths kept to 1e-4 m, root moved 1.35x);
  `Kael_RetargetReportsIncompatibleSkeletonsClearly`; on the cooked pak
  `Aether01_KaelMoveSetPlaysInTheCookedGame` (input reaches idle, aim, interact, fire, reload,
  jump, fall, land, walk, run, hit, death and back to idle; no warnings),
  `Aether01_KaelLocomotionTransitionsAtVariedFrameRates` (30/60/144 Hz: same transitions within
  a frame plus a physics step, same distance) and `Aether01_ArchiveSequencePlaysKaelMontageAndSkipStopsIt`.
- *Packaged.* `tools/aether01_character_check.py` replays the move-set test's input
  (`AETHER01_MOVESET_TRACE`) in the standalone `aether_player --headless` on the M8.5 pak and
  requires every reachable state's marker, ending Death -> Idle (Linux GCC CI step "Packaged
  character move set (M8.6)"); the M8.7 cinematic check also requires Kael's montage markers.
  Both pass locally; not yet run on CI.
- *Open.* Sprint is in the graph but unreachable in game (run speed 6 m/s is the cap); no editor
  play-through of the move set has been recorded; the rig is generated, not Kael's production
  mesh, and is not rendered skinned in the player; IK, aim and foot offsets, compression and
  animation events through cook and save/load are not exercised by the slice; the retarget map
  is written in the test, not an asset.

## M8.7 — Audio & Cinematics

Complete sound effects, music, spatial attenuation, buses, mixing, reverb, occlusion, cues and
runtime parameters. Sequence tracks cover cameras, characters, animation, audio, VFX, dialogue,
events and subtitles, with pause and skip semantics. Localized subtitles and audio routing must
survive package cooking.

**Pass:** one AETHER-01 sequence runs, pauses and skips without losing gameplay state; its
dialogue, effects and subtitles are synchronized in the standalone build; mixer levels and
accessibility settings persist.
Sequence Animation tracks now play in the packaged player: the player's animation host (M8.6)
turns their keys into `Animator::PlayMontage`/`StopMontage` on the bound entity, and the Archive
reveal plays Kael's listening montage at 0.5 s and 5 s (`Aether01_ArchiveSequencePlaysKaelMontageAndSkipStopsIt`;
a skip ends it interrupted; the packaged cinematic check counts its markers).
The sequencer now exposes Skip on players and sequence components: it applies
the final pose, releases temporary spawns/camera cuts/fade, stops active audio
cues and animation montages through host callbacks, and reports a skipped
completion without firing intermediate cue keys. The packaged player now
handles audio sequence events and the AETHER-01 prototype includes an Archive
sequence.

**Evidence so far (not passed).**

- *Engine.* Sequences have a Subtitle track (`SubtitleKey`: time, duration,
  localization key, speaker; JSON `"type": "subtitle"`, validation SQ024/SQ025).
  What it shows is a pure function of the playhead, nested sequences included,
  and Stop/Skip clear it (`SequencePlayer::Subtitle`, `SequenceSystem::Subtitle`,
  `SequenceComponent::GetSubtitle`). The player translates the line through the
  cooked `.astrings` tables in the current language (`Game::Subtitle`,
  `SubtitleSpeaker`), draws it over the HUD or the game's UI, and hides it when
  the new `GameSettings::subtitles` accessibility setting is off. It logs markers:
  `Subtitle shown: <key>`, `Subtitle hidden`, `Sequence cue started/stopped: <cue>`,
  `Sequence finished/skipped: <sequence>`. Sequence audio keys now play even in
  games without Blueprints. `Game::SequenceCues` lists the 2D cues sequences are
  playing. The pause menu stops device audio output, so a paused cinematic's voice
  can't run ahead of its subtitle.
- *Content.* `Sequences/Archive.asequence` has three AETHER dialogue lines: Voice-bus
  cues `Audio/dialogue_archive_{1,2,3}.acue` (placeholder voices from
  `tools/generate_aether01_audio.py`) and a subtitle track keyed to
  `Text/Dialogue.astrings` (en, fr). `MissionPresentation.luau` skips the reveal on
  the new `SkipCinematic` action (Enter, gamepad Back) and records `ArchiveComplete`
  (1 played through, 2 skipped).
- *Tests (cooked pak, headless).* `Aether01_ArchiveDialogueSyncAtFixedRatesAndSkip`
  plays the reveal at fixed 30, 60 and 144 Hz frame steps. In each run, every line's
  voice and subtitle start on the same frame, within one frame after the authored
  time, and the subtitle hides within one frame after time + duration. The camera
  track moves the view. The test then skips at 2.9 s, 1.23 s and 5.71 s. Each skip
  lands on the next frame, leaves no dialogue voice, subtitle or archive cue
  playing, starts no later line (3 s more are checked), and ends at the archive
  ending (stage 5, `ArchiveTime` 8, `ArchiveComplete` 2, health kept).
  `Aether01_ArchiveSubtitlesLocalizedPausedAndVoiceBusPersists` checks the following:
  - The pause menu holds the playhead, subtitle and voice.
  - Master, music, SFX and voice volumes, the language (fr) and the subtitles
    setting survive a restart and reach the mixer buses.
  - The dialogue voice's gain follows Voice x Master (0.5 x 0.8 of the reference).
  - With subtitles off the voice plays and no line shows.
  - French subtitles come from the cooked table.

  The `SequenceSubtitle_*` tests cover the format and the player/system behaviour.
- *Packaged run.* `tools/aether01_cinematic_check.py` replays the recorded
  playthrough (`AETHER01_INPUT_TRACE`) with the standalone `aether_player
  --headless`, once to the end and once with Enter pressed during the first line,
  and checks the log markers and the `--report` state. The Linux GCC CI job runs it
  on the M8.5 standalone pak. The Windows packaged playthrough now also asserts the
  dialogue markers and `ArchiveComplete`. Both pass locally against a fresh
  development cook. Neither has run on CI yet.

**Still open.** Animation keys still have no host in the player, so no animation
plays from a sequence. VFX tracks are missing. The editor sequencer panel can't
author Subtitle tracks yet, though MCP `seq_sample` reports them. The dialogue
audio is a placeholder, not recorded voice. There is no localized voice-over
(only subtitles are localized). Accessibility covers the subtitle toggle only, with
no size or background options. The pause-menu audio stop only matters with device
output, so headless tests can't check it. These checks have not yet passed on CI.

## M8.8 — AI & World Systems

Build reusable Scout, Sentinel, Hunter and Warden archetypes with perception, blackboard,
behavior trees, navigation and combat. Add patrol, search, alert, cover, squad and boss behavior
where required by the slice. Expose decisions and navigation state in the editor debugger.

**Pass:** repeatable encounter scenarios exercise detection, loss of target, path rebuild,
checkpoint reload and boss phase transitions; no archetype depends on hard-coded mission paths.
Profile the scene with the planned enemy count.

### M8.8 profile (Release, scripted nav followers)

`Aether01_Profile_24NavFollowers` and `Aether01_Profile_NavFollowersCacheAndScaling`
(tests/test_aether01.cpp) run the AETHER-01 `profile` scenario headless: 480 warm-up frames,
240 timed baseline frames, then N clones of the Scout script (Luau AI + `Nav.MoveTowards`)
and 240 more timed frames. The path cache is switched off with
`AiDebugStore::SetPathCacheEnabled(false)` (C++ only, measurement hook; not script-visible),
which makes every `Nav.MoveTowards` call re-path. The tests print and assert only that every
follower moved; timings are never asserted.

Config: `CMAKE_BUILD_TYPE=Release`, Ninja, GCC, editor/sandbox/MCP/bench off, physics and
scripting on. Machine: CI-like Linux container, 4 vCPU Intel Xeon @ 2.80 GHz, shared host,
so single runs vary by roughly +/-0.1 ms. Six runs (including one inside the full suite);
"AI+nav" is loaded minus that run's baseline.

| Case | AI+nav ms/frame, median (range) |
|---|---|
| Baseline scene (no followers), absolute | 0.30 (0.26-0.49) |
| 24 followers, path cache on | 0.13 (0.06-0.18) |
| 24 followers, path cache off | 0.25 (0.22-0.43) |
| 48 followers, path cache on | 0.31 (0.25-0.56) |
| 48 followers, path cache off | 0.41 (0.27-0.61) |

Reading: at the planned 24 enemies, AI+nav is roughly 0.13 ms/frame (under 1% of a 16.6 ms
frame); the cache roughly halves it. Going to 48 costs about 2x-2.5x with the cache, so
scaling is close to linear. Debug numbers for the same 24-follower case were 0.43-0.53 ms.

### M8.8 editor AI debugger

Qt editor, Window menu -> **AI · DEBUGGER** (tabbed with the Content Browser; built when
scripting is on). During Play-in-Editor it lists every agent from the play runtime's
`ScriptSystem::AiDebug().Collect` (`EditorPlayRuntime::AiDebugSnapshot`): name, reported state
(`AI.SetDebugState`), perception target, known-actor count and path status (`corner i/n`,
`arrived`, `none`, `(partial)`). The table refreshes at about 4 Hz; selecting a row selects the
entity in the Hierarchy (and so the viewport and Inspector), and Hierarchy selection highlights
its row. The viewport overlay (drawn through the navigation debug-line source the viewport already
renders) shows the selected agent's path corners (walked dim, ahead amber), a line to its next
corner, crosses at the next corner and the goal (green: complete, orange: partial) and a red line
to its perception target; "Draw all agents" shows every agent. Rows and overlay lines are shaped
in `editor/src/ai/ai_debugger.{h,cpp}` (no Qt), tested headless by
`AiDebugger_RowsAndOverlayFromSnapshot` (tests/test_ai_editor.cpp). `game_ai_debug` (the same data
over MCP) is tested by `Mcp_GameAiDebugReportsStateAndPerception` (tests/test_mcp.cpp).

Status: not passed. Every pass criterion above has automated evidence (the four
`Aether01_Scenario_*` runs, AI/Nav-driven archetypes, the profile), and the debugger deliverable
now exists, but the Qt dock is compiled only by the Windows CI editor jobs (the Qt editor needs
Windows D3D12; it was not built locally) and has not been exercised by hand in Play-in-Editor.
Flip the gate after a green Windows editor CI run and a manual Play check.

## M8.9 — Multiplayer Production

Turn replication, prediction, reconciliation, RPC and sessions into a documented developer
workflow. Add dedicated-server packaging, server configuration, LAN/lobby discovery,
authentication hooks, interest management and a network profiler. Keep gameplay authority and
reconnect behavior explicit.

**Pass:** a clean machine can host a packaged server and connect clients; soak, packet-loss,
latency, reconnect and security checks meet recorded thresholds. A single-player slice does not
waive this gate if multiplayer ships as a supported 1.0 feature.

### M8.9 evidence so far (automatable part)

Status: not passed. Left before sign-off: a reviewed sign-off of the recorded thresholds below (and
measuring them against the packaged server, not only the loopback simulation), a decision on
transport encryption (currently out of scope: authenticated, not encrypted), and a cross-machine
(and NAT) run. Security/auth hardening landed (2026-10-24, see "Security hardening" below). LAN
discovery from the dedicated server, a network profiler capture, Windows/macOS server packages and
release publishing of the server packages landed 2026-10-24 (see "Dedicated server: discovery,
profiler, platforms, release" below); the Windows and macOS parts are verified only by CI.

`tests/test_net_production.cpp` (`NetProd_*`, in the default `aether_tests` run) drives a
server (`SessionHost` + `ReplicationServer` + `RpcRouter`) and four clients (`SessionClient` +
`ReplicationClient` + `RpcRouter`) over the existing seeded `LoopbackNetwork` (latency, jitter,
loss, duplication). The scenario: 12 NPCs circling at 3 m/s with one NPC despawned/respawned every
second, one owned pawn per player, and a replicated server-authoritative score each owner bumps by
reliable RPC every 0.5 s. Time is simulated at 60 Hz and every link is seeded, so the numbers below
are identical run to run (checked by hashing the output of three runs) and the tests assert against
the thresholds, not against wall-clock time.

Divergence is the worst client-vs-server position error while things move, after 2 s of warm-up.
Convergence is how long after motion stops until every client has exactly the server's entity set,
positions (within 1e-4 m) and scores. Bandwidth is server-to-client bytes per client per second.
At 3 m/s, 0.3 m of divergence means state is 100 ms stale, 1.5 m means 500 ms, 2.4 m means 800 ms.

| Check | Link | Threshold | Measured (Release, 2026-10-23) |
| --- | --- | --- | --- |
| Clean baseline, 30 s | none | divergence <= 0.3 m, converge <= 0.5 s, <= 16000 B/s/client | 0.200 m, 0.033 s, 11242 B/s |
| Soak, 300 s, 290+ churns | 50 ms + 0-20 ms jitter, 2% loss | divergence <= 1.5 m, converge <= 1.5 s, <= 16000 B/s; entity tables equal live set, < 64 datagrams in flight, < 8 unacked reliable per peer, 2400 RPCs applied exactly once | 0.696 m, 0.117 s, 12070 B/s; all bounds held |
| Packet loss 5%, 60 s | 20 ms, 5% loss | divergence <= 1.5 m, converge <= 1.0 s | 1.086 m, 0.100 s, 10796 B/s |
| Packet loss 20%, 60 s | 20 ms, 20% loss, 5% duplicates | divergence <= 2.4 m, converge <= 2.0 s | 1.086 m, 0.133 s, 10762 B/s |
| Latency/jitter, 60 s | 100 ms + 0-60 ms jitter (130 +/- 30), 2% loss, 1% duplicates | divergence <= 1.5 m, converge <= 1.5 s | 0.892 m, 0.267 s, 11201 B/s |
| Reconnect (hard drop) | 50 ms, 5% loss | server drops the dead peer <= 5.5 s; rejoin on the same address -> full exact state <= 2.0 s; pawn reused and owned again; its RPCs land | 5.067 s, 0.533 s; yes |
| Reconnect (instant restart, same address) | same | old peer replaced, full state <= 2.0 s | 0.367 s |
| Security: garbage | clean | 200 rounds of noise, empty, truncated, 64 KiB and every-kind framed junk from a stranger, plus junk data frames from a joined client's address: no crash, no stranger peer, the other clients stay exact | held |
| Security: spoofed/forged frames (M8.9 hardening) | clean | forged-payload, re-tagged, guessed-tag data frames and a spoofed goodbye from a joined client's address, and a tagless "server" goodbye to a client: all dropped by the MAC, session kept, everyone exact | held (4 server-side, 1 client-side drops) |
| Security: handshake | clean | ConnectRequest 69 B, every pre-verification reply <= request (29 B), short requests unanswered; no peer before a valid cookie; cookie from another address, forged, reused after leave, or older than 10 s refused | held |
| Security: join tokens | clean | valid token joins (account recorded); expired -> TokenExpired; other issuer, other build, missing -> BadToken; token + wrong password -> WrongPassword | held |
| Security: rate limits | clean | 120000 handshake datagrams in 1 s from one address -> <= burst + rate cookies; 30000 junk data frames in 1 s capped at the packet budget, the other 3 clients exact; 64 sources vs a 16-entry table stays <= 16 | 11 cookies; held |
| Security: replay and authority | clean | a recorded RPC's datagrams replayed 3x from the owner and once from another address run it once; a non-owner (hacked `locally_owned`) is rejected; a wrong password is refused; a raw peer that skips the session gets no authority and is dropped after the join timeout | held |

What this does not cover, so the gate stays open:

- **Dedicated server: built, packaged and smoke-tested on Linux only.** `aether_server`
  (`tools/server/`, [manual](../manual/15-dedicated-server.md)) mounts a cooked game in a headless
  `player::Game` and hosts it with `SessionHost` + `ReplicationServer` + `RpcRouter` over real UDP
  at a fixed tick, configured by `server.json` and the command line (port, max players, password /
  `AETHER_SERVER_PASSWORD`, build id, scene, tick rate, log file, `--max-runtime`,
  `--quit-when-empty`), with clean SIGINT/SIGTERM shutdown and exit codes. `aether_netclient`
  joins headless, receives state, sends a server RPC and prints a JSON summary.
  `tools/package_server.sh` makes `aether-server-linux-x64.tar.gz` + `SHA256SUMS.txt` +
  `SERVER_PACKAGE.json`; the CI `dedicated-server` job unpacks it in a bare `ubuntu:24.04`
  container and runs `tools/server_smoke.sh` (two clients join, see the 43 replicated AETHER-01
  entities and get their RPC acked over localhost UDP; a wrong password is refused and gets no
  state). `DedicatedServer_*` in `aether_tests` cover config parsing and an in-process real-UDP
  run (two joins, a wrong password, a wrong build). Local Release run, 2026-10-23: both clients
  joined in ~0.08 s, 43 entities, 46 snapshots, RPC ack in ~0.07 s, 3.5 s wall time end to end.
  Still open for this item: the smoke runs on one host (localhost), not across two machines or a
  NAT; the soak/loss/latency thresholds above are measured on `LoopbackNetwork`, not against the
  packaged server. (Windows/macOS packages, release publishing and LAN discovery: see below.)
- **Authentication is a session password plus entity ownership.** `RpcRouter` gates server calls
  by ownership only; it does not know whether a peer has joined the session (a connected,
  not-joined peer owns nothing, so it is rejected, and `SessionHost` drops it after
  `join_timeout`). The dedicated server additionally hides every entity from peers that have not
  joined (a `ReplicationServer` relevancy rule); the bare `ReplicationServer` still sends state to
  any connected peer. Join tokens, a stateless cookie handshake, per-datagram SipHash-2-4 MACs and
  per-address rate limits now exist (below); traffic is still not encrypted.
- **Security hardening (2026-10-24).** `NetHost` connects with a stateless challenge (cookie =
  SipHash-2-4(server secret, address, client nonce, time); requests padded to 69 B, no reply
  before verification bigger than the request; one use per cookie, 10 s lifetime), derives
  per-direction session keys from the secret, both nonces and the address, and tags every
  Data/Disconnect datagram (8 B). Unauthenticated frames are dropped and counted
  (`HostSecurityStats`). Per-address token buckets cap handshakes, datagrams and bytes in a table
  bounded at 4096 addresses. `SessionHost::SetJoinTokens` checks signed join tokens (player id,
  expiry, build id; `aet1.` text form); `aether_server` takes `token_secret` / `require_token` /
  `--token-secret` / `AETHER_SERVER_TOKEN_SECRET` and `--issue-token`, `aether_netclient` takes
  `--token`. SipHash-2-4 is written from the reference implementation and checked against its
  published vectors (`NetProd_SipHashTestVectors`). Tests: `NetProd_Security*`,
  `DedicatedServer_RealUdpJoinTokens`; `tools/server_smoke.sh` now runs with tokens required.
  The existing garbage test used to frame its junk with a big-endian protocol id, so it never
  reached the data path; it is fixed and now also requires the targeted client to stay exact.
  Recorded numbers after the change (Release): clean 0.200 m / 0.017 s / 11242 B/s payload
  (12022 B/s on the wire with header + MAC); soak 0.844 m / 0.100 s / 12073 (12661); loss5
  0.892 m / 0.100 s; loss20 1.086 m / 0.150 s; latency 0.892 m / 0.183 s; reconnect drop 5.050 s,
  rejoin 0.850 s, same-address restart 0.650 s (the handshake is now two round trips). All
  thresholds unchanged; the bandwidth check now also applies to wire bytes. Not done: encryption
  (an on-path observer can read the keys in the Accept), token revocation, per-IP (rather than
  per-ip:port) limits; the wire protocol is incompatible with older builds.
- **Dedicated server: discovery, profiler, platforms, release (2026-10-24).**
  - *LAN discovery.* `aether_server` answers LAN discovery on `lan_port` (default 7778,
    `lan_advertise` default on; a taken port is a warning) with name, players (kept current), max
    players, build, game port, password flag and a `requires_token` property.
    `aether_netclient --discover [--timeout s] [--lan-port n] [--query ip:port]` broadcasts and also
    queries 127.0.0.1, prints `AETHER_DISCOVER {...}` JSON, and `--join-first` joins the first
    compatible server. `LanBeacon` now follows the #687 anti-amplification rules: queries are padded
    to 1024 B and no answer is larger than its query (16 B legacy queries get none, counted as
    oversized); 4 answers/s, burst 8, per source IP in a table bounded at 1024. Tests:
    `Session_LanBeaconAntiAmplification` (unpadded query unanswered; answer <= query; a 200-query
    flood from one address answered at most 8 times while another address is still answered),
    `DedicatedServer_RealUdpLanDiscoveryAndProfile` (real UDP: broadcast + 127.0.0.1, advert fields,
    another build listed incompatible, a client joins the discovered address, the advert's player
    count follows), and `tools/server_smoke.sh` (discovers the packaged server before anyone joins;
    bravo joins by `--discover --join-first`).
  - *Network profiler capture.* `aether_server --profile <file.jsonl> [--profile-interval s]`
    writes per-peer RTT, loss, bytes/packets each way, message bytes per channel (new
    `ConnectionStats::channel_bytes_*`), snapshots and snapshot bytes per interval (from
    `NetProfile`), `HostSecurityStats` (rate_limited, bad_cookies, replays, bad_macs, malformed) and
    LAN discovery counters, then a summary line (also printed as `AETHER_SERVER_PROFILE`). The CI
    `dedicated-server` job uploads it as the `dedicated-server-profile` artifact (also
    `-windows` / `-macos`). Sample capture, local Release packaged smoke, 2026-10-24 (localhost,
    2 token clients for 3 s + 2 refused, 30 Hz, 43 replicated AETHER-01 entities): 11 records at
    0.5 s, peak 2 peers, RTT mean 33.7 ms / max 35.3 ms (on localhost this is the 30 Hz tick
    batching, not the network), loss max 0, 92 snapshots averaging 96.9 B (largest interval
    average 462 B: the initial spawns), 10915 B sent / 3088 B received, rate_limited 0, bad_macs 0,
    16 LAN discovery answers, 0 rate-limited.
  - *Platforms.* `tools/package_server.sh` now runs on Linux, macOS and Windows Git Bash (archives
    written by Python: reproducible `.tar.gz`, or `.zip` on Windows; checked byte-identical across
    two local runs) and makes `aether-server-windows-x64.zip` / `aether-server-macos-arm64.tar.gz`.
    The `windows` (MSVC) and `macos` (Apple clang) CI jobs package their already-built
    `aether_server`/`aether_netclient` with the AETHER-01 cook and run the same
    `tools/server_smoke.sh` natively (discovery, 2 clients with tokens, refusals, profile). Windows
    adds `SetConsoleCtrlHandler` (Ctrl+C/Break, console close) to the existing signal handling.
    Only CI exercises these two platforms; no Windows or macOS run was made locally.
  - *Release.* The `release` job (now also needing `dedicated-server`) downloads the three server
    artifacts, checks each against its own SHA256SUMS, appends the archives and their
    `SERVER_PACKAGE.json` (as `aether-server-<platform>.json`) to the release `SHA256SUMS.txt`,
    re-checks every archive against it, attests them with the other assets, and publishes them.
    Not yet exercised by a tagged release.
- Interest management is exercised by the replication tests, not at production scale; the soak
  is 4 clients and 16 entities.

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

**Evidence so far (not passed).** `tools/ship/aether_ship.py` packages
AETHER-01 for Linux. It writes `GAME_MANIFEST.json` (version, content
`build_id`, git revision, engine/minimum engine version, CMake toolchain,
`DT_NEEDED` runtime libraries, licenses, pak mount order, per-file size and
SHA-256, symbols with GNU build-ids), a stripped player, a build-id keyed
`objcopy --only-keep-debug` symbols archive, a release manifest and
`SHA256SUMS.txt`. Packaging is byte-identical for the same build and cook.
`tools/ship/build_release.sh` builds v1 (a strict shipping cook), v2 (v1 plus an
`aether_cook --patch-of` delta pak whose player starts at Health 5, not 3) and
the v1 → v2 patch. The patch holds two files and is about 9 KB. `apply`
verifies the checksum, the installed manifest, the patch base `build_id`, the
payload hashes and the minimum engine version. It stages a copy, verifies it,
switches with two renames and keeps `<dir>.previous`. `rollback` swaps the two
folders. Interrupted switches are recovered, and files the manifest doesn't
list (saves) are carried over. The CI `game-ship` job runs
`tools/ship/ship_smoke.sh` in bare `ubuntu:24.04` with only `python3` and
`libvulkan1`. The job:

- checks the SHA256SUMS, every installed file and the stripped player's build-id
  against the symbol file;
- launches v1 headless outside the checkout (`--report` Health 3.0);
- refuses a damaged and a tampered patch, leaving v1 intact;
- patches to v2 (Health 5.0), rolls back to v1 (Health 3.0) and rolls forward
  again, with the user-dir save and an in-install `Saved/` file kept throughout;
- runs `tools/ship/assemble_release.sh` as a release dry run that publishes
  nothing.

On a `v*` tag, the Release job adds the package, symbols and manifest to
`SHA256SUMS.txt`, the provenance attestation and the published assets. The
tests are 29 `ship_tool_tests` unit tests (ctest).

Still open:

- CI has not run this yet, and no tag has exercised it.
- Fresh-cook reproducibility (fixed, verified on CI in PR #695): sub-asset GUIDs that a
  committed `.ameta` doesn't list yet (the Kael/Warden rigs) were random on
  each import, so two cooks without `Intermediate/` differed. They are now
  derived from the parent GUID + importer + key (`DeriveSubAssetGuid`);
  persisted GUIDs are kept. The Linux GCC step "Content pipeline evidence
  (M8.5)" cooks AETHER-01 from two clean copies; both cooks are
  byte-identical (pak and manifest sha256). This item is closed.
- Windows (code and CI wiring added, not yet run on CI): `aether_ship.py
  --platform windows-x64` writes a `.zip` package (`AETHER-01.exe` + DLLs),
  a symbol-store `.pdb` zip whose `SYMBOLS.json` maps each binary to its PDB
  GUID/age (exe RSDS record vs. PDB info/DBI streams, parsed in pure Python),
  and a `.zip` patch. Folders are switched with `os.replace`, and files in use
  fail with a clear error. The MSVC job (`-DAETHER_MSVC_RELEASE_PDB=ON`) runs
  `ship_smoke.sh` natively under Git Bash, and the Release job adds
  `AETHER-01-windows-x64-ship.{zip,-symbols.zip,manifest.json}` to SHA256SUMS
  and the attestation. The PE/PDB parsers are tested only on synthetic
  fixtures until that CI run.
- No macOS game package.
- No Authenticode or other signing of the game package beyond the Sigstore
  attestation.
- The sample patch is a CI fixture. A real upgrade from the prior tagged
  candidate has not been recorded.
- Profiles are only shipping (the game) and dedicated server. There is no
  `aether_build` entry point.

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
