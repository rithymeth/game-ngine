#include "particle_pass.h"

#include "aether/core/log.h"

#include <algorithm>

namespace aether::player {
namespace {

// The particle pass (M8.7): camera-facing sprites and ribbons already in world space (particle_pass.h), projected here and drawn after the
// scene's surfaces, depth-tested without writing depth, through an alpha-blended and an additive pipeline. A sprite (params.x = 1) fades to
// a soft round edge; its colour is linear and encoded like the scene's.
constexpr char kParticleShader[] = R"(
struct PushConstants {
    float4x4 g_ViewProjection;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);
// The texture index varies per sprite within a draw. D3D12 (FXC 5.1) is told so; glslang's HLSL front end has no NonUniformResourceIndex.
#ifdef __spirv__
#define AETHER_NON_UNIFORM(i) (i)
#else
#define AETHER_NON_UNIFORM(i) NonUniformResourceIndex(i)
#endif

struct VSInput {
    float3 position : POSITION;
    float3 params : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : TANGENT;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation float3 params : TEXCOORD2;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(g_PC.g_ViewProjection, float4(input.position, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    output.params = input.params;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float4 color = input.color;
    color.a = saturate(color.a);
    uint texture_index = (uint)floor(input.params.x * 0.25);
    float kind = input.params.x - 4.0 * (float)texture_index;
    bool sprite = kind > 0.5;
    if (texture_index != 0 && texture_index < 32) {
        float4 texel = g_Textures[AETHER_NON_UNIFORM(texture_index)].Sample(g_Sampler, input.uv);
        float frame_blend = sprite ? saturate(kind - 1.0) : 0.0;
        if (frame_blend > 0.0) texel = lerp(texel, g_Textures[AETHER_NON_UNIFORM(texture_index)].Sample(g_Sampler, input.uv + input.params.yz), frame_blend);
        color *= texel;
    } else if (sprite) {
        float2 cell = frac(input.uv) * 2.0 - 1.0;
        float falloff = saturate(1.0 - dot(cell, cell));
        color.a *= falloff * falloff;
    }
    float3 encoded = saturate(color.rgb);
    encoded = lerp(encoded * 12.92, 1.055 * pow(encoded, 1.0 / 2.4) - 0.055, step(0.0031308, encoded));
    return float4(encoded, saturate(color.a));
}
)";

struct ParticleConstants {
    Mat4 view_projection;
};
static_assert(sizeof(ParticleConstants) == 64);

struct ColorOut {
    f32 rgba[4];
};

ColorOut StraightColor(const vfx::LinearColor& c, vfx::BlendMode blend) {
    ColorOut out{{c.r, c.g, c.b, c.a}};
    if (blend == vfx::BlendMode::Opaque) out.rgba[3] = 1.0f;
    if (blend == vfx::BlendMode::Premultiplied) {
        // The alpha pipeline multiplies by alpha; undo the premultiply (a premultiplied colour with no alpha adds nothing here).
        if (c.a > 1e-5f) {
            out.rgba[0] /= c.a;
            out.rgba[1] /= c.a;
            out.rgba[2] /= c.a;
        } else {
            out.rgba[0] = out.rgba[1] = out.rgba[2] = 0.0f;
        }
    }
    return out;
}

ParticleQuadVertex Vertex(const Vec3& p, const Vec3& params, f32 u, f32 v, const ColorOut& color) {
    return {{p.x, p.y, p.z}, {params.x, params.y, params.z}, {u, v}, {color.rgba[0], color.rgba[1], color.rgba[2], color.rgba[3]}};
}

f32 TextureBase(u32 texture) { return 4.0f * static_cast<f32>(texture < gfx::rhi::kMaxBindlessTextures ? texture : 0u); }

void AppendSprite(const vfx::SpriteInstance& s, vfx::BlendMode blend, bool blend_frames, u32 texture, std::vector<ParticleQuadVertex>& out) {
    const ColorOut color = StraightColor(s.color, blend);
    // params: the texture and 1 + the frame blend, then the next cell's offset from this one.
    const f32 frame_blend = blend_frames ? std::clamp(s.frame_blend, 0.0f, 0.999f) : 0.0f;
    const Vec3 params(TextureBase(texture) + 1.0f + frame_blend, frame_blend > 0.0f ? s.uv_next.x - s.uv.x : 0.0f, frame_blend > 0.0f ? s.uv_next.y - s.uv.y : 0.0f);
    // Corners: bottom-left, bottom-right, top-right, top-left; uv from the flipbook cell (v down).
    const Vec3 bl = s.center - s.right - s.up, br = s.center + s.right - s.up;
    const Vec3 tr = s.center + s.right + s.up, tl = s.center - s.right + s.up;
    const f32 u0 = s.uv.x, u1 = s.uv.x + s.uv.w, v0 = s.uv.y, v1 = s.uv.y + s.uv.h;
    const ParticleQuadVertex a = Vertex(bl, params, u0, v1, color), b = Vertex(br, params, u1, v1, color);
    const ParticleQuadVertex c = Vertex(tr, params, u1, v0, color), d = Vertex(tl, params, u0, v0, color);
    out.insert(out.end(), {a, b, c, a, c, d});
}

} // namespace

void PackParticleVertices(const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera, ParticleVertexStreams& out,
                          const ParticleTextureResolver& textures) {
    out.Clear();
    const auto texture_of = [&](const std::string& material) -> u32 { return material.empty() || !textures ? 0u : textures(material); };
    struct Sorted {
        const vfx::SpriteInstance* sprite;
        vfx::BlendMode blend;
        bool blend_frames;
        u32 texture;
        f32 depth;
    };
    thread_local std::vector<Sorted> alpha_sprites;
    alpha_sprites.clear();
    for (const vfx::SpriteBatch& batch : data.sprites) {
        const u32 texture = texture_of(batch.material);
        if (batch.blend == vfx::BlendMode::Additive) {
            for (const vfx::SpriteInstance& s : batch.instances) AppendSprite(s, batch.blend, batch.blend_frames, texture, out.additive);
        } else {
            for (const vfx::SpriteInstance& s : batch.instances) {
                alpha_sprites.push_back({&s, batch.blend, batch.blend_frames, texture, (s.center - camera.position).Dot(camera.forward)});
            }
        }
    }
    // Back to front across batches (stable: ties keep the batches' own order).
    std::stable_sort(alpha_sprites.begin(), alpha_sprites.end(), [](const Sorted& l, const Sorted& r) { return l.depth > r.depth; });
    for (const Sorted& s : alpha_sprites) AppendSprite(*s.sprite, s.blend, s.blend_frames, s.texture, out.alpha);

    for (const vfx::RibbonStrip& ribbon : data.ribbons) {
        std::vector<ParticleQuadVertex>& stream = ribbon.blend == vfx::BlendMode::Additive ? out.additive : out.alpha;
        const Vec3 params(TextureBase(texture_of(ribbon.material)) + 0.25f, 0.0f, 0.0f);
        for (usize i = 0; i + 3 <= ribbon.indices.size(); i += 3) {
            bool valid = true;
            for (usize k = 0; k < 3; ++k) valid = valid && ribbon.indices[i + k] < ribbon.vertices.size();
            if (!valid) continue;
            for (usize k = 0; k < 3; ++k) {
                const vfx::ParticleVertex& v = ribbon.vertices[ribbon.indices[i + k]];
                stream.push_back(Vertex(v.position, params, v.u, v.v, StraightColor(v.color, ribbon.blend)));
            }
        }
    }
}

ParticlePass::ParticlePass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = kParticleShader;
    desc.push_constant_size_bytes = sizeof(ParticleConstants);
    desc.use_vertex_buffer = true;
    desc.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUvTangent;
    desc.depth_test = true;
    desc.depth_write = false;
    desc.enable_blending = true;
    desc.enable_bindless_textures = true;
    alpha_pipeline_ = device.CreatePipeline(desc, swap_chain);
    desc.additive_blending = true;
    additive_pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!Valid()) AETHER_LOG_WARN("PlayerRenderer", "Particle pipelines unavailable; particle effects are not drawn");
}

