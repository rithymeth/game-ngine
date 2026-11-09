#include "shadow_pass.h"

#include "aether/core/log.h"
#include "aether/gfx/rhi/command_list.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace aether::player {

namespace {

constexpr char kCasterShader[] = R"(
struct PushConstants {
    float4x4 g_LightMvp;
    uint g_Texture;
    float g_Alpha;
    float g_Cutoff;
    float g_Padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[128] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(g_PC.g_LightMvp, float4(input.position, 1.0));
    output.uv = input.uv;
    return output;
}

// Alpha-tested casters only (opaque ones have no pixel shader): the same cutoff the scene shader clips at.
void PSMain(PSInput input) {
    float alpha = g_PC.g_Alpha;
    if (g_PC.g_Texture < 128) alpha *= g_Textures[g_PC.g_Texture].Sample(g_Sampler, input.uv).a;
    clip(alpha - g_PC.g_Cutoff);
}
)";

} // namespace

const char* ShadowCasterShaderSource() { return kCasterShader; }

void ShadowAtlasSize(u32 resolution, u32 count, u32& width, u32& height) {
    count = std::clamp(count, 1u, 4u);
    width = resolution * std::min(count, 2u);
    height = resolution * (count > 2 ? 2u : 1u);
}

void ShadowCascadeOrigin(u32 resolution, u32 cascade, u32& x, u32& y) {
    x = resolution * (cascade % 2);
    y = resolution * (cascade / 2);
}

std::vector<ShadowCascade> FitShadowCascades(const View& view, const Vec3& light_direction, u32 resolution, u32 count, f32 distance) {
    CascadeSettings settings;
    settings.count = std::clamp(count, 0u, 4u);
    settings.resolution = resolution;
    settings.shadow_distance = distance > 0.0f ? distance : 100.0f;
    return ComputeCascades(view, light_direction, settings);
}

ShadowFrameData PackShadowFrameData(const std::vector<ShadowCascade>& cascades, u32 resolution, u32 atlas_index) {
    ShadowFrameData out;
    if (cascades.empty() || resolution == 0 || atlas_index >= gfx::rhi::kMaxBindlessTextures) return out;
    const usize count = std::min<usize>(cascades.size(), 4);
    for (usize i = 0; i < count; ++i) {
        const ShadowCascade& c = cascades[i];
        std::memcpy(out.view_projection[i], &c.view_projection, sizeof(out.view_projection[i]));
        u32 x = 0, y = 0;
        ShadowCascadeOrigin(resolution, static_cast<u32>(i), x, y);
        out.cascade[i][0] = static_cast<f32>(x);
        out.cascade[i][1] = static_cast<f32>(y);
        out.cascade[i][2] = c.texel_size;
        // The orthographic depth range: OrthographicRH(…, 0, far) with far = the distance from the light's eye to the sphere's far side.
        const f32 depth_range = 1.0f / std::max(c.projection.cols[2].z == 0.0f ? 1.0f : -c.projection.cols[2].z, 1e-6f);
        out.cascade[i][3] = 1.0f / depth_range;
    }
    out.info[0] = atlas_index;
    out.info[1] = static_cast<u32>(count);
    out.info[2] = resolution;
    return out;
}

bool CasterTouchesCascade(const ShadowCaster& caster, const ShadowCascade& cascade) {
    if (!(caster.bounds_radius >= 0.0f)) return true;
    const Vec4 p = cascade.view * Vec4(caster.bounds_center.x, caster.bounds_center.y, caster.bounds_center.z, 1.0f);
    const f32 r = caster.bounds_radius, half = cascade.radius;
    if (std::fabs(p.x) > half + r || std::fabs(p.y) > half + r) return false;
    // The light looks down -Z: depth is -z. Beyond the far plane it can't shadow anything in the cascade; nearer is clamped, so kept.
    const f32 far_plane = cascade.projection.cols[2].z == 0.0f ? 0.0f : 1.0f / -cascade.projection.cols[2].z;
    return -p.z - r <= far_plane;
}

