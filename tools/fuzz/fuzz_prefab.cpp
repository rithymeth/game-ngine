// Fuzz target: the prefab loader (Phase 47 step 1) over arbitrary JSON.
#include "fuzz_common.h"

#include "aether/scene/prefab.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    const auto json = nlohmann::json::parse(data, data + size, nullptr, /*allow_exceptions=*/false);
    aether::PrefabData prefab;
    std::string error;
    aether::PrefabFromJson(json, prefab, &error);
    return 0;
}
