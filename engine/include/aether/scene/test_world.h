#pragma once

// @stability: experimental
//
// A generated test world (Phase 42 step 3, docs/design/UPGRADE_PLAN.md): a deterministic city-block grid of
// entities for the flythrough hitch test (42.7) and for scale tests. The same parameters always give the same
// world, entity for entity, so two runs (or two machines) compare like with like.

#include "aether/core/base.h"
#include "aether/ecs/world.h"

#include <string>

namespace aether::scene {

struct TestWorldParams {
    u32 seed = 1;
    u32 blocks_x = 16;       // grid size, 1 to 1024 per side
    u32 blocks_z = 16;
    f32 block_spacing = 12.0f;
    u32 props_per_block = 6; // 0 to 256 scattered around each block's building
    std::string model = "Models/box.glb"; // every entity references this asset path (it need not exist for CPU tests)
};

struct TestWorldStats {
    usize entities = 0;
    f32 extent_x = 0.0f; // the world's size, for placing a flythrough camera
    f32 extent_z = 0.0f;
};

// Adds the world to `world`. False (with a reason) for out-of-range parameters; `world` is untouched then.
bool GenerateTestWorld(World& world, const TestWorldParams& params, TestWorldStats* stats = nullptr,
                       std::string* error = nullptr);

} // namespace aether::scene
