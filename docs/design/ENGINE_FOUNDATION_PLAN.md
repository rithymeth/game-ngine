# Engine Readiness Plan Before AETHER-01 Production

## Objective

Strengthen Aether until a small team can author, iterate on, and ship an
AETHER-01 vertical slice with it. Start from the game's actual requirements,
identify which engine capabilities are complete, partial, missing, or not
proven, then close the highest-priority gaps before resuming game production.

This is **not** a feature freeze. The existing M8 milestone remains an
improvement-only effort, as described in
[`M8_PRODUCTION_TECHNOLOGY.md`](M8_PRODUCTION_TECHNOLOGY.md). This readiness
plan also allows targeted completion or addition of an engine capability when
the approved AETHER-01 scope depends on it. Such work must be named as a game
readiness gap, have an acceptance test, and stay limited to what the game
needs. It must not be counted as M8 completion or grow into unrelated engine
features.

## Target and boundaries

Use the AETHER-01 GDD as the requirements source, but stage its scope:

- **Before the vertical slice:** support the core PC, single-player authoring
  and runtime needs for one connected level, one playable character, the
  agreed MVP weapons/enemies/boss, save/checkpoint, presentation, and a
  packaged build.
- **Before full-game production:** close the additional GDD requirements when
  their acts need them, including larger world transitions/streaming,
  additional missions, progression, puzzles, and full narrative presentation.
- **Out of scope for AETHER-01:** multiplayer, dedicated servers, MMO, console,
  VR, mobile, marketplace, and mod SDK work, as specified in the GDD.

The engine readiness target is a playable, representative vertical slice. It
does not mean the full 3–5 hour game or Aether 1.0 is complete. The current
AETHER-01 greybox is a regression and integration workload during engine work;
do not expand its story or production content before the readiness gate passes.

## Starting reference

- Use released **v0.27.2**, commit `cc29494`, as the immutable engine baseline.
  Validate newer changes against that release and record their exact commit.
- The release and local proof-game checks show useful capabilities, but do not
  prove the full GDD requirements or every clean-machine workflow.
- The current product-gate tracker marks all twelve entries **n/a yet**. Keep
  those statuses until the specified evidence exists. This plan does not
  complete the M7 production gate or certify Aether 1.0.
- Public API stability remains experimental until its documented freeze gate
  is approved. Treat serialized formats as compatibility contracts; any
  approved change needs a version, migration, and fixture coverage.
- The proof plan records static glTF/base-color rendering and local D3D12 and
  Vulkan captures, while lighting and animation rendering remain unverified.
  It also describes the current game as a short greybox, not the full
  60–90-minute MVP. Use the capability audit below to refresh these facts from
  evidence rather than assuming roadmap entries are implemented.

## Capability status rules

Every requirement in the GDD-to-engine matrix must have one status:

- **Proven:** works end to end in a clean, versioned engine build with a
  repeatable test or recorded acceptance run.
- **Partial:** some behavior exists, but a required workflow, quality level,
  edge case, or integration is missing.
- **Missing:** the engine does not provide the required capability and the
  game cannot implement it safely using supported APIs.
- **Unverified:** implementation appears to exist, but evidence is absent or
  only local/ad hoc.
- **Game-owned:** the behavior belongs in AETHER-01 content/scripts and does
  not need a general engine API.

Do not mark a capability proven because code compiles or a roadmap says it is
done. Keep engine-owned requirements separate from game-owned content and
mechanics.

## Required capability audit

Phase 0 must create a row for every applicable GDD requirement. At minimum,
audit these groups and identify the concrete gap and acceptance evidence for
each:

