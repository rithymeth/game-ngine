#pragma once

#include "aether/core/base.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::ui {

// Bidirectional text, the small version (Phase 29 step 6, §29.6): enough to put
// Hebrew (and any right-to-left script that needs no letter joining) in the
// right order. It follows the shape of the Unicode algorithm (UAX #9) without
// embeddings, overrides or isolates:
//  - the base direction is the first strong character's (right-to-left for a
//    Hebrew or Arabic letter, else left-to-right);
//  - Hebrew and Arabic letters are right-to-left, letters of other scripts and
//    digits left-to-right (so a number inside Hebrew keeps its order);
//  - spaces and punctuation take the direction around them, else the base;
//  - runs are then reversed from the deepest level up, and brackets in a
//    right-to-left run are mirrored.
// Arabic letters still draw as separate (isolated) forms: joining them needs
// shaping, which this doesn't do.

bool IsRightToLeft(u32 codepoint); // a Hebrew or Arabic letter or mark (and their presentation forms)

// True if any code point of `text` is right-to-left.
bool HasRightToLeft(const std::vector<u32>& text);

// The code points of one line in the order they are drawn, left to right.
// Text with no right-to-left character comes back as it is.
std::vector<u32> ReorderVisual(const std::vector<u32>& logical);

// The base direction of `text` (true: right-to-left).
bool BaseDirectionIsRtl(const std::vector<u32>& text);

} // namespace aether::ui
