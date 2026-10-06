// Fuzz target: scene loading (Phase 47 step 1). The JSON scene loader and the binary scene loader
// (the first input byte picks which) over arbitrary bytes into a fresh world.
#include "fuzz_common.h"

#include "aether/ecs/world.h"
#include "aether/scene/serialization.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    aether::World world;
    const std::span<const aether::u8> bytes(data + 1, size - 1);
    if (data[0] & 1) aether::LoadSceneJsonFromMemory(world, bytes, "fuzz");
    else aether::LoadSceneFromMemory(world, bytes, "fuzz");
    return 0;
}
