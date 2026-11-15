# Checkpoints

The checkpoints kit gives a game respawn points: you place `Checkpoint` components in a scene, the kit notices when the player reaches one and
remembers the last, and a script can put the player back there. It is an optional part of the build (`AETHER_KIT_CHECKPOINTS`, on by default) and
needs no other kit. What counts as "dying", and when to respawn, is your game's business.

## The component

| Field | Meaning |
| --- | --- |
| `name` | what scripts and the save call it; a checkpoint with no name does nothing |
| `enabled` | switched off, it is never reached |
| `radius` | how close an activator has to come, in metres (0.01 to 10,000; default 3) |
| `activator_tag` | which entities reach it: the ones carrying this tag (default `Player`) |
| `respawn_offset` | where `Respawn` puts an entity, relative to the checkpoint (default 0.5 m up, so nobody lands in the floor) |

A checkpoint is reached on the frame an activator is within its `radius` while it is not already the last one. Reaching a different checkpoint
makes *that* the last; going back to an earlier one makes it the last again.

## What is remembered

The state is in the game's **save bag**, so it goes into and out of a save slot with the rest of the game (`SaveGames.SaveToSlot` /
`LoadFromSlot`), and a scene change does not touch it:

| Key | Holds |
| --- | --- |
| `checkpoint.last` | the name of the last checkpoint reached |
| `checkpoint.scene` | the scene it is in, so a script can load it |
| `checkpoint.<name>` | 1, for each one ever reached |

Loading a slot does not announce anything: the checkpoint's script hears `OnCheckpoint` only when one is reached in play. A new game
(`SaveGames.CreateSaveObject()`, which empties the bag) forgets them.

## From a script

The checkpoint's own script hears `OnCheckpoint` (the entity that reached it) each time it becomes the last one:

```lua
function Gate:OnCheckpoint(who)
    UI.SetText("ToastText", "Checkpoint saved")
    SaveGames.SaveToSlot("autosave")
end
```

When the player dies, put them back (loading the checkpoint's scene first if it is another one):

```lua
if Checkpoints.GetScene() ~= Scene.Current() then
    Scene.Load(Checkpoints.GetScene())   -- then Respawn in the new scene's first frames
else
    Checkpoints.Respawn(player)
end
```

| Call | Result |
| --- | --- |
| `Checkpoints.Last()` / `GetScene()` | the last checkpoint's name and scene (`""` when none) |
| `Checkpoints.IsReached(name)` | it was reached at some time |
| `Checkpoints.Exists(name)` | this scene has a checkpoint by that name |
| `Checkpoints.Activate(name)` | makes it the last one at once; `false` if this scene has none by that name |
| `Checkpoints.Respawn(entity)` | `"ok"`, `"no_checkpoint"`, `"not_in_scene"` (the last one is in another scene), `"dead_entity"` or `"no_transform"` |

`Respawn` moves the entity's `Transform` (position to the checkpoint's plus `respawn_offset`, rotation to the checkpoint's). The engine's character
system (`CharacterMovement`) takes a changed `Transform` as a teleport, so a player moved this way ends up where it was put. Anything else a physics
body drives keeps its velocity: the kit does not know what moves it, so such an entity's own script should reset that afterwards.
