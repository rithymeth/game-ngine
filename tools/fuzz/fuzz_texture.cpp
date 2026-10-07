// Fuzz target: bounded in-memory decoding and processing for texture assets.
#include "fuzz_common.h"

#include "aether/assets/texture_importer.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::assets::TextureImporter importer;
    (void)importer.ImportFromMemory(std::span<const aether::u8>(data, size), importer.DefaultSettings());
    return 0;
}
