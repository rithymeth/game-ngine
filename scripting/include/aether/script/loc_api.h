#pragma once

#include "aether/script/luau_host.h"

// Luau's `Localization` table (Phase 29 step 2, docs/design/PHASE_SPECS.md
// §29.2): the same functions as the Blueprint library (aether::loc::Localize),
// on the active Localization:
//
//   Localization.GetText (key, default)             -> string
//   Localization.Format (key, default, name, value) -> string   (value: a number or a string)
//   Localization.HasText (key)                      -> boolean
//   Localization.SetLanguage (language)
//   Localization.GetLanguage ()                     -> string
//
// Without an active Localization they return the default (or the key).
// A call with a missing or wrong-typed argument raises a script error.

namespace aether::script {

void InstallLocApi(LuauHost& host);

} // namespace aether::script