| Capability group | AETHER-01 need to assess |
|---|---|
| Project/editor workflow | Create/open project; compose and inspect scenes; import and reimport content; edit scripts/data; save, close, reopen, undo, and recover interrupted work without data loss. |
| Content pipeline | Import static and skeletal models, textures/materials, animation, audio, and effects; report invalid inputs; track dependencies; cook reproducibly; package without source-tree dependencies. |
| Rendering | Windows target backend, visible materials and lighting, shadows, animated/skinned characters, effects, HUD composition, resize/resolution handling, and defined visual checks. Validate other backends only where the project declares them supported. |
| Player and physics | Keyboard/mouse and gamepad input, rebinding, third-person movement and camera, collision and queries for actual level collider shapes/transforms, interaction, damage, death, restart, and checkpoint behavior. |
| Gameplay and scripting | Existing gameplay kits, attributes/resources, weapon switching/reload/fire, damage feedback, abilities required by the approved slice, reliable Luau/Blueprint iteration, and useful script diagnostics. |
| Enemies and animation | Navigation, perception, behavior authoring, enemy state/attack tells, boss phases, skeletal animation, transitions, hit reactions, and weapon/character IK required by the slice. |
| Audio, effects, and sequences | Spatial and 2D audio, music/ambience, combat feedback, logs/dialogue presentation, runtime effects, timed sequences, and reliable stop/restart/scene-replacement behavior. |
| UI and accessibility | HUD, pause/settings/controls, keyboard and gamepad navigation, readable feedback, subtitles where used, and persistence of user settings. |
| Progress and saves | Checkpoints and agreed progression state survive restart and packaged execution; failures are visible; corrupt or incompatible saves do not silently destroy valid progress. |
| Build, performance, and release | Clean CI, repeatable build/cook/package, actionable failures, crash/log capture, supported-hardware profiles, memory/frame-time budgets, checksums, and tested standalone launch. |

The full GDD also includes terrain/world streaming, more acts, additional
puzzles and progression systems. Classify those as **vertical-slice required**,
**full-production required**, or **deferred** during Phase 0. Do not implement
them early if the approved vertical slice does not exercise them.

## Ordered upgrade roadmap

### Phase 0 — Requirements and gap map

Translate the GDD and the agreed First Contact slice into a traceable matrix.
For each requirement, record its owner (engine or game), status, dependent
systems, acceptance method, target build/platform, known limitations, and
priority. Reconcile the matrix against CI, tests, benchmarks, release records,
the AETHER-01 proof plan, and the M7/M8 trackers.

**Exit:** every vertical-slice requirement has a status and a pass condition;
scope conflicts and out-of-scope features are resolved; current claims link to
evidence; the exact baseline commit and supported Windows/toolchain matrix are
recorded.

### Phase 1 — Core project, data, and delivery foundation

Stabilize project and scene serialization, asset identity/dependencies,
configuration, save compatibility, public API ownership/lifetimes, clean
builds, CI, cooker, package manifests, and standalone launch. Finish or repair
existing implementations first. Add missing pieces only when Phase 0 shows
they block safe authoring or delivery.

**Exit:** representative projects and saves survive load/edit/save/reopen and
engine upgrade checks; required CI passes repeatedly from clean checkouts; a
released package can cook and launch without the source tree; data-loss,
missing-dependency, and package-failure paths are covered by evidence.

### Phase 2 — Authoring and content pipeline

Make the editor and import workflow sufficient for the vertical slice: scene
composition, inspection, asset import/reimport, material and animation setup,
script/Blueprint iteration, actionable diagnostics, undo/recovery, and
repeatable cooking. Establish the selected editor shell as supported only
after it passes the same workflow checks; an editor framework choice by itself
does not close this phase.

**Exit:** a clean workstation can create/open the reference project, import
representative mesh, texture, animation, and audio assets, make and save a
scene/script change, reopen it, cook, and find useful diagnostics for an
intentional import or script error.

### Phase 3 — Player, combat, and enemy runtime

