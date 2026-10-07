// Fuzz target: bounded glTF document parsing without external filesystem reads.
#include "fuzz_common.h"

#include "aether/assets/gltf_loader.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::assets::GltfScene scene;
    (void)aether::assets::LoadGltfFromMemory(std::span<const aether::u8>(data, size), scene);
    return 0;
}
