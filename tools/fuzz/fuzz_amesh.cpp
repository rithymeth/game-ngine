// Fuzz target: the shipped cooked mesh container (.amesh), which the runtime
// loads out of .apak archives. `fuzz_mesh` covers the importer's AMSH container;
// this is the cooker's output (LODs, meshlets, HLOD proxies).
#include "fuzz_common.h"

#include "aether/cook/mesh_cook.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::cook::CookedMesh mesh;
    (void)aether::cook::LoadAmesh(std::span<const aether::u8>(data, size), mesh);
    return 0;
}
