# AETHER-01: First Contact

This is the playable project scaffold for the AETHER-01 engine proof game. It
is based on the engine's Third Person project template so the first pass uses
the real project, input, scene, Lua, physics, cooker, and standalone-player
workflows. The player currently draws static glTF meshes and textures through
the engine RHI; this remains greybox-only and does not yet render lighting or
skeletal animation.

## Current state

**Early combat prototype; not a complete playable slice.** The project proves
project creation, third-person movement and camera follow,
`CharacterMovement` collision, textured model importing, asset cooking, and
packaged-player rendering/startup. Its 15-entity scene contains Kael, a Scout,
a tiled room, collision walls, a service-door opening, and cryo pods. The player, Scout,
and environment use temporary cube/checker greybox art. The AEGIS prototype
uses an aim cone and checks visibility against the room's unrotated box
colliders. Three hits knock the Scout back and defeat it. Scout contact drains
Kael's three health points; press R or gamepad Y after death to restart the
room. The prototype still lacks a visible HUD, ammo, hit/damage indicators,
objective flow, boss encounter, story sequence, save/checkpoint flow, and
finished level art.

The first playable milestone is one connected HELIOS-7 room sequence: Kael
wakes in cryo, retrieves the AEGIS Rifle, defeats a Scout, opens the service
route, and reaches the Warden arena. Use greybox geometry and existing engine
systems; keep this game code and content inside this project.

## Controls

- W/A/S/D: move
- Mouse: orbit camera
- Mouse-left: fire the AEGIS prototype
- Space: jump
- Gamepad right trigger: fire
- R after death: restart the room
- Gamepad Y after death: restart the room

## Build and run (Windows)

From the engine repository root, after configuring and building the engine:

```powershell
build\tools\cook\aether_cook.exe games\AETHER-01\AETHER-01.aproject --out build\aether01-cooked
build\player\aether_player.exe --pak build\aether01-cooked\Game.apak
```

For a headless smoke run, add `--headless --frames 120 --report` to the player
command. To capture the rendered window after 10 frames, run:

```powershell
build\player\aether_player.exe --pak build\aether01-cooked\Game.apak --frames 10 --size 960x540 --screenshot build\aether01-preview.bmp
```

Generated builds belong under the ignored engine `build/` directory.

## Milestones

1. Add a HUD and damage/hit feedback so health, Scout health, ammo, and danger
   are readable during the current three-hit encounter.
2. Add the AEGIS Rifle pickup and service-route objective, then make the first
   connected cryo/research sequence completable.
3. Add the remaining MVP weapons/enemies and Warden encounter after the first
   combat room is repeatable and packaged.
4. Verify save/checkpoint behavior, a clean cook/package/run path, and a
   complete start-to-ending playthrough before calling this an engine proof.

The broader five-act, three-to-five-hour game remains preproduction scope.
See [`../../docs/design/AETHER_01_PROOF_PLAN.md`](../../docs/design/AETHER_01_PROOF_PLAN.md)
for the locked MVP and release gates.
