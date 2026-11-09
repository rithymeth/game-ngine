# Aether Roadmap

The previous 100 KB phase-by-phase roadmap (Phases 6-48) was retired on 2026-11-08. It described a plan, not the
current state, and had drifted from the code. It is still in git history (`git log -- docs/ROADMAP.md`). Progress is
recorded in [`devlog/`](devlog/2026-11-08.md); evidence of what works is in the readiness matrix.

## Where the engine stands

Milestones M1 to M7 (editor foundation, scripting and Blueprints, renderer, animation, audio, UI, VFX, AI and nav,
terrain and streaming, netcode, cook and package, cinematics, save, localization, gameplay kits, SDK, QA) have code in
the tree. M8 is the improvement-only pass over those systems:
[`design/M8_PRODUCTION_TECHNOLOGY.md`](design/M8_PRODUCTION_TECHNOLOGY.md).

Code existing is not the same as proven. The honest status per requirement is in
[`design/AETHER_01_ENGINE_READINESS_MATRIX.md`](design/AETHER_01_ENGINE_READINESS_MATRIX.md); the proof workload is
[`design/AETHER_01_PROOF_PLAN.md`](design/AETHER_01_PROOF_PLAN.md).

## Direction: a AAA-capable engine

Judge status from code and CI, not from documents. Measure before building ([`design/PERFORMANCE.md`](design/PERFORMANCE.md)).

| Priority | Goal | Done when |
|---|---|---|
| 1 | Close the AETHER-01 matrix | Every "Partial" and "Unverified" row is proven on a clean Windows machine, skeletal animation/IK, VFX and the Luau/Blueprint loop first |
| 2 | Performance budgets | Frame-time budgets exist for defined hardware and a stress scene (10k+ entities, streaming world) meets them |
| 3 | Production editor | The Qt editor's future is decided, missing tools are migrated, and interrupted sessions recover without data loss |
| 4 | Pipeline at scale | Cooks are deterministic and incremental with thousands of assets; reimport keeps identity stable |
| 5 | Platform depth | Windows x64 / D3D12 is excellent and has a signed-off GPU and OS policy before other platforms are promoted |
| 6 | Second game | A different kind of game ships on the engine and its gaps become tasks |

## Rules

- A feature is done when it is verified by running it, not by inspection.
- `ctest` is the gate. Run all of it before believing a change.
- Negative results are kept in `PERFORMANCE.md`.
