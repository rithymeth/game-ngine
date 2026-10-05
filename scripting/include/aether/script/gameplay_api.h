#pragma once

#include "aether/script/luau_host.h"

// Luau's gameplay tables (Phase 30 step 5, docs/design/PHASE_SPECS.md §30.5): the
// same functions as the Blueprint libraries (aether::gas), on the active systems.
// Entities are the usual entity values; effect and ability handles are numbers
// (0 means none). Without an active system they return false / 0 / the default.
//
//   GameplayTags.IsValid (tag) / Matches (tag, parent) / MatchesExact (tag, other) / GetParent (tag) / GetDepth (tag)
//   Attributes.Get / GetBase / GetMin / GetMax (entity, name, default)   -> number
//   Attributes.Has (entity, name)                                        -> boolean
//   Attributes.Define (entity, name, base, min, max) / SetBase / AddBase (entity, name, value)
//   Effects.Apply (target, effect, source)    -> handle (0 for an instant or rejected effect)
//   Effects.Remove (handle) / RemoveByTag (target, tag) / HasActive (target, effect) / GetActiveCount (target)
//   Abilities.Grant / Revoke / CanActivate / IsActive / IsGranted (owner, ability)
//   Abilities.TryActivate (owner, ability)    -> handle (0 on failure)
//   Abilities.Commit / End / Cancel (handle)
//
// An entity's script hears the changes as methods, when it defines them:
//   OnAttributeChanged (name, old, new)        OnEffectApplied / OnEffectRemoved (effect, handle)
//   OnAbilityActivated (ability, handle)       OnAbilityEnded (ability, handle, cancelled)
//   OnAbilityFailed (ability, reason)
// A call with a missing or wrong-typed argument raises a script error.

namespace aether::script {

void InstallGameplayApi(LuauHost& host);

} // namespace aether::script
