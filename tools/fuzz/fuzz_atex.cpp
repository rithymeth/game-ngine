// Fuzz target: the cooked texture container (ATEX) decoder. `fuzz_texture` covers
// importing a source image; this covers the other direction -- the container the
// runtime actually reads, which arrives from .apak archives and is therefore
// untrusted input. Must never crash, hang or read out of bounds.
#include "fuzz_common.h"

#include "aether/assets/texture_importer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::assets::TextureData texture;
    (void)aether::assets::DecodeTextureData(std::vector<aether::u8>(data, data + size), texture);
    return 0;
}
