#include "aether/scene/test_world.h"

#include "aether/scene/components.h"

#include <cmath>
#include <cstring>

namespace aether::scene {

namespace {

// A small, portable generator (SplitMix64), so the world doesn't depend on the standard library's engines.
struct Rng {
    u64 state;
    u64 Next() {
        state += 0x9E3779B97F4A7C15ull;
        u64 z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    f32 Unit() { return static_cast<f32>(Next() >> 40) / static_cast<f32>(1u << 24); } // [0, 1)
};

bool Fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

} // namespace

bool GenerateTestWorld(World& world, const TestWorldParams& p, TestWorldStats* stats, std::string* error) {
    if (p.blocks_x == 0 || p.blocks_x > 1024 || p.blocks_z == 0 || p.blocks_z > 1024)
        return Fail(error, "blocks_x and blocks_z must be 1 to 1024");
    if (p.props_per_block > 256) return Fail(error, "props_per_block must be at most 256");
    if (!(p.block_spacing > 0.0f && p.block_spacing < 1.0e5f)) return Fail(error, "block_spacing must be positive");
    if (p.model.size() >= sizeof(ModelRenderer{}.asset_path)) return Fail(error, "model path is too long");

    ModelRenderer renderer;
    SetModelPath(renderer, p.model);

    Rng rng{static_cast<u64>(p.seed) * 0x2545F4914F6CDD1Dull + 1};
    usize count = 0;
    for (u32 bz = 0; bz < p.blocks_z; ++bz) {
        for (u32 bx = 0; bx < p.blocks_x; ++bx) {
            const Vec3 origin(static_cast<f32>(bx) * p.block_spacing, 0.0f, static_cast<f32>(bz) * p.block_spacing);
            const f32 height = 2.0f + rng.Unit() * 18.0f;
            Transform building;
            building.position = origin + Vec3(0.0f, height * 0.5f, 0.0f);
            building.scale = Vec3(p.block_spacing * 0.4f, height, p.block_spacing * 0.4f);
            world.CreateEntity(building, renderer);
            ++count;
            for (u32 i = 0; i < p.props_per_block; ++i) {
                Transform prop;
                prop.position = origin + Vec3((rng.Unit() - 0.5f) * p.block_spacing * 0.9f, 0.25f,
                                              (rng.Unit() - 0.5f) * p.block_spacing * 0.9f);
                prop.rotation = Quaternion::FromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), rng.Unit() * 6.2831853f);
                prop.scale = Vec3(0.5f, 0.5f, 0.5f);
                world.CreateEntity(prop, renderer);
                ++count;
            }
        }
    }
    if (stats) {
        stats->entities = count;
        stats->extent_x = static_cast<f32>(p.blocks_x) * p.block_spacing;
        stats->extent_z = static_cast<f32>(p.blocks_z) * p.block_spacing;
    }
    return true;
}

} // namespace aether::scene
