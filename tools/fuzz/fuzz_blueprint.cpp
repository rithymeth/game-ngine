// Fuzz target: Blueprint loading and compiling (Phase 47 step 1). Arbitrary JSON into a Blueprint, and
// if it loads, through the compiler.
#include "fuzz_common.h"

#include "aether/blueprint/compiler.h"
#include "aether/blueprint/graph.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    const nlohmann::json j = nlohmann::json::parse(data, data + size, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return 0;
    aether::bp::Blueprint blueprint;
    std::string error;
    if (aether::bp::BlueprintFromJson(j, blueprint, &error)) (void)aether::bp::CompileBlueprint(blueprint);
    return 0;
}
