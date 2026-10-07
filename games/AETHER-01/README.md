# AETHER-01: First Contact

This is the playable project scaffold for the AETHER-01 engine proof game. It
is based on the engine's Third Person project template so the first pass uses
the real project, input, scene, Lua, physics, cooker, and standalone-player
workflows. The player currently draws static glTF meshes and textures through
the engine RHI; this remains greybox-only and does not yet render lighting or
skeletal animation.

## Current state

**First connected greybox mission; full MVP remains incomplete.** The project proves
project creation, third-person movement and camera follow,
`CharacterMovement` collision, textured model importing, asset cooking, and
packaged-player rendering/startup. Its 31-entity scene contains Kael, a Scout,
a cryo bay, a gated service route, a Warden arena and an archive terminal. The player,
enemies, and environment use temporary cube/checker greybox art. The AEGIS
prototype uses an aim cone and checks visibility against the room's unrotated
box colliders. Three hits knock the Scout back and defeat it. Scout contact
drains Kael's three health points. The packaged player draws a game-owned HUD
with health, Scout health, ammo/reload state, a Scout objective cue, damage
flash, and hit confirmation. The connected prototype mission is to retrieve
the rifle, defeat the Scout, enter the service route, defeat
the Warden and read the archive terminal to finish. Warden phases remove armor,
expose its core for extra damage and accelerate marked ranged strikes. Red floor
plates show the strike area before damage; move outside to dodge. The archive
reveals that the facility's loss was deliberate. This is brief prototype text,
not a finished cinematic. Lighting, animation rendering, audio, remaining MVP
weapons/enemies, menus and finished level art remain incomplete.
Repeated 960x540 captures pass direct pixel checks on D3D12 and Vulkan. Earlier
partial previews were an image-preview problem: the saved pixels contained the
full room and HUD. CI checks scene, player, health bar and text pixels across
three independent launches. Use an image viewer at original resolution when
reviewing the evidence. Completion of the game slice and release gates remains
open.

Milestone checkpoints use the existing save API and automatically resume on
launch. They preserve rifle/route/boss/ending progress, then restore full health
and ammunition at a safe starting position. An unfinished Warden fight restarts
from full boss health. Failed saves are shown in the HUD. N starts a new game
and deletes this project's checkpoint. Camera obstruction uses the current
level's unrotated box colliders; rotated/other collider shapes remain unsupported.

## Controls

- W/A/S/D: move
- Mouse: orbit camera
- Mouse-left: fire the AEGIS prototype
- E / Gamepad B: retrieve the rifle or read the archive terminal
- R: reload (30 rounds, 1.1-second reload)
- Space: jump
- Gamepad right trigger: fire
- Gamepad X: reload
- R / Gamepad Y after death: reload the last milestone checkpoint
- N: start a new game

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

1. Locally verified: cooked pickup/combat/route/Warden/ending path, checkpoint
   reload, new game, strike warning/damage/dodge and save-error feedback.
2. Complete the locked three-weapon/three-enemy roster and audio/cinematic/menu
   proof using existing systems.
3. Pass clean cook/package/run and the release CI gates, then validate the game
   against the released engine artifact. No release is claimed by local tests.

The broader five-act, three-to-five-hour game remains preproduction scope.
See [`../../docs/design/AETHER_01_PROOF_PLAN.md`](../../docs/design/AETHER_01_PROOF_PLAN.md)
for the locked MVP and release gates.
