#pragma once

// The player's drawing of a ProceduralTerrain (renderer/components.h): the terrain module's generator, chunk meshes, splatmap and GPU pass
// (terrain/terrain_pass.h) wired to one component. SceneRenderer owns one and draws it inside the scene pass, after the sky and before the
// models, so models and particles are depth-tested against the ground.
//
// Not yet done: the terrain casts and receives no sun shadows and takes no part in the water reflection, its four layers are small generated
// textures (no authored albedo maps), and the sun is the scene's first DirectionalLight's direction and brightness only.

#include "aether/core/base.h"
#include "aether/gfx/rhi/device.h"
#include "aether/math/math.h"
#include "aether/renderer/components.h"
#include "aether/terrain/generator.h"
#include "aether/terrain/renderer.h"
#include "aether/terrain/terrain_pass.h"

#include <memory>

namespace aether::player {

// The generator settings a ProceduralTerrain stands for. `samples` is clamped to the generator's range.
inline terrain::TerrainGenSettings ToGenSettings(const ProceduralTerrain& t) {
    terrain::TerrainGenSettings s;
    s.seed = t.seed;
    s.width = s.height = t.samples < 9 ? 9u : (t.samples > 1025 ? 1025u : t.samples);
    s.cell_size = t.cell_size;
    s.height_scale = t.height_scale;
    s.frequency = t.frequency;
    s.ridge_amount = t.ridge_amount;
    s.sea_level = t.sea_level;
    s.river_min_catchment = t.river_min_catchment;
    s.hydraulic_droplets = t.hydraulic_droplets;
    return terrain::SanitizeSettings(s);
}

class TerrainView {
public:
    TerrainView(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target);
    ~TerrainView();
    TerrainView(const TerrainView&) = delete;
    TerrainView& operator=(const TerrainView&) = delete;

    // Whether the pass compiled: false and nothing is drawn (the log says so once).
    bool Valid() const;

    // Generates and uploads the terrain when `terrain` differs from the one built last (all of it on the first call), and re-meshes the chunks
    // whose level of detail the camera's position changes. True when there is ground to draw. Call it before the frame's render pass begins:
    // it creates textures and buffers on the device. Generation is synchronous: seconds at 513 samples.
    bool Update(const ProceduralTerrain& terrain, const Vec3& camera_position);

    // Records the chunks' draws in the current render pass (what Update prepared). Returns the chunks drawn.
    u32 Draw(gfx::rhi::ICommandList& commands, const Mat4& view_projection, const Vec3& camera_position, const Vec3& sun_direction, f32 sun_intensity);

    struct Stats {
        u32 builds = 0;         // times the terrain has been generated
        f64 build_ms = 0.0;     // the last build, generation and upload
        u32 chunks_drawn = 0;   // the last Draw
        u64 triangles_drawn = 0;
    };
    Stats GetStats() const;

    const terrain::GeneratedTerrain* Ground() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aether::player
