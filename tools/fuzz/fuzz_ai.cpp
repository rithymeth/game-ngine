// Fuzz target: the AI decision data loaders (Phase 47 step 1): utility brains and state trees. The first input
// byte picks the loader; the rest is JSON text. A loaded state tree is also run for a few ticks.
#include "fuzz_common.h"

#include "aether/ai/state_tree.h"
#include "aether/ai/utility.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const auto json = nlohmann::json::parse(data + 1, data + size, nullptr, /*allow_exceptions=*/false);
    std::string error;
    aether::ai::Blackboard blackboard;
    if (data[0] & 1) {
        aether::ai::StateTree tree;
        if (aether::ai::StateTree::FromJson(json, tree, &error) && tree.Start(blackboard)) {
            for (int i = 0; i < 4; ++i) (void)tree.Tick(blackboard);
            tree.Stop(blackboard);
        }
    } else {
        aether::ai::UtilityBrain brain;
        if (aether::ai::UtilityBrain::FromJson(json, brain, &error)) (void)brain.Choose(blackboard);
    }
    return 0;
}
