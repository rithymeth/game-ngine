#pragma once

#include "aether/script/luau_host.h"

// Luau's `UI` table: change what the game's on-screen UI shows, by widget name. The widgets are the ones in the layouts (.aui) that
// WidgetComponent entities show, on the active UISystem; a name that no layout holds does nothing (SetX) or returns nil (GetX), so a script
// need not know which entity shows which layout.
//
//   UI.SetText (widget, text)            a Text, a text box, or the first Text inside a button or panel
//   UI.GetText (widget)         -> string or nil
//   UI.SetValue (widget, number)         a slider's value, a progress bar's percent (0..1), a toggle (0 / 1), a dropdown's choice
//   UI.GetValue (widget)        -> number or nil
//   UI.SetVisible (widget, boolean)      shown, or collapsed (takes no space)
//   UI.SetEnabled (widget, boolean)
//   UI.Exists (widget)          -> boolean
//   UI.WasClicked (widget)      -> boolean   a button (or any control) was clicked since the last frame; keys, the pad or the pointer
//   UI.ValueChanged (widget)    -> number or nil   a slider, toggle or dropdown changed: its new value (1 / 0 for a toggle)
//
// Every function returns whether a widget of that name was found (Set*, Exists) or the value (Get*). A wrong-typed argument raises a script error.

namespace aether::script {

void InstallUiApi(LuauHost& host);

} // namespace aether::script
