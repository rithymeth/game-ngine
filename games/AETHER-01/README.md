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
a tiled room, collision walls, a service-door opening, and cryo pods. The player,
Scout, and environment use temporary cube/checker greybox art. The AEGIS
prototype uses an aim cone and checks visibility against the room's unrotated
box colliders. Three hits knock the Scout back and defeat it. Scout contact
drains Kael's three health points. The packaged player draws a game-owned HUD
with health, Scout health, ammo/reload state, a Scout objective cue, damage
flash, and hit confirmation. This is an early combat prototype, not a finished
mission: objective flow, boss encounter, story sequence, save/checkpoint flow,
lighting, animation rendering, and finished level art remain incomplete.
Repeated 960x540 captures pass direct pixel checks on D3D12 and Vulkan. Earlier
partial previews were an image-preview problem: the saved pixels contained the
full room and HUD. CI checks scene, player, health bar and text pixels across
three independent launches. Use an image viewer at original resolution when
reviewing the evidence. Completion of the game slice and release gates remains
open.

The first playable milestone is one connected HELIOS-7 room sequence: Kael
wakes in cryo, retrieves the AEGIS Rifle, defeats a Scout, opens the service
route, and reaches the Warden arena. Use greybox geometry and existing engine
systems; keep this game code and content inside this project.

## Controls

- W/A/S/D: move
- Mouse: orbit camera
- Mouse-left: fire the AEGIS prototype
- R: reload (30 rounds, 1.1-second reload)
- Space: jump
- Gamepad right trigger: fire
- Gamepad X: reload
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

1. Tune and verify the combat loop, then add the AEGIS Rifle pickup and
   service-route objective to make the first
   connected cryo/research sequence completable.
2. Add the remaining MVP weapons/enemies and Warden encounter after the first
   combat room is repeatable and packaged.
3. Verify save/checkpoint behavior, a clean cook/package/run path, and a
   complete start-to-ending playthrough before calling this an engine proof.

The broader five-act, three-to-five-hour game remains preproduction scope.
See [`../../docs/design/AETHER_01_PROOF_PLAN.md`](../../docs/design/AETHER_01_PROOF_PLAN.md)
for the locked MVP and release gates.
