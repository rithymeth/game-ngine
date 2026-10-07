# AETHER-01: First Contact — Engine Proof Plan

## Purpose

Build the AETHER-01 MVP as a real game project to prove the current Aether
Engine workflows. The game is also the acceptance workload for an improvement
release of the engine. Keep engine changes within existing systems: correctness,
reliability, performance, diagnostics and workflow fixes. Game-specific content
and gameplay belong in the game project; they must not silently become new
engine feature families.

This plan does not claim that the full Aether 1.0 production gate is complete.
The gate in `production_gates.json` remains authoritative for that release.

## Fixed proof scope

Use the GDD's reduced MVP, **Aether-01: First Contact**, as the first target:

- One compact, connected environment, primarily the HELIOS-7 cryo/research
  facility. Sell the wider world with vistas, doors, tunnels and audio logs.
- One playable character, Kael in the AEGIS-9 suit.
- AEGIS Rifle, ARC Pistol and VOLT Shotgun.
- Scout, Sentinel and Hunter.
- One Warden encounter with readable attack tells, at least two meaningful
  encounter changes, and a story reveal.
- A 60–90 minute target is aspirational for this compact MVP; the first
  engine-proof build is one complete playable level and combat encounter.
- Single-player, Windows/PC first. No multiplayer, dedicated server, new
  platform target, marketplace, modding, or optional movement feature.

The GDD's 3–5 hour, five-act game is later production scope. Do not let it
expand the first proof build.

## Initial engine capability audit

This is a starting map, not a feature guarantee. Recheck each item against the
actual authoring and packaged-player workflow as the prototype is built.

| AETHER-01 need | Existing engine path | Initial status / gap |
|---|---|---|
| 3D scene, camera, project and player | Third Person project template, scene components, Luau script, player runtime, RHI | AETHER-01 now has a 31-entity connected cryo/service-route/Warden greybox. The packaged player draws static glTF meshes and base-color textures through the existing cross-API RHI. Direct pixel inspection confirms complete scene/HUD captures. Lighting and animation rendering remain unverified. |
| Rendered-player evidence | Existing swap-chain readback API and player screenshot option | Normal-build 960x540 captures passed 12 D3D12 and eight Vulkan launches locally. Earlier partial image previews did not reflect the saved pixels; no rendering/readback fault was established. CI now checks scene/player/HUD pixels across three launches, replacing its file-size-only check; this strengthened gate still needs a pushed CI result. |
| Input and rebinding | Input actions, mapping contexts, player settings | Present; the first Windows test run exposed open-file cleanup failures, fixed in the current working changes and verified in the full suite. |
| Collision and physics | Jolt-backed rigid bodies, scene queries and component serialization | Present; build encounters with existing behavior and validate packaged startup. |
| Enemy navigation and behavior | Navmesh, NavAgents, behavior trees and perception | Present; enemy decision graphs and tuning are game content. |
| Weapons, health and abilities | Gameplay tags, attributes, effects, abilities; Luau and Blueprint support | Existing building blocks. Weapon definitions, encounter rules and damage tuning are game-owned. |
| Animation, audio and effects | Animation graphs/IK, audio sources/cues, VFX | Existing systems; final content and quality still need proof. |
| HUD and menus | Runtime UI draw list, font atlas and RHI | AETHER-01 locally draws player/enemy health, Warden phase/strike warnings, ammo/reload state, mission objectives, checkpoint status, ending text, damage flash and hit confirmation. Menus and shield/energy/stamina remain unimplemented. |
| Checkpoints and save/load | Save slots, world state, settings and player folders | Game-owned milestone/schema payload locally verifies progression across fresh runtime instances. Health/ammo and an unfinished boss reset at safe checkpoint positions. Save failure is visible; clean packaged-path validation remains open. |
| Cook/package/standalone run | Cooker, .apak, package workflow and player executable | Existing path; clean-checkout and machine-independent launch still require end-to-end proof. |
| Third-person camera collision | Game-owned box-segment query in AETHER-01 | Camera distance shortens at the current level's unrotated box colliders. Other shapes and rotations remain unsupported; this is not a general engine camera-collision guarantee. |

## Baseline verification recorded 2026-10-07

On the Windows development workstation (MSVC 17.14.37, CMake 3.31.0, NVIDIA
GeForce RTX 3050 Laptop GPU), the Ninja `RelWithDebInfo` build of the current
working changes built the editor,
player, cooker, pak tool, functional runner, Coin Run and Gem Hop. The Windows
unit suite passed **1,036/1,036** and the functional runner passed **2/2**.
This is a local baseline, not a clean-checkout or GitHub release result. The
proof game, clean packaging validation and GitHub release gates remain open.

