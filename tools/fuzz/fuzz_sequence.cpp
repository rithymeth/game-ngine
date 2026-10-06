// Fuzz target: the level sequence loader over arbitrary JSON.
#include "fuzz_common.h"

#include "aether/sequencer/sequence.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    const auto json = nlohmann::json::parse(data, data + size, nullptr, /*allow_exceptions=*/false);
    aether::seq::LevelSequence sequence;
    std::string error;
    aether::seq::SequenceFromJson(json, sequence, &error);
    return 0;
}
