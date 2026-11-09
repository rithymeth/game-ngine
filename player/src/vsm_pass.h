#pragma once

#include "shadow_pass.h"
#include "vsm_pages.h"

#include <span>

// Draws virtual shadow map pages (vsm_pages.h) into their physical pages of one depth atlas. Each page is set to the far depth by a triangle
// drawn with an ALWAYS depth test, then the casters whose bounding spheres reach it are drawn with the page's own orthographic matrix
// (VsmPageMatrix) through the same caster shaders as the cascades (shadow_pass.h). Every page of a frame goes in one depth pass. Casters that
// deform (a skinned mesh posed anew each frame) are not drawn here: a page would have to be redrawn every frame with a mesh of tens of thousands
// of vertices in every page it reaches, so the cascades keep them and the pages they overlap are left out of the page table (vsm_pages.h).

namespace aether::player {

class VsmPass {
public:
    VsmPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain);
    ~VsmPass();
    VsmPass(const VsmPass&) = delete;
    VsmPass& operator=(const VsmPass&) = delete;
    // The device draws depth textures and the three pipelines built.
    bool Valid() const { return clear_.IsValid() && opaque_.IsValid() && masked_.IsValid(); }

    // The atlas for a pool of pool_pages_per_side^2 pages, made (or remade, when the size changes) here. False when the device can't make it.
    bool EnsureAtlas(u32 pool_pages_per_side);
    gfx::rhi::SampledTextureHandle Atlas() const { return atlas_; }
    u32 AtlasTexels() const { return atlas_texels_; }

    // Clears and draws `pages` (physical pages of a pool of pool_pages_per_side^2), outside any render pass. Returns the casters drawn over all
    // the pages (the profiler's number). Nothing is recorded for no pages or no atlas.
    u32 DrawPages(gfx::rhi::ICommandList& commands, const VsmLight& light, std::span<const VsmPageDraw> pages, u32 pool_pages_per_side,
                  std::span<const ShadowCaster> casters);
    void Release();

private:
    gfx::rhi::IDevice& device_;
    gfx::rhi::PipelineHandle clear_;
    gfx::rhi::PipelineHandle opaque_;
    gfx::rhi::PipelineHandle masked_;
    gfx::rhi::SampledTextureHandle atlas_;
    u32 atlas_pool_ = 0;
    u32 atlas_texels_ = 0;
};

} // namespace aether::player
