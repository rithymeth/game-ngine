#pragma once

// Draws a VegetationBatch (vegetation_batch.h): one pipeline, a 24-colour palette texture, and one indexed draw per visible chunk-and-kind mesh,
// culled by the view frustum and by a distance per kind (grass goes first, trees last). A vertex shader sways what the wind moves.

#include "aether/gfx/rhi/device.h"
#include "aether/terrain/terrain_pass.h"
#include "vegetation_batch.h"
#include "view_cull.h"

#include <memory>
#include <vector>

namespace aether::player {

// Beyond this many metres from the camera (to the nearest point of a chunk's plants) a kind is not drawn.
f32 VegetationDrawDistance(PlantKind kind);

// The vegetation HLSL (VSMain, PSMain). With `hdr_target` the pixel shader writes linear light for the scene colour target.
std::string VegetationShaderSource(bool hdr_target);

class VegetationPass {
public:
    VegetationPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain, bool hdr_target);
    ~VegetationPass();
    VegetationPass(const VegetationPass&) = delete;
    VegetationPass& operator=(const VegetationPass&) = delete;

    bool Valid() const { return pipeline_.IsValid() && palette_.IsValid(); }

    // Replaces what is drawn with `batch`'s meshes (freeing the last ones). Returns how many meshes have buffers.
    u32 Upload(const VegetationBatch& batch);
    void Clear();

    struct DrawStats {
        u32 meshes_drawn = 0;
        u32 meshes_culled = 0;
        u64 triangles_drawn = 0;
    };
    // Records the visible meshes in the current render pass. `time_seconds` drives the sway.
    DrawStats Draw(gfx::rhi::ICommandList& commands, const Mat4& view_projection, const ViewFrustum& frustum, const Vec3& camera_position,
                   const Vec3& sun_direction, f32 sun_intensity, f32 time_seconds);

    u32 MeshCount() const { return static_cast<u32>(meshes_.size()); }
    u64 GpuBytes() const { return gpu_bytes_; }

private:
    struct Mesh {
        PlantKind kind;
        Vec3 centre;
        f32 radius;
        Vec3 bounds_min, bounds_max;
        u64 vertex_buffer = 0, index_buffer = 0;
        u32 index_count = 0;
    };
    gfx::rhi::IDevice& device_;
    terrain::RhiTerrainGpu gpu_;
    gfx::rhi::PipelineHandle pipeline_;
    gfx::rhi::SampledTextureHandle palette_;
    std::vector<Mesh> meshes_;
    u64 gpu_bytes_ = 0;
};

} // namespace aether::player
