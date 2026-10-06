// Fuzz target: the player's Manifest.json parser over arbitrary text.
#include "fuzz_common.h"

#include "aether/player/game.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    aether::player::GameManifest manifest;
    std::string error;
    aether::player::ParseGameManifest(std::string_view(reinterpret_cast<const char*>(data), size), manifest, &error);
    return 0;
}
