# AETHER-01: First Contact

This is the playable project scaffold for the AETHER-01 engine proof game. It
is based on the engine's Third Person project template so the first pass uses
the real project, input, scene, Lua, physics, cooker, and standalone-player
workflows.

## Current state

**Greybox room scaffold; the combat slice is not implemented yet.** The project
currently proves project creation, third-person input and camera follow,
`CharacterMovement` collision, textured model importing, asset cooking, and
headless player startup. Its 387-entity scene contains a tiled room, collision
walls and cryo-pod placeholders. The cube/checker art and template pickup
targets are temporary. There are no weapons, enemies, boss encounter, story
sequence, save/checkpoint flow, or finished level art yet.

The first playable milestone is one connected HELIOS-7 room sequence: Kael
wakes in cryo, retrieves the AEGIS Rifle, defeats a Scout, opens the service
route, and reaches the Warden arena. Use greybox geometry and existing engine
systems; keep this game code and content inside this project.

## Controls

- W/A/S/D: move
- Mouse: orbit camera
- Space: jump

## Build and run (Windows)

From the engine repository root, after configuring and building the engine:

```powershell
build\tools\cook\aether_cook.exe games\AETHER-01\AETHER-01.aproject --out build\aether01-cooked
build\player\aether_player.exe --pak build\aether01-cooked\Game.apak
```

For a headless smoke run, add `--headless --frames 120 --report` to the player
command. Generated builds belong under the ignored engine `build/` directory.

## Milestones

1. Replace the template pickup scene with HELIOS-7 cryo and service-room
   greyboxes, readable objectives, and a simple end-of-slice transition.
2. Add one weapon and one enemy using existing engine systems; prove hit,
   damage, death, restart, and feedback in a repeatable encounter.
3. Add the remaining MVP weapons/enemies and the Warden encounter only after
   the first combat room is playable and packaged.
4. Verify save/checkpoint behavior, a clean cook/package/run path, and a
   complete start-to-ending playthrough before calling this an engine proof.

The broader five-act, three-to-five-hour game remains preproduction scope.
See [`../../docs/design/AETHER_01_PROOF_PLAN.md`](../../docs/design/AETHER_01_PROOF_PLAN.md)
for the locked MVP and release gates.
