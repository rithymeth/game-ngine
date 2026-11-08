#include "vsm_pass.h"

#include "aether/core/log.h"

#include <algorithm>

namespace aether::player {

namespace {

// One triangle over the page at the far depth; no pixel shader.
constexpr char kClearShader[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 1.0, 1.0);
}
)";

} // namespace

VsmPass::VsmPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    const gfx::rhi::SampledTextureHandle probe = device.CreateDepthTexture(4, 4);
    if (!probe.IsValid()) return;
    device.DestroyTexture(probe);
    gfx::rhi::PipelineDesc clear;
    clear.hlsl_source = kClearShader;
    clear.depth_target = true;
    clear.depth_always = true;
    clear.cull_back_face = false;
    clear.ps_entry = "";
    clear_ = device.CreatePipeline(clear, swap_chain);
    // The caster pipelines of ShadowPass, built again here (they hold no state): two-sided, a slope-scaled bias against self-shadowing.
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = ShadowCasterShaderSource();
    desc.push_constant_size_bytes = sizeof(ShadowCasterConstants);
    desc.use_vertex_buffer = true;
    desc.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUvTangent;
    desc.depth_target = true;
    desc.cull_back_face = false;
    desc.slope_scaled_depth_bias = 1.5f;
    desc.ps_entry = "";
    opaque_ = device.CreatePipeline(desc, swap_chain);
    desc.ps_entry = "PSMain";
    desc.enable_bindless_textures = true;
    masked_ = device.CreatePipeline(desc, swap_chain);
    if (!Valid()) AETHER_LOG_WARN("PlayerRenderer", "Virtual shadow page pipelines unavailable; the sun's shadows stay on the cascades");
}

VsmPass::~VsmPass() {
    Release();
    for (gfx::rhi::PipelineHandle* pipeline : {&masked_, &opaque_, &clear_}) {
        if (pipeline->IsValid()) device_.DestroyPipeline(*pipeline);
    }
}

void VsmPass::Release() {
    if (atlas_.IsValid()) device_.DestroyTexture(atlas_);
    atlas_ = {};
    atlas_pool_ = atlas_texels_ = 0;
}

bool VsmPass::EnsureAtlas(u32 pool_pages_per_side) {
    pool_pages_per_side = std::clamp(pool_pages_per_side, 1u, 64u);
    if (atlas_.IsValid() && atlas_pool_ == pool_pages_per_side) return true;
    Release();
    const u32 texels = pool_pages_per_side * kVsmPageTexels;
    atlas_ = device_.CreateDepthTexture(texels, texels);
    if (!atlas_.IsValid() || atlas_.index >= gfx::rhi::kMaxBindlessTextures) {
        if (atlas_.IsValid()) device_.DestroyTexture(atlas_);
        atlas_ = {};
        return false;
    }
    atlas_pool_ = pool_pages_per_side;
    atlas_texels_ = texels;
    return true;
}

u32 VsmPass::DrawPages(gfx::rhi::ICommandList& commands, const VsmLight& light, std::span<const VsmPageDraw> pages, u32 pool_pages_per_side,
                       std::span<const ShadowCaster> casters) {
    if (!Valid() || pages.empty() || !EnsureAtlas(pool_pages_per_side) || !commands.BeginDepthPass(atlas_, false)) return 0;
    u32 drawn = 0;
    const f32 size = static_cast<f32>(kVsmPageTexels);
    for (const VsmPageDraw& draw : pages) {
        const i32 x = static_cast<i32>((draw.physical % atlas_pool_) * kVsmPageTexels);
        const i32 y = static_cast<i32>((draw.physical / atlas_pool_) * kVsmPageTexels);
        commands.SetViewportAndScissor({static_cast<f32>(x), static_cast<f32>(y), size, size, 0.0f, 1.0f},
                                       {x, y, x + static_cast<i32>(kVsmPageTexels), y + static_cast<i32>(kVsmPageTexels)});
        commands.BindPipeline(clear_);
        commands.Draw(3);
        const Mat4 page_matrix = VsmPageMatrix(light, draw.page);
        for (const bool masked : {false, true}) {
            bool bound = false;
            for (const ShadowCaster& caster : casters) {
                if (caster.alpha_tested != masked || caster.index_count == 0) continue;
                if (caster.bounds_radius >= 0.0f && !VsmSphereTouchesPage(light, draw.page, caster.bounds_center, caster.bounds_radius)) continue;
                if (!bound) {
                    commands.BindPipeline(masked ? masked_ : opaque_);
                    if (masked) commands.BindBindlessTextures();
                    bound = true;
                }
                ShadowCasterConstants constants;
                constants.light_mvp = page_matrix * caster.model;
                constants.base_color_texture = caster.base_color_texture < gfx::rhi::kMaxBindlessTextures ? caster.base_color_texture : 255u;
                constants.alpha = caster.alpha;
                constants.alpha_cutoff = caster.alpha_cutoff;
                commands.BindVertexBuffer(caster.vertices, 48);
                commands.BindIndexBuffer(caster.indices, gfx::rhi::IndexFormat::UInt32);
                commands.SetPushConstants(&constants, sizeof(constants));
                commands.DrawIndexed(caster.index_count);
                ++drawn;
            }
        }
    }
    commands.EndRenderPass();
    return drawn;
}

} // namespace aether::player