The follow-up character-controller integration is locally verified: the Third
Person template uses the existing `CharacterMovement` system and a
collider-backed ground in physics builds. Physics-on and physics-off template
test groups each pass **15/15**. The full Windows unit and functional suites
also pass on this working tree. This closes the template's fixed-floor
movement gap, but does not prove AETHER-01 gameplay or a release build.

The first AETHER-01 project scaffold is at `games/AETHER-01`. The initial
project cooks and passes a 120-frame packaged-player smoke run in
[GitHub CI](https://github.com/rithymeth/game-ngine/actions/runs/37637914254).
The pushed rendering update adds a D3D12 player screenshot gate; its CI run is
tracked separately. The current local game iteration adds a game-owned
box-collider line-of-sight test, three-hit Scout health with knockback, player
death/restart, a 30-round reload loop, and a runtime HUD using existing UI
primitives. It cooks to 19 assets and completes a local 120-frame headless run.
The current follow-up passes **19/19 CTest entries**, including **1,038 unit
tests** and the two cooked-game combat tests. Blank and truncated captures are
rejected by the new pixel verifier. Normal-build windowed screenshots pass repeated direct pixel checks on D3D12
and Vulkan. Investigation corrected an earlier report: partial previews
omitted pixels that were present in the saved image, and original-resolution
viewing shows the complete frame. Temporary draw diagnostics and speculative
readback changes were removed.

The next local iteration completes a connected greybox mission: rifle pickup,
Scout defeat, service gate/route, four Warden phases, archive reveal, ending and
new game. Seven Windows physics-on cooked-game tests verify combat, checkpoint reload, the input-driven
start-to-ending path, strike warning/damage/dodge, and visible save-write failure.
The Warden removes armor, exposes a core that takes extra damage, and increases
strike radius/frequency in its final phase. The route and moving Scout use existing
kinematic bodies. All six applicable tests also pass in a separate physics-off
`RelWithDebInfo` build, whose full unit suite passes **1,003/1,003**. This fixes
the missing optional-component lookups found in the first combat CI run
(`37651575215`, commit `1031fab`); a new CI result is still required.
This is a short prototype workload; it does not establish the
MVP's 60–90 minute duration. Remaining audio/cinematics, menus,
clean package delivery, released-engine validation and CI gates remain open.
The Windows physics-on full suite also passes **1,043/1,043** unit tests and
**19/19 CTest entries**. The current 32-asset cook passes startup scene/HUD pixel
checks on both D3D12 and Vulkan.

## Packaged audio hardening recorded 2026-10-08

The current local engine changes connect the existing AudioSystem, streamed
WAV/Ogg/FLAC bank, cue player, mixer and platform/null output to Game. Sound cues
(`.acue`) now receive asset registration and can be cooked. Bound and unbound
sequence audio keys play, stop and fade through existing components/cue APIs.
Master/Music/SFX/Voice settings apply to the mixer. Headless tests render real
samples without opening an audio device. Cooked fixture playback passes after
its source Content folder is removed.

This verification exposed and fixes two existing scene-replacement lifetime
issues: gameplay kit scheduler stages kept pointers into the old Runtime, and
sequence cache data was cleared before its players. Kit stages now resolve the
current runtime while custom scheduler stages remain registered. Sequence
players are torn down before their cached data. The selected player/game/asset
checks pass **42/42**, including audio playback/volume/listener, bound/unbound
sequence fades, invalid wave diagnostics, source-free cook/play and repeated
scene replacement. Local CTest passes **19/19**, including **1,052/1,052** unit
tests and **2/2** functional checks; the physics-off suite passes **1,012/1,012**.
The windowed player still passes the startup scene/HUD pixel check.
Game-specific sound content,
cinematic presentation and pause/settings menus remain open; this engine host
work does not establish those game acceptance items.

## Delivery sequence

### Combat roster verification recorded 2026-10-08

The local 37-entity greybox now contains all three MVP weapons and all three
regular enemy roles. The ARC Pistol damages the Sentinel shield twice as fast
as the rifle; the short-range VOLT Shotgun deals three damage, pushes lighter
enemies farther and staggers them. Magazines remain separate across switches,
and switching is blocked during reload. Research contains a shielded stationary
Sentinel with a warned ground strike and an ambushing Hunter with a warned
charge. Both must die before the Warden gate opens. Enemy damage checks the
same unrotated-box visibility contract as the existing game-owned hitscan.
The arena checkpoint spawns beyond the new gate collider.

Cooked-game checks pass **11/11** with physics and **10/10** without physics.
They cover the complete input-driven mission, weapon selection (including a
gamepad binding), magazine/reload/held-fire behavior, shotgun range/damage/
knockback/stagger, Sentinel warning/damage/dodge, and Hunter warning/charge/dodge.
The local Windows physics-on full unit suite passes **1,047/1,047**. The current
cook contains 41 assets. These are local results; the prior connected-mission
CI run `37655534021` is still queued and this roster needs its own pushed result.
Audio, cinematic presentation, pause/settings menus and release/package gates
remain open. The content is a brief greybox, not a measured 60–90 minute game.

### 1. Lock the baseline

- Record the current commit, clean-build commands, CI jobs, supported
  configurations, current tests and package contents.
- Inventory the existing player, renderer, physics/character movement,
  animation, audio, UI, VFX, AI/navigation, streaming, save, sequencer,
  gameplay kits, cooker and pak workflows against the game's needs.
- For each requirement, record `usable`, `needs hardening`, `game-owned`, or
  `unsupported`; link code and verification evidence. Do not promise a system
  based on roadmap text alone.
- Preserve uncommitted user work while doing this audit.

### 2. Improve only proven engine blockers

Prioritize issues that stop the first level from being authored, played,
saved, cooked, packaged or run reliably. Fix correctness and data-loss risks
first, then build/release diagnostics, then measured performance. Each engine
change must state its affected existing contract and add focused regression
coverage when appropriate. Do not add a new public subsystem to satisfy a
game-specific need; implement game-owned behavior on existing APIs or record
the need as an explicit blocker for a later scope decision.

The M8 improvement-only principles in `M8_PRODUCTION_TECHNOLOGY.md` apply,
with this proof game as the representative project. The game is an acceptance
workload, not permission to expand engine feature scope.

### 3. Build the proof game

Create a separate `AETHER-01` project with source, content, configuration,
build/package instructions, and a playable default level. Build in this order:

1. Greybox player movement, over-shoulder camera, one weapon, one enemy and
   one combat room. Prove controls, collision, aiming, hit feedback, death,
   restart and checkpoint/save behavior.
2. Add the locked three-weapon and three-enemy roster using existing gameplay,
   AI, animation, audio, VFX and UI capabilities. Add one complete Warden fight.
3. Complete one connected level with objective flow, environmental clues,
   one short cinematic/audio-log sequence, settings, pause, save/load and a
   clear ending.
4. Cook and package a clean Windows player build; validate it from a clean
   checkout and on a machine without the development tree.

Art quality, playtime and performance targets must be measured and reviewed;
do not claim final-quality content from a greybox build.

### 4. Release the engine proof version

Use a versioned **proof release** (pre-1.0) only after the relevant current
Windows build, tests, game functional checks, clean cook/package/run path,
artifact inventory, checksums and release notes all pass. Include known
limitations and supported configurations. Do not change the Aether 1.0 gate
statuses to passed based on this narrower release. Signing is used when the
release environment has configured signing credentials; otherwise disclose
that the artifacts are unsigned.

### 5. Validate and publish the game proof

Build the game against the released engine version, not an undocumented local
engine checkout. Capture a clean build log, playable build artifact, controls,
known limitations, and evidence for the acceptance checklist below. Keep the
full 3–5 hour game in preproduction until the proof build has been reviewed.

## Acceptance checklist

### Engine proof

- A clean Windows checkout configures and builds the editor, cooker, pak tool,
  player and game without relying on ignored local build outputs.
- The first level can be authored, saved, closed and reopened without data loss.
- The project can be cooked and packaged; the packaged player starts and loads
  the intended level without editor or source assets. A Windows CI screenshot
  confirms the packaged player renders the scene, not only that startup works.
- Existing engine tests and applicable functional checks pass on the released
  commit; CI results and artifact checks are linked in release evidence.
- Performance claims include machine, build configuration, workload and
  capture. A 1080p target is claimed only after an actual representative scene
  is measured on target hardware.

### Game proof

- A new player can start, move, aim, fire, use the selected abilities, defeat
  enemies, reach the boss, complete the encounter and see the ending.
- HUD communicates health/shield/energy as implemented, ammunition, objective
  and relevant cooldowns; feedback makes hits, damage and danger legible.
- Checkpoint or save/reload preserves the agreed MVP progress state.
- The packaged build completes a clean launch-to-ending playthrough without
  editor access, crashes, blocking defects or lost progress.
- The delivered content is labeled as prototype/greybox or polished according
  to what was actually completed; the 60–90 minute target is reported honestly.

## Release distinction

The proof release demonstrates a defined Windows game workflow and one game
project. Aether 1.0 remains blocked until every row in the production gate is
verified, including its broader platform, QA, SDK, compatibility and sample
game requirements. Keep the two release claims separate.
