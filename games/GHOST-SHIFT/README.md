# Ghost Shift — Willow House first playable (foundation)

Ghost Shift is a 1–4 player co-op first-person comedy-horror cleaning game: you run an underqualified paranormal
cleaning company in the haunted city of Grimborough. This project is the **single-player, graybox foundation** of
the first contract, Willow House. It is game-owned content only (no engine changes); the engine plan and ledger are
in `docs/design/GHOST_SHIFT_ENGINE_INTEGRATION.md` and the engine inventory in `docs/design/GHOST_SHIFT_INVENTORY.md`.

## What is in it

- `GHOST-SHIFT.aproject` — fixed 30 Hz simulation (`fixed_timestep_hz: 30`); all rules run on the fixed step.
- `Content/Scenes/WillowHouse.ascene` — graybox Willow House: living room, kitchen, basement with stairs, attic
  floor and hatch marker, lawn and company van, 4 ectoplasm patches, 1 cursed stain, 4 hiding spots, 5 Prankster
  waypoints, the Prankster. Colliders are unscaled, axis-aligned boxes (separate from the scaled visual meshes),
  because the player's nav bake and AI line of sight ignore Transform scale/rotation.
- `Content/Scripts/GhostShift.luau` — first-person look/move on the engine's Jolt `CharacterMovement`; tools; the
  `WillowCleanup` quest; the haunting director; the `company` save.
- `Content/Scripts/Prankster.luau` — the Prankster, a seeded state machine (Dormant → Investigating → 1.5 s warning
  → Pranking, Stunned; Hunting/Contained reserved).
- `Content/Gameplay/` — `SpectralVacuum`/`UVScrubber` items, `VacuumPulse` (cost: Battery −2, Tank +1) and
  `StunBlast` (Battery −4) abilities, `ContractPayout` (+250 Currency) and the quest.
- `Content/Input/` — Move (WASD / left stick), Look (mouse / right stick), Interact (E / pad X), UseTool (hold
  mouse left / RT), SwitchTool (Q / pad Y).
- `tools/build_content.py` — regenerates the scene, models, sounds, input and kit assets with deterministic
  (uuid5) GUIDs. The Luau scripts are hand-written.

## Rules

| | |
|---|---|
| Vacuum | Hold UseTool within 2.5 m of ectoplasm: a pulse every 0.4 s removes 1 residue (2 per patch). It never fires (or spends) with no dirty patch in reach or a full tank (6). |
| UV scrubber | Cleans the cursed stain (optional objective). On ectoplasm it is wrong cleaning: haunting +6. |
| Van | Interact within 3.5 m: empties the tank; once all 4 patches are clean it extracts and completes the contract (+250, once). |
| Haunting | 0–100, stages Quiet/Stirring/Active/Dangerous/Critical every 20. +0.15/s per dirty patch, −2 per vacuum pulse, −1/s when clean, +5 per prank. |
| Prankster | Waits (shorter at higher stages), drifts to a waypoint, then — if it can steal — warns for 1.5 s (light flare, spatial moan, `Warnings`+1) and steals the held tool, hiding it at a seeded hiding spot. Vacuum it while it investigates (stun blast, Battery −4) to stun it for 4 s. Interact near a hidden tool to take it back. |
| Save | Slot `company`: currency and contracts completed, loaded at start. |

## Run and test

```
python3 games/GHOST-SHIFT/tools/build_content.py          # only after changing the generator
<build>/tools/cook/aether_cook --project games/GHOST-SHIFT/GHOST-SHIFT.aproject --out <dir> --strict
<build>/player/aether_player --pak <dir> --headless --frames 300 --report
<build>/tests/aether_tests GhostShift_
```

At the end of a run `GhostShift.luau` prints `Ghost Shift attributes: {...}` (its `OnDestroy`), since the
player's `--report` line lists a fixed set of engine attributes. `tests/test_ghost_shift.cpp` cooks this project and
plays it headless at 30/60/144 Hz, and runs a loopback host + 2 clients vacuum test.

## Not yet

Multiplayer in the player (no listen server; Luau state doesn't replicate), HUD/prompts, containment trap and
tether, other contaminations/tools, the extraction flow beyond the van, HQ/progression, real art.
