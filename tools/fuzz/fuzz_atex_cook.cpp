// Fuzz target: the shipped cooked texture container (.atex), which the runtime
// loads out of .apak archives. `fuzz_texture` covers importing a source image and
// `fuzz_atex` the importer's ATEX container; this is the cooker's output (BC/ASTC
// blocks plus its mip chain).
#include "fuzz_common.h"

#include "aether/cook/texture_cook.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::cook::CookedTexture texture;
    (void)aether::cook::LoadAtex(std::span<const aether::u8>(data, size), texture);
    return 0;
}