namespace {

struct Point2 {
    f32 x, y;
};
f32 Cross2(const Point2& o, const Point2& a, const Point2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

} // namespace

namespace {

// The convex outline (counter-clockwise, at most 8 corners) of eight world points seen in a cascade's light plane, and the farthest depth along the
// light any of them has.
void LightOutline(const Vec3 corners[8], const Mat4& light_view, f32 hull[16][2], u32& hull_count, f32& max_depth) {
    Point2 points[8];
    max_depth = -3.4e38f;
    for (int i = 0; i < 8; ++i) {
        const Vec4 v = light_view * Vec4(corners[i].x, corners[i].y, corners[i].z, 1.0f);
        points[i] = {v.x, v.y};
        max_depth = std::max(max_depth, -v.z);
    }
    std::sort(points, points + 8, [](const Point2& a, const Point2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    // Andrew's monotone chain.
    Point2 chain[17];
    usize k = 0;
    for (int i = 0; i < 8; ++i) {
        while (k >= 2 && Cross2(chain[k - 2], chain[k - 1], points[i]) <= 0.0f) --k;
        chain[k++] = points[i];
    }
    for (int i = 6, lower = static_cast<int>(k) + 1; i >= 0; --i) {
        while (k >= static_cast<usize>(lower) && Cross2(chain[k - 2], chain[k - 1], points[i]) <= 0.0f) --k;
        chain[k++] = points[i];
    }
    const usize count = std::min<usize>(k > 1 ? k - 1 : k, 16);
    for (usize i = 0; i < count; ++i) {
        hull[i][0] = chain[i].x;
        hull[i][1] = chain[i].y;
    }
    hull_count = static_cast<u32>(count);
}

// Whether a circle at `centre` of radius `r` touches the convex outline: inside it, or within r of an edge.
bool CircleTouchesOutline(const f32 hull[16][2], u32 n, const Point2& centre, f32 r) {
    if (n >= 3) {
        bool inside = true;
        for (u32 i = 0; i < n && inside; ++i) {
            const Point2 a{hull[i][0], hull[i][1]};
            const Point2 b{hull[(i + 1) % n][0], hull[(i + 1) % n][1]};
            if (Cross2(a, b, centre) < 0.0f) inside = false;
        }
        if (inside) return true;
    }
    for (u32 i = 0; i < n; ++i) {
        const Point2 a{hull[i][0], hull[i][1]};
        const Point2 b{hull[(i + 1) % n][0], hull[(i + 1) % n][1]};
        const f32 dx = b.x - a.x, dy = b.y - a.y;
        const f32 length_squared = dx * dx + dy * dy;
        f32 t = length_squared > 0.0f ? ((centre.x - a.x) * dx + (centre.y - a.y) * dy) / length_squared : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const f32 ex = a.x + dx * t - centre.x, ey = a.y + dy * t - centre.y;
        if (ex * ex + ey * ey <= r * r) return true;
    }
    return false;
}

} // namespace

CascadeReceivers CascadeReceiverRegion(const View& view, const std::vector<ShadowCascade>& cascades, usize index) {
    CascadeReceivers out;
    if (index >= cascades.size()) return out;
    const f32 near_depth = cascades[index].split_near;
    const f32 far_depth = cascades.back().split_far;
    if (!(far_depth > near_depth)) return out;
    Vec3 corners[8];
    FrustumSliceCorners(view, near_depth, far_depth, corners);
    LightOutline(corners, cascades[index].view, out.hull, out.hull_count, out.max_depth);
    out.valid = out.hull_count > 0;
    if (index > 0) {
        // The slice before: what this cascade may be handed from the edge frame of the previous map.
        const ShadowCascade& previous = cascades[index - 1];
        FrustumSliceCorners(view, previous.split_near, previous.split_far, corners);
        LightOutline(corners, cascades[index].view, out.band_hull, out.band_hull_count, out.band_max_depth);
        out.previous_view = previous.view;
        // The shader tests the pixel two texels clear of the edge, after a normal offset of up to two more; five covers the snap as well.
        out.previous_inner_half = previous.radius - 5.0f * previous.texel_size;
    }
    return out;
}

bool CasterReachesReceivers(const ShadowCaster& caster, const ShadowCascade& cascade, const CascadeReceivers& receivers) {
    if (!(caster.bounds_radius >= 0.0f) || !receivers.valid) return true;
    const Vec4 p = cascade.view * Vec4(caster.bounds_center.x, caster.bounds_center.y, caster.bounds_center.z, 1.0f);
    const f32 r = caster.bounds_radius + 4.0f * cascade.texel_size;
    const Point2 centre{p.x, p.y};
    // The light looks down -Z, so depth is -z. A caster beyond the farthest receiver along the light shadows none of them.
    if (-p.z - r <= receivers.max_depth && CircleTouchesOutline(receivers.hull, receivers.hull_count, centre, r)) return true;
    if (receivers.band_hull_count == 0 || -p.z - r > receivers.band_max_depth) return false;
    if (!CircleTouchesOutline(receivers.band_hull, receivers.band_hull_count, centre, r)) return false;
    // The band's pixels are only those in the previous map's edge frame: the caster must reach outside that map's inner square.
    const Vec4 q = receivers.previous_view * Vec4(caster.bounds_center.x, caster.bounds_center.y, caster.bounds_center.z, 1.0f);
    const f32 half = receivers.previous_inner_half;
    return !(std::fabs(q.x) + r <= half && std::fabs(q.y) + r <= half);
}

ShadowPass::ShadowPass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    if (const char* cull = std::getenv("AETHER_RENDER_SHADOW_CULL")) cull_receivers_ = std::strcmp(cull, "0") != 0;
    // Probe for depth textures with a tiny one: no point building pipelines the device can't draw into.
    const gfx::rhi::SampledTextureHandle probe = device.CreateDepthTexture(4, 4);
    if (!probe.IsValid()) {
        AETHER_LOG_INFO("PlayerRenderer", "No depth textures on this device; the scene draws without sun shadows");
        return;
    }
    device.DestroyTexture(probe);
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = kCasterShader;
    desc.push_constant_size_bytes = sizeof(ShadowCasterConstants);
    desc.use_vertex_buffer = true;
    desc.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUvTangent;
    desc.depth_target = true;
    desc.cull_back_face = false; // two-sided: open and thin meshes (cloth, leaves, a lantern's frame) still cast
    desc.slope_scaled_depth_bias = 1.5f;
    desc.ps_entry = "";
    opaque_ = device.CreatePipeline(desc, swap_chain);
    desc.ps_entry = "PSMain";
    desc.enable_bindless_textures = true;
    masked_ = device.CreatePipeline(desc, swap_chain);
    if (!Valid()) AETHER_LOG_WARN("PlayerRenderer", "Shadow caster pipelines unavailable; the scene draws without sun shadows");
}

ShadowPass::~ShadowPass() {
    ReleaseAtlas();
    if (masked_.IsValid()) device_.DestroyPipeline(masked_);
    if (opaque_.IsValid()) device_.DestroyPipeline(opaque_);
}

void ShadowPass::Configure(u32 resolution, u32 cascades, f32 distance) {
    resolution_ = std::min(resolution, 8192u);
    cascades_ = std::min(cascades, 4u);
    if (distance > 0.0f && std::isfinite(distance)) distance_ = distance;
    if (resolution_ == 0 || cascades_ == 0) ReleaseAtlas();
}

void ShadowPass::ReleaseAtlas() {
    if (atlas_.IsValid()) device_.DestroyTexture(atlas_);
    atlas_ = {};
    atlas_resolution_ = atlas_cascades_ = 0;
}

ShadowFrameData ShadowPass::Render(gfx::rhi::ICommandList& commands, const View& view, const Vec3& light_direction,
                                   std::span<const ShadowCaster> casters) {
    last_draws_ = 0;
    if (!Valid() || resolution_ < 8 || cascades_ == 0) return {};
    if (!atlas_.IsValid() || atlas_resolution_ != resolution_ || atlas_cascades_ != cascades_) {
        ReleaseAtlas();
        u32 width = 0, height = 0;
        ShadowAtlasSize(resolution_, cascades_, width, height);
        atlas_ = device_.CreateDepthTexture(width, height);
        if (!atlas_.IsValid() || atlas_.index >= gfx::rhi::kMaxBindlessTextures) {
            if (atlas_.IsValid()) device_.DestroyTexture(atlas_);
            atlas_ = {};
            if (!warned_) AETHER_LOG_WARN("PlayerRenderer", "No room for the %ux%u shadow atlas; the scene draws without sun shadows", width, height);
            warned_ = true;
            return {};
        }
        atlas_resolution_ = resolution_;
        atlas_cascades_ = cascades_;
    }
    const std::vector<ShadowCascade> cascades = FitShadowCascades(view, light_direction, resolution_, cascades_, distance_);
    if (cascades.empty() || !commands.BeginDepthPass(atlas_, true)) return {};

    std::vector<CascadeReceivers> receivers;
    if (cull_receivers_) {
        for (usize i = 0; i < cascades.size(); ++i) receivers.push_back(CascadeReceiverRegion(view, cascades, i));
    }
    const f32 size = static_cast<f32>(resolution_);
    for (usize i = 0; i < cascades.size(); ++i) {
        u32 x = 0, y = 0;
        ShadowCascadeOrigin(resolution_, static_cast<u32>(i), x, y);
        commands.SetViewportAndScissor({static_cast<f32>(x), static_cast<f32>(y), size, size, 0.0f, 1.0f},
                                       {static_cast<i32>(x), static_cast<i32>(y), static_cast<i32>(x + resolution_), static_cast<i32>(y + resolution_)});
        // Opaque casters first, then the alpha-tested ones: one pipeline switch per cascade at most.
        for (const bool masked : {false, true}) {
            bool bound = false;
            for (const ShadowCaster& caster : casters) {
                if (caster.alpha_tested != masked || caster.index_count == 0 || !CasterTouchesCascade(caster, cascades[i])) continue;
                if (cull_receivers_ && !CasterReachesReceivers(caster, cascades[i], receivers[i])) continue;
                if (!bound) {
                    commands.BindPipeline(masked ? masked_ : opaque_);
                    if (masked) commands.BindBindlessTextures();
                    bound = true;
                }
                ShadowCasterConstants constants;
                constants.light_mvp = cascades[i].view_projection * caster.model;
                constants.base_color_texture = caster.base_color_texture < gfx::rhi::kMaxBindlessTextures ? caster.base_color_texture : 255u;
                constants.alpha = caster.alpha;
                constants.alpha_cutoff = caster.alpha_cutoff;
                commands.BindVertexBuffer(caster.vertices, 48);
                commands.BindIndexBuffer(caster.indices, gfx::rhi::IndexFormat::UInt32);
                commands.SetPushConstants(&constants, sizeof(constants));
                commands.DrawIndexed(caster.index_count);
                ++last_draws_;
            }
        }
    }
    commands.EndRenderPass();
    return PackShadowFrameData(cascades, resolution_, atlas_.index);
}

} // namespace aether::player
