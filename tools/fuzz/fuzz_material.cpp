// Fuzz target: the material graph and material instance loaders (the first input byte picks which).
#include "fuzz_common.h"

#include "aether/renderer/material.h"
#include "aether/renderer/material_instance.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const auto json = nlohmann::json::parse(data + 1, data + size, nullptr, /*allow_exceptions=*/false);
    std::string error;
    if (data[0] & 1) {
        aether::mat::MaterialInstance instance;
        aether::mat::InstanceFromJson(json, instance, &error);
    } else {
        aether::mat::Material material;
        aether::mat::MaterialFromJson(json, material, &error);
    }
    return 0;
}