Complete the capabilities required to implement the agreed slice: input and
rebinding; third-person character movement/camera/collision; combat interactions
and feedback; physics queries for the shapes and transforms used by the level;
required gameplay resources/weapons; navigation, perception, behavior and boss
encounters; and the animation/IK path used by the player and enemies. Fill gaps
in current systems or add a narrowly scoped capability only when the matrix
shows existing APIs cannot meet an approved requirement.

**Exit:** a scripted test scenario exercises start, movement, aiming, weapon
use, enemy tells/damage/death, boss encounter, player death/restart and
checkpoint restore in the packaged runtime. The scenario runs deterministically
within documented tolerances and has regression coverage for fixed defects.

### Phase 4 — Presentation and progression runtime

Complete the vertical-slice needs for spatial audio and mix/settings, readable
HUD and menus, gamepad navigation, subtitle/log presentation where required,
combat/environment effects, sequences, interactions, objective flow, and
save/checkpoint progression. Confirm teardown, scene reload, audio stop, and
settings persistence across repeated sessions.

**Exit:** the packaged scenario communicates objectives, danger, damage,
resources and encounter changes; audio/sequence/UI state survives expected
pause, restart and checkpoint paths; the slice can be completed without editor
access or developer commands.

### Phase 5 — Performance, stability, and supported PC baseline

Use a named representative scene and hardware profiles based on the GDD's
preliminary minimum and recommended PC targets. Measure CPU/GPU frame time,
memory, load/cook duration, and variance before tuning. Establish approved
budgets first: the GDD's preliminary target is 1080p/30 FPS minimum and
1080p–1440p/60 FPS recommended, subject to confirmation on real hardware.
Exercise supported graphics backends, long sessions, save/load, packaging,
and failure recovery. Do not claim hardware coverage without a run on that
class of machine.

**Exit:** the representative packaged scene meets reviewed budgets on named
hardware; repeated play/save/reload runs have no blocker crash or save
corruption; supported backend checks pass; remaining limits and untested
hardware are published.

### Phase 6 — Engine-readiness review

Review Phases 0–5 together on one exact candidate commit. Fix any dependency
gaps exposed by the integrated run, then produce a tagged proof build and
versioned readiness report. Keep later full-game capabilities visible as
separate gates instead of implying they are finished.

**Exit:** every vertical-slice requirement is Proven or explicitly accepted as
Game-owned; no Missing/Partial/Unverified vertical-slice blocker remains; all
required clean-build, authoring, packaged-runtime, save, performance, and
supported-backend evidence points to the candidate; the remaining full-game
requirements have owners and pass conditions.

## Rule for resuming AETHER-01 work

Until Phase 6 passes, use the existing greybox only to reproduce engine gaps
and verify fixes; do not add production story, levels, or game content. Once it
passes, resume with the scoped vertical slice against the tagged engine
package, not an undocumented local checkout. Before expanding that slice into
the full five-act game, pass the separate full-production capability gates
identified in Phase 0.

Keep game-specific behavior in the game project unless the audit proves that
an engine capability is missing or its current contract is insufficient. For
every engine gap, record why game-side code cannot safely satisfy the need,
implement the smallest reusable capability that does, and add a regression
and end-to-end acceptance check.

## Immediate next actions

1. Create the GDD-to-engine matrix and classify each requirement by vertical
   slice, full production, game-owned, or deferred scope.
2. Mark every matrix row Proven, Partial, Missing, Unverified, or Game-owned;
   attach links and exact evidence rather than inferring readiness from code.
3. Rank gaps by dependency: project/data/build safety first, then authoring and
   content, player/runtime systems, presentation/progression, and performance.
4. Convert the highest-priority blockers into small engine upgrades with
   acceptance criteria before resuming game-content production.
5. Update the M7/M8/product gate trackers only when their own required evidence
   exists; keep this readiness plan and Aether 1.0 release decision distinct.

For each phase, record candidate commit, date, platform/toolchain, evidence,
known exceptions, reviewer, and status. A capability is ready when it is
integrated, tested, documented, and usable in the packaged game workflow—not
when it merely compiles.
