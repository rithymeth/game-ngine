#pragma once

#include "aether/script/luau_host.h"

// Luau's `Dialogue` table: conversations (`.adialogue`) run on the active DialogueSystem, one per listening entity. Present only when the
// dialogue kit is built.
//
//   Dialogue.Start (listener, dialogue)  -> "ok", or why not: "unknown_dialogue", "dead_entity". Replaces a conversation already running.
//   Dialogue.IsActive (listener)         -> boolean
//   Dialogue.Speaker / Text / Event (listener) -> string for the line now (empty when no conversation runs)
//   Dialogue.ChoiceCount (listener)      -> number of choices the line offers (0: Advance continues)
//   Dialogue.Choice (listener, n)        -> the text of choice n (1 is the first), empty if there is none
//   Dialogue.IsLast (listener)           -> boolean: the line has no next node and no choices, so Advance ends the conversation
//   Dialogue.Advance (listener)          -> "ok" (also when it ended the conversation), "not_active", "needs_choice", "dead_entity"
//   Dialogue.Choose (listener, n)        -> "ok", "bad_choice" (no such choice), "not_active", "dead_entity"; n counts from 1
//   Dialogue.End (listener)              -> nothing; ends the conversation
//   Dialogue.Exists (dialogue)           -> boolean
//
// A call with a missing or wrong-typed argument raises a script error; without an active system the functions return false / 0 / "" /
// "unknown_dialogue".

namespace aether::script {

void InstallDialogueApi(LuauHost& host); // does nothing when the kit isn't built

} // namespace aether::script
