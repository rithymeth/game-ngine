# Stardust Hop

A small 3D platformer made entirely through the Aether MCP server (`aether_mcp_server`): the project, the models,
the sounds, the input bindings, the script and the level were all written with its tools, and the game was cooked,
loaded and played by a bot through the same server.

Hop across fourteen floating platforms, collect the fifteen coins, touch the flags to move your respawn point, and
reach the golden pad to win. Fall off and you come back at the last flag. After a win the level resets by itself.

| Key | Action |
| --- | --- |
| W A S D / left stick | run (relative to the camera) |
| Mouse / right stick | turn the camera |
| Space / A button | jump |
| R | start over |

There is no on-screen text in this engine's player, so the score is drawn in the world: a row of gems floats in the top
left of the view and each one lights up gold as you collect a coin.

## Play it

```
cmake --build <build-dir> --target aether_player
<build-dir>/player/aether_player.exe --pak games/STARDUST-HOP/dist
```

`dist/` is the cooked game (`Game.apak`), made by `tools/ship.py` (the MCP `asset_cook` tool, shipping configuration).

## How it was made

Everything is in `tools/` (Python, talking to the MCP server over stdio; run them from a build that has
`aether_mcp_server`, or set `AETHER_MCP_SERVER`):

| Script | What it does |
| --- | --- |
| `build1.py` | Creates the project file; writes the glTF models (platforms, player, coin, goal, flags ...) with `content_write`; synthesizes the sounds and imports them with `asset_add_files`; makes the sound cues and input bindings with `kit_put`. |
| `build2.py` | Writes `Hopper.luau` and the level scene (`Scenes/Main.ascene`) from `level.py`, then checks the script with `script_check` and `script_check_project`. |
| `ship.py` | Cooks the game into `dist/`. |
| `bot.py` | Cooks, loads the game with `game_load` and plays it: steers the player with `game_input`, steps with `game_step`, reads entities with `game_get_entity` and takes screenshots with `game_screenshot`. |

`level.py` is the level: edit the platform list and rerun `build2.py` to change it.

## Notes

- The player's `CharacterMovement` is the engine's Jolt character controller; platforms are `BoxCollider`s.
- `Hopper.luau` runs the whole game: camera orbit, coin pickup, flags, respawn, the win burst and the reset.
- The scene has no lights: this player lights the scene itself, and does not register light components.
