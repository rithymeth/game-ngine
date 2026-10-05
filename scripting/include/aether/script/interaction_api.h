#pragma once

#include "aether/script/luau_host.h"

// Luau's `Interaction` table (Phase 30 step 7b, docs/design/PHASE_SPECS.md §30.8): the same
// functions as the `Interaction` Blueprint library, on the active InteractionSystem. Present
// only when the interaction kit is built.
//
//   Interaction.Find (interactor, x, y, z, fx, fy, fz)  -> entity or nil   (what can be used from (x,y,z) looking along (fx,fy,fz); 0,0,0 looks all round)
//   Interaction.GetPrompt (target)                      -> string
//   Interaction.CanInteract (interactor, target)        -> boolean
//   Interaction.Interact (interactor, target)           -> boolean (true if it was used)
//   Interaction.SetEnabled (target, enabled) / Reset (target) -> boolean
//
// A target's script hears OnInteract (interactor); a user's hears OnInteractFailed (target, reason),
// the reason a name such as "out_of_range", "missing_tag", "cooldown". A call with a missing or
// wrong-typed argument raises a script error; without an active system the functions return nil / false / "".

namespace aether::script {

void InstallInteractionApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
