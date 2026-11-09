#pragma once

#include "aether/core/base.h"

#include <algorithm>
#include <cstdlib>

// How many worker threads the player skins big meshes with (the render thread works too, so the threads used are one more). Skinning is
// per-vertex and independent, so it scales with the cores it is given: one 192,562-vertex character took 3.8 ms to skin with 2 workers, 2.5
// with 4, 1.9 with 6 and 1.7 with 7 (8 cores, 16 threads), and the speed-up falls off beyond that.

namespace aether::player {

// Half the logical threads (a physical core each on a machine with hyper-threading) less the render thread, between 1 and 7: 7 on 16 threads, 3
// on 8, 1 on 4. A machine with two threads or fewer skins on the render thread alone (0), and an unknown count (0 threads reported) keeps the
// old 2 workers. `override_text` is AETHER_SKIN_WORKERS: a number (0 skins on the render thread alone, 1 to 32 that many workers); anything
// else is ignored.
inline u32 SkinWorkerCount(u32 hardware_threads, const char* override_text = nullptr) {
    if (override_text != nullptr && *override_text != '\0') {
        char* end = nullptr;
        const long value = std::strtol(override_text, &end, 10);
        if (end != override_text && *end == '\0' && value >= 0) return static_cast<u32>(std::min<long>(value, 32));
    }
    if (hardware_threads == 0) return 2;
    if (hardware_threads <= 2) return 0;
    return std::clamp<u32>(hardware_threads / 2 - 1, 1u, 7u);
}

} // namespace aether::player