void ParticlePass::Draw(gfx::rhi::ICommandList& commands, const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera,
                        const Mat4& view_projection, u32 frame_index) {
    PackParticleVertices(data, camera, vertices_, textures_);
    std::array<Buffer, 2>& slot = buffers_[frame_index % kFrameSlots];
    // Alpha-blended back to front first, then additive light on top.
    DrawStream(commands, alpha_pipeline_, vertices_.alpha, slot[0], view_projection);
    DrawStream(commands, additive_pipeline_, vertices_.additive, slot[1], view_projection);
}

void ParticlePass::DrawStream(gfx::rhi::ICommandList& commands, gfx::rhi::PipelineHandle pipeline, const std::vector<ParticleQuadVertex>& vertices,
                              Buffer& buffer, const Mat4& view_projection) {
    if (vertices.empty() || !pipeline.IsValid()) return;
    if (!buffer.handle.IsValid() || buffer.capacity < vertices.size()) {
        usize capacity = std::max<usize>(buffer.capacity, 6 * 1024);
        while (capacity < vertices.size()) capacity *= 2;
        const std::vector<ParticleQuadVertex> zeros(capacity, ParticleQuadVertex{});
        buffer.handle = device_.CreateVertexBuffer(zeros.data(), capacity * sizeof(ParticleQuadVertex));
        buffer.capacity = buffer.handle.IsValid() ? capacity : 0;
        if (!buffer.handle.IsValid()) return;
    }
    device_.UpdateVertexBuffer(buffer.handle, vertices.data(), vertices.size() * sizeof(ParticleQuadVertex));
    commands.BindPipeline(pipeline);
    commands.BindBindlessTextures();
    commands.BindVertexBuffer(buffer.handle, sizeof(ParticleQuadVertex));
    const ParticleConstants constants{view_projection};
    commands.SetPushConstants(&constants, sizeof(constants));
    commands.Draw(static_cast<u32>(vertices.size()));
}

} // namespace aether::player
