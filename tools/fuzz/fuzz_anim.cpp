// Fuzz target: the animation graph, blend space and montage loaders (the first input byte picks which).
#include "fuzz_common.h"

#include "aether/animation/anim_graph.h"
#include "aether/animation/blend_space.h"
#include "aether/animation/montage.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const auto json = nlohmann::json::parse(data + 1, data + size, nullptr, /*allow_exceptions=*/false);
    std::string error;
    switch (data[0] % 3) {
    case 0: {
        aether::anim::AnimGraph graph;
        aether::anim::AnimGraphFromJson(json, graph, &error);
        break;
    }
    case 1: {
        aether::anim::BlendSpace space;
        aether::anim::BlendSpaceFromJson(json, space, &error);
        break;
    }
    default: {
        aether::anim::Montage montage;
        aether::anim::MontageFromJson(json, montage, &error);
        break;
    }
    }
    return 0;
}
