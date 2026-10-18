// Fuzz target: complete, filesystem-free model import result generation.
#include "fuzz_common.h"

#include "aether/assets/model_importer.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::assets::ModelImporter importer;
    (void)importer.ImportFromMemory(std::span<const aether::u8>(data, size), importer.DefaultSettings());
    return 0;
}
