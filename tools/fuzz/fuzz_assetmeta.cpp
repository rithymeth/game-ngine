// Fuzz target: untrusted .ameta asset database sidecars.
#include "fuzz_common.h"

#include "aether/assets/asset_database.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::assets::AssetMeta meta;
    std::string error;
    (void)aether::assets::LoadAssetMetaFromMemory(std::span<const aether::u8>(data, size), meta, &error);
    return 0;
}
