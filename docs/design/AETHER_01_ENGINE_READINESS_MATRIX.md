# AETHER-01 Engine Readiness Matrix

## Assessment record

- **Assessment point:** 2026-10-09, current upstream master f9f903231618c032a9494bccb3449a2fe0e2b0e4.
- **Released baseline:** Aether Engine v0.27.2, commit cc294945d453dc7a03a7b8e0ab3efd1e8bc4d0ee.
- **Current change validation:** PR #186 head 5a36d31581612f029d5ececf6dd2fcdc5a37a131 passed [CI run 37927046532](https://github.com/rithymeth/game-ngine/actions/runs/37927046532). The Qt-enabled Windows build, unit tests, Qt editor self-test, functional tests, AETHER-01 cook/launch/render, and standalone package checks passed. The Release job was skipped.
- **Requirements source:** the 112-section AETHER-01 Game Design Document supplied by the project owner. GDD references below use its section numbers.
- **Workload and release evidence:** [AETHER-01 Proof Plan](AETHER_01_PROOF_PLAN.md), [M8 Technology Plan](M8_PRODUCTION_TECHNOLOGY.md), and [master CI run 37872212846](https://github.com/rithymeth/game-ngine/actions/runs/37872212846).
- **Status rule:** use the definitions in [Engine Foundation Plan](ENGINE_FOUNDATION_PLAN.md#capability-status-rules). A passing build alone does not prove an authoring or packaged-game workflow.

The latest master CI run passed Linux GCC, Linux Clang, Linux Physics, Linux
ASan/UBSan, Linux TSan, fuzz, Windows MSVC/D3D12, and macOS Apple Silicon
MoltenVK jobs. Its Windows job also passed unit and functional checks, the
AETHER-01 cook and launch, scene rendering checks, editor packaging, and
source-independent proof-game packaging and launch. The separate Release job
was skipped; clang-tidy was skipped as advisory. These checks did not validate
a tagged package, and this evidence does not include a performance run on the
GDD's minimum or recommended hardware.

The follow-up change in PR #186 also passed the platform matrix. In addition,
its Windows job built the optional Qt editor and passed the Qt editor
self-test, which checks the runtime component round-trip. The unit suite
passed the unknown-component rejection case. This still does not replace the
full AETHER-01 scene comparison or clean-workstation authoring run.

The current configured build matrix is broader than the game's declared
shipping target. **The only game target established here is Windows x64 with
MSVC and D3D12.** Linux GCC/Clang and macOS MoltenVK are CI validation targets;
this matrix does not certify those as AETHER-01 shipping platforms. The Qt
editor remains an opt-in, Windows-only spike (AETHER_BUILD_QT_EDITOR is OFF by
default); its documented local toolchain is Qt 6.8.3 with MSVC 2022. The
minimum supported Windows version, Windows SDK/MSVC minimum, GPU vendor
coverage, and Qt distribution/licensing decision still need a signed-off
product policy.

## Scope decisions

- The full five-act, 3–5 hour game in GDD sections 3 and 64–70 is **full
  production**. It is not a prerequisite for this engine proof.
- GDD section 106 defines the reduced First Contact MVP as one environment,
  one character, three weapons, three enemy types, one boss, and one story,
  with 60–90 minutes as its target. The first engine-readiness run is narrower:
  one complete connected level and one representative combat encounter, as
  defined by the proof plan. Playtime is not an engine pass condition.
- The vertical-slice runtime target is Windows x64, single-player, with the
  AEGIS Rifle, ARC Pistol, VOLT Shotgun, Scout, Sentinel, Hunter, and a
  readable Warden encounter. The first encounter may be a reduced content
  workload, but it must exercise the same required engine workflows.
- Networking, dedicated servers, MMO, console, VR, mobile, marketplace, and
  mod-SDK work are deferred or out of scope under GDD section 78.
- Story, encounter tuning, weapon data, objective design, and other
  AETHER-01-specific behavior stay game-owned when supported engine APIs can
  express them. An engine gap needs a concrete case showing why game-side
  code cannot safely meet the requirement.

## Vertical-slice requirements

Each row records the owner, current evidence, remaining limit, and the pass
condition. Scope labels are VS (vertical slice), FP (full production), and D
(deferred). Priorities are P0 (data or delivery blocker), P1 (vertical-slice
readiness), P2 (full-production gate), and P3 (deferred).

### Project authoring and content

| ID and GDD source | Scope · owner · status · priority | Dependencies | Acceptance condition and evidence or known limit |
|---|---|---|---|
| A01. Create/open a project, persist project settings, and reopen it | VS · Engine · Partial · P1 | Project serializer, editor host, project paths | On a clean Windows workstation, create and open the proof project, change startup scene and project settings, close and reopen, and compare values. Qt project/settings actions exist in the [Qt coverage ledger](QT_EDITOR_FEATURE_COVERAGE.md); the host is still experimental and the full clean-workstation workflow is not certified. |
| A02. Compose and inspect scenes; preserve all components, entity IDs, and assets through load/edit/save/reopen | VS · Engine · Partial · P0 | Scene serializer, component registry, project modules, editor document | Round-trip the AETHER-01 scene through each supported editor host and compare every entity GUID and serialized component before and after editing one transform. PR #186 registers the Qt host's audio, physics, gameplay, and sequence schemas, starts enabled project modules before the startup scene, and rejects unknown components in editor scene loads. CI run 37927046532 passed the Qt editor self-test and unknown-component regression case. A full AETHER-01 scene comparison remains open; older master skipped unknown plugin components during load. |
| A03. Undo, redo, play/stop restore, and recover interrupted authoring | VS · Engine · Partial · P1 | Command stack, scene snapshot, editor document recovery | Verify entity/transform edits, undo and redo, Play/Pause/Stop restore, then terminate during an unsaved edit and recover without overwriting the last saved scene. Command and play-session operations are covered by the [Qt coverage ledger](QT_EDITOR_FEATURE_COVERAGE.md); crash/interruption recovery remains unproven. |
| A04. Keep project, scene, save, settings, and asset formats compatible across engine updates | VS + FP · Engine · Partial · P0 | Format versions, migration hooks, fixtures, backup policy | Load representative v0.27.2 projects and saves in the candidate, apply migrations, save, reopen, and compare semantic state; reject unsupported versions without modifying source data. Scene files have format versions and reflection supports field migration, but a cross-format old-project fixture audit and recovery evidence are not recorded here. |
| A05. Import and reimport static meshes, textures, and materials with useful diagnostics | VS · Engine · Partial · P1 | Importers, asset database, dependency tracking, cooker | Import/reimport the representative environment and weapon assets on a clean workstation, verify stable identities and dependencies, and require actionable failures for malformed inputs. CI proves the current static glTF/base-color path can cook and render; the complete artist reimport and error-repair workflow is not proven. |
| A06. Import and use skeletal characters, animation clips, graphs, and IK | VS · Engine · Unverified · P1 | Skeletal importer, animation graph, renderer, IK, editor tools | Import a representative Kael rig and enemy rig, set up a graph and weapon pose, cook them, and verify animation transitions and IK in the packaged scene. The Qt viewport ledger explicitly leaves skeletal deformation and animation authoring unverified; static-mesh rendering does not cover this path. |
| A07. Import and package sound, cues, and music | VS · Engine · Partial · P1 | Audio import, cue registry, mixer, cook dependencies | Import representative weapon, ambience, and narrative audio; play spatial and 2D cues, verify mix controls, and repeat after removing source content. The proof plan records cooked cue playback and source-free packaging work; final spatial mix, voice delivery, and music behavior still need a clean packaged acceptance run. |
| A08. Author and package combat and environment effects | VS · Engine · Unverified · P1 | Existing VFX tools, materials, renderer, cook pipeline | Create and tune hit, muzzle, warning, and environment effects in the existing authoring workflow; verify timing, visibility, and performance in the packaged level. The editor migration ledger lists the VFX editor among tools not yet exposed in Qt, and no repeatable packaged effect acceptance evidence is recorded. |
| A09. Edit Luau and Blueprint gameplay, iterate, and diagnose intentional script errors | VS · Engine · Partial · P1 | Script/Blueprint editors, runtime, reload, compiler diagnostics | Change an interaction or encounter value, save/reload or hot-reload it, then introduce a known error and show its file, location, and actionable message. Runtime systems exist, but the Qt coverage ledger lists both editors as not migrated; a full game-authoring loop is not proven. |
| A10. Track asset dependencies and produce repeatable source-independent game packages | VS · Engine · Partial · P0 | Asset database, importer outputs, cooker, manifest, package launcher | From a clean checkout, cook twice and compare deterministic outputs; extract the package outside the source tree and complete the encounter. Windows CI passed the standalone proof-game packaging and launch steps in [run 37872212846](https://github.com/rithymeth/game-ngine/actions/runs/37872212846). A published release package and independent clean-machine validation remain open. |

### Rendering, input, player, and combat

| ID and GDD source | Scope · owner · status · priority | Dependencies | Acceptance condition and evidence or known limit |
|---|---|---|---|
| R01. Render a static glTF scene with base-color materials and visible HUD on the primary backend | VS · Engine · Proven · P1 | Windows D3D12, cooker, runtime renderer, capture verifier | The exact master CI commit cooks and launches AETHER-01, then passes scene and HUD image checks on Windows D3D12 in [run 37872212846](https://github.com/rithymeth/game-ngine/actions/runs/37872212846). This proves the checked scene path only; it is not a claim of final lighting, animation, or cross-vendor coverage. |
| R02. Provide the GDD's intended lighting, shadows, material quality, and visual identity | VS · Engine + Game · Unverified · P1 | PBR/shadow path, material authoring, representative art, image checks | Run the authored encounter with approved lighting and materials on target hardware and compare reviewed image checks. Current proof-plan evidence names static meshes and base-color textures; final lighting and shadows remain unverified. |
| R03. Render animated characters, hit reactions, boss states, and required weapon poses | VS · Engine + Game · Unverified · P1 | Skeletal importer, animation runtime, IK, content pipeline | Exercise Kael, all three MVP enemies, and Warden attack/phase transitions in the packaged game; verify state changes and image evidence. Current CI's static-scene render gate does not exercise skeletal animation or IK. |
| R04. Handle the selected window size, resolution, quality profile, frame pacing, and graphics settings | VS · Engine + Game · Partial · P1 | Window/RHI resize, quality settings, input/menu persistence | Change supported resolution/window settings, restart, and verify the chosen mode and scene output; measure frame pacing at the selected profile. The proof project contains quality presets and the player settings path exists, but fullscreen support, all GDD graphics controls, and resolution coverage are not established by CI. |
| G01. Keyboard/mouse, controller, action mapping, and remappable controls | VS · Engine + Game · Partial · P1 | Input contexts, device backends, settings, UI focus | Complete movement, aiming, firing, menu navigation, and rebinding with keyboard/mouse and a physical gamepad; restart and verify bindings persist. Input and gamepad workflows have local proof-plan evidence, but a clean packaged two-device run is still required. |
| G02. Third-person walk/run/jump, aiming camera, dodge, and collision avoidance | VS · Engine + Game · Partial · P1 | Character movement, camera, physics queries, input | Run the connected level with keyboard and gamepad; verify collision, slopes, aiming distance, dodge timing, and camera obstruction around every level collider. Existing character movement is integrated. AETHER-01's game-owned camera query handles only unrotated box colliders; other shapes and rotations are unsupported. |
| G03. Physics and interaction for the transforms and collider shapes used by the level | VS · Engine + Game · Partial · P1 | Jolt integration, component serialization, scene queries, triggers | Verify movement, grounded state, trigger interaction, projectile/hitscan collision, and query behavior for every collider transform used in the packaged level. Physics and box-query paths exist; the current game-owned visibility/camera logic is limited to unrotated boxes, and rotated/scaled-shape coverage is not recorded. |
| G04. AEGIS Rifle, ARC Pistol, VOLT Shotgun, damage feedback, ammunition, and reload | VS · Game · Partial · P1 | Input, gameplay attributes/effects, physics queries, audio/VFX, HUD, save state | Complete a scripted packaged encounter that switches all three weapons, reloads, hits armor and weak points, and verifies visible/audio feedback and resource state. The proof plan records the three-weapon roster and local combat scenarios; final balance and release-package acceptance remain open. Weapon definitions and tuning stay game-owned. |
| G05. Scout, Sentinel, Hunter, and a readable Warden with attack tells and encounter changes | VS · Game · Partial · P1 | Navigation, perception, behavior trees, animation, combat, sequences | Play a deterministic scenario from first alert through the Warden reveal; verify each archetype, tells, damage, death, gate progression, and boss phase changes in the package. Existing AI systems and greybox scenarios are documented, but they do not establish final enemy quality or a clean released vertical slice. |
| G06. Vision, distance, sound, damage, and ally-alert perception | VS · Engine + Game · Unverified · P1 | AI perception, audio events, navigation, behaviors | Exercise each perception source in a packaged seeded scenario and verify investigation, alert propagation, attack, and reset within documented timing tolerances. The engine has perception systems; this complete player-facing loop lacks recorded acceptance evidence. |
| G07. Health, shield, energy, stamina, abilities, and damage-type resistance | VS core health/combat · FP remaining attributes and abilities · Engine + Game · Partial · P1 | Attributes, tags, effects/abilities, UI, save state, input | The slice must expose and persist every resource it actually uses and show damage, vulnerability, and ability feedback through death/restart. Existing gameplay building blocks and game-owned Health/ammo scenarios are documented; the complete four-resource/ability loop and Thermal/Energy/Explosive resistance matrix are not proven. |

### Presentation, story, and progression

| ID and GDD source | Scope · owner · status · priority | Dependencies | Acceptance condition and evidence or known limit |
|---|---|---|---|
| P01. HUD and pause/settings/navigation for the playable slice | VS · Engine + Game · Partial · P1 | Runtime UI, input focus, save/settings, localization-ready text | Finish the encounter without editor access using keyboard and gamepad; verify health, shield/energy/stamina where used, ammo, objectives, danger, pause, resume, restart, and settings feedback. CI captures the current scene/HUD and the proof plan records a game menu, but all required HUD/resource states are not yet shown together. |
| P02. Spatial audio, music/ambience, combat feedback, voice/radio, and mix settings | VS · Engine + Game · Partial · P1 | Audio import/runtime, mixer, listener, cue control, settings | Verify positional attenuation and pan, music/ambience transitions, combat cues, and narrative voice in the packaged level; stop/restart without orphaned audio. Existing cue, mixer, and package work is documented; final mix and narrative voice are not established. |
| P03. Cinematics, interaction sequences, terminals/logs, and objective transitions | VS · Engine + Game · Partial · P1 | Sequencer, camera, audio, UI, interaction, save progression | Trigger the Warden introduction and story reveal, interrupt or restart them at expected checkpoints, and verify camera/audio/UI teardown. The proof plan describes a game-owned archive reveal using existing sequence APIs; the remaining GDD cinematics are full-production content. |
| P04. Checkpoint, death/restart, progress, and settings survive process restart and packaging | VS · Engine + Game · Partial · P0 | Save slots, game state schema, user paths, package, error reporting | In the extracted package, complete an objective, die/reload, restart the process, and verify checkpoint, objective, settings, and expected resource state. Local proof-plan tests cover checkpoint and visible write failure; clean released-path compatibility and corrupt/incompatible-save preservation remain open. |
| P05. Basic interaction and one environmental puzzle only where the selected level requires it | VS · Game-owned · Game-owned · P1 | Interaction APIs, physics, UI, objective state | Each required door/terminal/power interaction must be completable with clear feedback and must survive the specified checkpoint path. Puzzle rules, clues, and content belong to AETHER-01; advanced energy-routing, gravity, hacking, and object-manipulation puzzles remain full-production scope until the vertical-slice level requires them. |
| P06. Tutorial and first playable objective flow | VS · Game · Partial · P1 | Input prompts, interaction, objective state, UI/audio | A first-time player can learn movement, look, interact, scan, shoot, and dodge through the level without developer commands. The current proof project is a short regression workload; the complete onboarding flow is not yet accepted. |
| P07. Branching endings, five acts, logs/collectibles, inventories, economy, upgrades, and achievements | FP · Game-owned · Game-owned · P2 | Save schema, UI, content pipeline, progression APIs | Define and test each authored progression path in the full-production plan before those acts are built. These are game-owned content and systems unless the requirements audit identifies a missing reusable engine contract; they do not block the current engine proof. |

### Delivery, performance, diagnostics, and support

| ID and GDD source | Scope · owner · status · priority | Dependencies | Acceptance condition and evidence or known limit |
|---|---|---|---|
| D01. Clean build, cook, package, extracted launch, and actionable failure output | VS · Engine · Partial · P0 | CMake/CI, cooker, package manifest, runtime, diagnostics | Repeated clean-checkout CI must build, cook, package, extract outside the source tree, launch, and complete the encounter. Current master CI passed its Windows package and standalone proof-game steps; a published release and repeated-run stability record remain outstanding. |
| D02. Crash context and logs identify build, hardware, graphics API, scene, stack, and last gameplay state | VS · Engine + Game · Unverified · P1 | Logger, crash capture, build metadata, user-path storage | Trigger a controlled failure in the packaged runtime and verify a durable report with the GDD's required fields and no loss of the last valid save. The GDD defines the policy, but this acceptance evidence is not recorded. |
| D03. Measure CPU/GPU frame time, memory, load/cook duration, and variance against reviewed budgets | VS · Engine + Game · Unverified · P1 | Profiler, representative packaged scene, named hardware | Capture repeatable runs on the agreed minimum and recommended hardware, then demonstrate 1080p/30 minimum and 1080p–1440p/60 recommended targets or revise them with the owner. GDD section 57 calls GTX 1060/RX 580 and RTX 3060/RX 6600 preliminary classes; the documented RTX 3050 laptop run is not a substitute. |
| D04. Clean packaged run on the declared primary platform | VS · Engine · Partial · P0 | Windows/MSVC/D3D12 support policy, package, GPU/driver coverage | Complete the full proof scenario on a clean Windows x64 machine outside the source tree; record OS, MSVC/SDK, GPU, driver, backend, resolution, and package hash. CI proves one Windows MSVC/D3D12 runner path; user-hardware/vendor coverage and final support policy remain open. |
| D05. Subtitles, speaker identity, text sizing, color options, sensitivity, aim assist, and motion settings | VS required controls/subtitles · FP quality/accessibility completion · Engine + Game · Partial · P1 | UI focus, input/settings persistence, subtitle presentation, color-safe indicators | Verify every accessibility setting named in GDD section 58 in the packaged slice and confirm persistence. The current game has audio/control settings work; subtitles, speaker identification, adjustable text, color options, aim assist, and motion controls are not all proven. |
| D06. Other graphics backends and platforms | D unless the project later declares support · Engine · Unverified · P3 | RHI backends, packaging, device test matrix | Do not claim game support from a compile alone. Linux and macOS jobs validate engine configurations; AETHER-01 remains Windows-first until full packaged scenarios and support criteria are approved for another platform/backend. |

## Full-production and deferred requirements

| GDD sections | Classification and owner | Current readiness / exit condition |
|---|---|---|
| 22–25, 29–35, 55: Guardian, Aberrant, Aether Prime, later acts, surface/ruins/core, terrain, and world streaming | FP · Game content plus Engine capabilities | The systems may exist, but no full-game packaged acceptance is required for First Contact. Before the corresponding act, prove import, streaming boundaries, memory use, animation, boss phase transitions, and loading recovery on the target PC. |
| 11, 17, 36–41, 59: advanced traversal, later abilities, exploration, puzzles, inventory, upgrades, resources, crafting, and difficulty modes | FP · mostly Game-owned | Specify which requirements ship before implementing them. Exercise the selected content through supported input, UI, gameplay, and save APIs. Optional slide, grapple, and air dash remain deferred unless a designed level requires them. |
| 62–75, 89: full quest graph, five-act narrative, three endings, post-credits, 20–30 achievements, 25 logs, 10 fragments, 15 weapon modules, complete economy/progression | FP · Game-owned | Each act and persistent reward needs a content acceptance scenario and migration-safe save fixture. None is a current engine-readiness pass claim. |
| 54 and 78: multiplayer, dedicated servers, MMO, console, VR, mobile, marketplace, mod SDK | D / out of scope · Engine | No AETHER-01 delivery gate. Retain existing engine capabilities without expanding this upgrade into new targets or services. |
| 79–80, 95–103, 107–108: staffing, production calendar, store launch, marketing, pricing, community, press kit, and project documentation | Game production / business · Game-owned | These are not engine capability requirements. Store packaging may use the engine's package output, but account, storefront, commercial, and marketing acceptance belongs to the project owner. |

## Highest-priority gaps from this audit

1. **Prevent scene component loss (P0).** At the assessment point, Qt loaded scenes without starting the project's modules, and generic loads skipped unknown component names. PR #186 adds module startup, built-in schema registration, and strict editor loading that rejects unknown components; CI passed, while the full AETHER-01 component/GUID comparison is still required before closing the gap. See editor/src/core/scene_document.cpp, engine/src/scene/serialization.cpp, and editor/qt/main.cpp.
2. **Complete the clean-package and save-data gate (P0).** CI now proves the standalone AETHER-01 package flow, but publish and validate a release archive outside the checkout; include checkpoint, corrupt-save, and engine-version upgrade fixtures.
3. **Close the vertical-slice content path (P1).** Prove skeletal animation, useful lighting/shadows, effects, input/accessibility states, and script/Blueprint diagnostics in one authored scene.
4. **Measure on agreed PC profiles (P1).** The frame budgets and GPU classes are preliminary. Record repeatable CPU/GPU/memory captures on named minimum and recommended machines before claiming performance readiness.

The production gate files remain unchanged. Their n/a yet values stay in
place until those gates' own evidence is collected.
