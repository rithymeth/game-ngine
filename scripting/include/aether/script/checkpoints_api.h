#pragma once

#include "aether/script/luau_host.h"

// Luau's `Checkpoints` table: the checkpoint kit's Checkpoint components, with the last one kept in the game's save bag (so it is saved and loaded with
// the slot, and a scene change does not touch it). Present only when the kit is built.
//
//   Checkpoints.Last ()              -> name of the last checkpoint reached ("" when none)
//   Checkpoints.GetScene ()          -> the scene that checkpoint is in ("" when none, or the host did not say): Scene.Load it before Respawn
//   Checkpoints.IsReached (name)     -> boolean: it was reached at some time
//   Checkpoints.Exists (name)        -> boolean: this scene has a checkpoint with that name
//   Checkpoints.Activate (name)      -> boolean: makes it the last one at once (false if this scene has none by that name)
//   Checkpoints.Respawn (entity)     -> "ok", or why not: "no_checkpoint" (none reached yet), "not_in_scene" (the last one is in another scene),
//                                       "dead_entity", "no_transform". The entity goes to the checkpoint's position plus its respawn_offset, facing as it faces
//
// The checkpoint's own script hears OnCheckpoint (the entity that reached it, a null entity when a script activated it) each time it becomes the last one.
// A call with a missing or wrong-typed argument raises a script error; without an active system the functions return false / "" / "no_checkpoint".

namespace aether::script {

void InstallCheckpointsApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
