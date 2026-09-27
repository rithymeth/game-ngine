#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::editor {

// One problem to show in the error overlay (script compile errors during hot
// reload, Phase 11 step 5).
struct ErrorOverlayItem {
    std::string source;  // "Spin.luau"
    i32 line = 0;        // 0: no location
    std::string message;
};

// Draws the errors as a stack of red toasts in the bottom-right corner of the
// current viewport ("Spin.luau:12  attempt to index nil"), newest last, at
// most `max_shown` with a "+N more" line. Nothing is drawn for an empty
// list. Returns the index of the toast the user clicked (to open the file at
// that line), or -1.
int DrawErrorOverlay(const std::vector<ErrorOverlayItem>& items, int max_shown = 5);

} // namespace aether::editor
