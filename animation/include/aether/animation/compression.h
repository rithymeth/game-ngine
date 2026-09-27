#pragma once

#include "aether/animation/clip.h"

#include <string>
#include <vector>

namespace aether::anim {

// Clip compression (Phase 16 step 1, docs/design/PHASE_SPECS.md §16.1):
// times and values quantized to 16 bits within each track's range, and
// rotations stored as their three smallest components plus the index of
// the dropped one. About 4x smaller than floats; the error is bounded by
// the quantization step (a few 1e-5 of the range, about 1e-4 rad).

std::vector<u8> CompressClip(const AnimationClip& clip);
// False with a reason for data that isn't a compressed clip, or is cut short.
bool DecompressClip(const std::vector<u8>& bytes, AnimationClip& out, std::string* error = nullptr);
// The size of the clip's keys as plain floats, for comparison.
usize RawClipSize(const AnimationClip& clip);

} // namespace aether::anim
