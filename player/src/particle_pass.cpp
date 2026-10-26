#include "particle_pass.h"

#include "aether/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::player {
namespace {

// The particle pass (M8.7): camera-facing sprites and ribbons already in world space (particle_pass.h), projected here and drawn after the
// scene's surfaces, depth-tested without writing depth, through an alpha-blended and an additive pipeline. A sprite (params.x = 1) fades to
// a soft round edge; its colour is linear and encoded like the scene's.
constexpr char kParticleShader[] = R"(
struct PushConstants {
    float4x4 g_ViewProjection;
    uint4 g_Texture;      // x: this draw's bindless texture index (0: untextured); y: the scene depth's (0: none); z: asuint(soft fade distance)
    float4 g_DepthToView; // ParticleDepthToView: clip depth back to view z
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

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

float ViewDistance(float depth) {
    float4 k = g_PC.g_DepthToView;
    return abs((k.y - depth * k.w) / (depth * k.z - k.x));
}

float4 PSMain(PSInput input) : SV_TARGET {
    float4 color = input.color;
    color.a = saturate(color.a);
    // The texture comes from the push constants, so it is uniform across the draw (the pass draws each run of one texture separately):
    // indexing the bindless table with a per-vertex value would need NonUniformResourceIndex/NonUniformEXT, which glslang's HLSL front
    // end doesn't emit. The vertex still carries its index (4 * index + kind) for the kind.
    uint texture_index = g_PC.g_Texture.x;
    float kind = input.params.x - 4.0 * floor(input.params.x * 0.25);
    bool sprite = kind > 0.5;
    if (texture_index != 0 && texture_index < 32) {
        float4 texel = g_Textures[texture_index].Sample(g_Sampler, input.uv);
        float frame_blend = sprite ? saturate(kind - 1.0) : 0.0;
        if (frame_blend > 0.0) texel = lerp(texel, g_Textures[texture_index].Sample(g_Sampler, input.uv + input.params.yz), frame_blend);
        color *= texel;
    } else if (sprite) {
        float2 cell = frac(input.uv) * 2.0 - 1.0;
        float falloff = saturate(1.0 - dot(cell, cell));
        color.a *= falloff * falloff;
    }
    // Soft particles: fade out within the soft distance (view units) of the opaque scene behind, read with a texel load (no filtering of
    // depth) from the scene depth copy at this pixel.
    float soft = asfloat(g_PC.g_Texture.z);
    uint depth_index = g_PC.g_Texture.y;
    if (soft > 0.0 && depth_index != 0 && depth_index < 32) {
        float scene = g_Textures[depth_index].Load(int3(int2(input.position.xy), 0)).r;
        color.a *= saturate((ViewDistance(scene) - ViewDistance(input.position.z)) / soft);
    }
    float3 encoded = saturate(color.rgb);
    encoded = lerp(encoded * 12.92, 1.055 * pow(encoded, 1.0 / 2.4) - 0.055, step(0.0031308, encoded));
    return float4(encoded, saturate(color.a));
}
)";

struct ParticleConstants {
    Mat4 view_projection;
    u32 texture[4]; // texture, scene depth, soft fade (bits), 0
    f32 depth_to_view[4];
};
static_assert(sizeof(ParticleConstants) == 96);

f32 SanitizedSoft(f32 soft) { return std::isfinite(soft) && soft > 0.0f ? soft : 0.0f; }

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

void AppendSprite(const vfx::SpriteInstance& s, vfx::BlendMode blend, bool blend_frames, u32 texture, f32 soft, std::vector<ParticleQuadVertex>& out,
                  std::vector<f32>& out_soft) {
    out_soft.insert(out_soft.end(), 6, soft);
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

void ParticleTextureRuns(const std::vector<ParticleQuadVertex>& vertices, std::vector<ParticleDrawRun>& out, const std::vector<f32>* soft) {
    out.clear();
    const bool has_soft = soft != nullptr && soft->size() == vertices.size();
    const auto soft_at = [&](usize i) { return has_soft ? (*soft)[i] : 0.0f; };
    for (usize i = 0; i < vertices.size();) {
        const u32 texture = ParticleVertexTexture(vertices[i]);
        const f32 fade = soft_at(i);
        usize end = i + 1;
        while (end < vertices.size() && ParticleVertexTexture(vertices[end]) == texture && soft_at(end) == fade) ++end;
        out.push_back({static_cast<u32>(i), static_cast<u32>(end - i), texture, fade});
        i = end;
    }
}

bool ParticleVertexStreams::AnySoft() const {
    const auto any = [](const std::vector<f32>& v) { return std::any_of(v.begin(), v.end(), [](f32 f) { return f > 0.0f; }); };
    return any(alpha_soft) || any(additive_soft);
}

std::array<f32, 4> ParticleDepthToView(const Mat4& projection) {
    return {projection.cols[2].z, projection.cols[3].z, projection.cols[2].w, projection.cols[3].w};
}

f32 ParticleViewDistance(const std::array<f32, 4>& k, f32 depth) { return std::fabs((k[1] - depth * k[3]) / (depth * k[2] - k[0])); }

f32 ParticleSoftFade(const std::array<f32, 4>& depth_to_view, f32 scene_depth, f32 fragment_depth, f32 soft_fade) {
    if (!(soft_fade > 0.0f)) return 1.0f;
    const f32 gap = ParticleViewDistance(depth_to_view, scene_depth) - ParticleViewDistance(depth_to_view, fragment_depth);
    return std::clamp(gap / soft_fade, 0.0f, 1.0f);
}

void PackParticleVertices(const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera, ParticleVertexStreams& out,
                          const ParticleTextureResolver& textures, const ParticleMeshResolver& meshes) {
    out.Clear();
    const auto texture_of = [&](const std::string& material) -> u32 { return material.empty() || !textures ? 0u : textures(material); };
    struct Sorted {
        const vfx::SpriteInstance* sprite;
        vfx::BlendMode blend;
        bool blend_frames;
        u32 texture;
        f32 soft;
        f32 depth;
    };
    thread_local std::vector<Sorted> alpha_sprites;
    alpha_sprites.clear();
    for (const vfx::SpriteBatch& batch : data.sprites) {
        const u32 texture = texture_of(batch.material);
        const f32 soft = SanitizedSoft(batch.soft_fade);
        if (batch.blend == vfx::BlendMode::Additive) {
            for (const vfx::SpriteInstance& s : batch.instances) AppendSprite(s, batch.blend, batch.blend_frames, texture, soft, out.additive, out.additive_soft);
        } else {
            for (const vfx::SpriteInstance& s : batch.instances) {
                alpha_sprites.push_back({&s, batch.blend, batch.blend_frames, texture, soft, (s.center - camera.position).Dot(camera.forward)});
            }
        }
    }
    // Back to front across batches (stable: ties keep the batches' own order).
    std::stable_sort(alpha_sprites.begin(), alpha_sprites.end(), [](const Sorted& l, const Sorted& r) { return l.depth > r.depth; });
    for (const Sorted& s : alpha_sprites) AppendSprite(*s.sprite, s.blend, s.blend_frames, s.texture, s.soft, out.alpha, out.alpha_soft);

    for (const vfx::RibbonStrip& ribbon : data.ribbons) {
        const bool additive = ribbon.blend == vfx::BlendMode::Additive;
        std::vector<ParticleQuadVertex>& stream = additive ? out.additive : out.alpha;
        std::vector<f32>& stream_soft = additive ? out.additive_soft : out.alpha_soft;
        const Vec3 params(TextureBase(texture_of(ribbon.material)) + 0.25f, 0.0f, 0.0f);
        for (usize i = 0; i + 3 <= ribbon.indices.size(); i += 3) {
            bool valid = true;
            for (usize k = 0; k < 3; ++k) valid = valid && ribbon.indices[i + k] < ribbon.vertices.size();
            if (!valid) continue;
            for (usize k = 0; k < 3; ++k) {
                const vfx::ParticleVertex& v = ribbon.vertices[ribbon.indices[i + k]];
                stream.push_back(Vertex(v.position, params, v.u, v.v, StraightColor(v.color, ribbon.blend)));
                stream_soft.push_back(0.0f); // ribbons have no soft distance
            }
        }
    }

    // Mesh particles: each instance's transform applied to the mesh's triangles, opaque (alpha 1), shaded like a ribbon (colour times the
    // texture, no falloff).
    if (!meshes) return;
    for (const vfx::MeshBatch& batch : data.meshes) {
        const ParticleMesh* mesh = batch.mesh.empty() ? nullptr : meshes(batch.mesh);
        if (mesh == nullptr || mesh->positions.size() < 3) continue;
        const usize triangles = mesh->positions.size() / 3 * 3;
        const bool has_uv = mesh->uvs.size() >= mesh->positions.size() * 2;
        const Vec3 params(TextureBase(texture_of(batch.material)) + 0.25f, 0.0f, 0.0f);
        out.opaque.reserve(out.opaque.size() + triangles * batch.transforms.size());
        for (usize i = 0; i < batch.transforms.size(); ++i) {
            const Mat4& m = batch.transforms[i];
            const vfx::LinearColor c = i < batch.colors.size() ? batch.colors[i] : vfx::LinearColor{};
            const ColorOut color = StraightColor(c, vfx::BlendMode::Opaque);
            for (usize k = 0; k < triangles; ++k) {
                const Vec3& p = mesh->positions[k];
                const Vec4 w = m * Vec4(p.x, p.y, p.z, 1.0f);
                out.opaque.push_back(Vertex(Vec3(w.x, w.y, w.z), params, has_uv ? mesh->uvs[k * 2] : 0.0f, has_uv ? mesh->uvs[k * 2 + 1] : 0.0f, color));
            }
        }
    }
}

ParticlePass::ParticlePass(gfx::rhi::IDevice& device, gfx::rhi::ISwapChain& swap_chain) : device_(device) {
    // Soft particles read the scene depth copy; without one (backend, depth format or a full table) they draw hard-edged.
    const gfx::rhi::SampledTextureHandle depth = swap_chain.SceneDepthTexture();
    scene_depth_ = depth.IsValid() && depth.index < gfx::rhi::kMaxBindlessTextures ? depth.index : 0u;
    if (scene_depth_ == 0) AETHER_LOG_INFO("PlayerRenderer", "No scene depth texture on this device; soft particles draw hard-edged");
    gfx::rhi::PipelineDesc desc;
    desc.hlsl_source = kParticleShader;
    desc.push_constant_size_bytes = sizeof(ParticleConstants);
    desc.use_vertex_buffer = true;
    desc.vertex_layout = gfx::rhi::VertexLayout::PositionNormalUvTangent;
    desc.depth_test = true;
    desc.depth_write = false;
    desc.enable_blending = true;
    desc.enable_bindless_textures = true;
    // Mesh particles first: opaque, writing depth.
    desc.depth_write = true;
    desc.enable_blending = false;
    opaque_pipeline_ = device.CreatePipeline(desc, swap_chain);
    desc.depth_write = false;
    desc.enable_blending = true;
    alpha_pipeline_ = device.CreatePipeline(desc, swap_chain);
    desc.additive_blending = true;
    additive_pipeline_ = device.CreatePipeline(desc, swap_chain);
    if (!Valid()) AETHER_LOG_WARN("PlayerRenderer", "Particle pipelines unavailable; particle effects are not drawn");
}

void ParticlePass::Draw(gfx::rhi::ICommandList& commands, const vfx::ParticleRenderData& data, const vfx::ParticleCamera& camera,
                        const Mat4& view_projection, const Mat4& projection, u32 frame_index) {
    PackParticleVertices(data, camera, vertices_, textures_, meshes_);
    depth_to_view_ = ParticleDepthToView(projection);
    std::array<Buffer, 3>& slot = buffers_[frame_index % kFrameSlots];
    // Opaque mesh particles first (writing depth), alpha-blended back to front next, then additive light on top.
    DrawStream(commands, opaque_pipeline_, vertices_.opaque, nullptr, slot[2], view_projection, 0);
    // The scene's depth so far (surfaces and mesh particles) for the soft sprites, only on frames that have some; the blended draws after
    // the copy don't write depth, so the copy stays what they test against.
    u32 scene_depth = 0;
    if (scene_depth_ != 0 && vertices_.AnySoft() && commands.CopySceneDepth()) scene_depth = scene_depth_;
    DrawStream(commands, alpha_pipeline_, vertices_.alpha, &vertices_.alpha_soft, slot[0], view_projection, scene_depth);
    DrawStream(commands, additive_pipeline_, vertices_.additive, &vertices_.additive_soft, slot[1], view_projection, scene_depth);
}

void ParticlePass::DrawStream(gfx::rhi::ICommandList& commands, gfx::rhi::PipelineHandle pipeline, const std::vector<ParticleQuadVertex>& vertices,
                              const std::vector<f32>* soft, Buffer& buffer, const Mat4& view_projection, u32 scene_depth) {
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
    // One draw per run of a texture (whole triangles: a sprite's or ribbon's vertices share one), in the stream's order, so alpha sprites
    // stay sorted back to front and the shader's texture index is uniform across each draw.
    ParticleTextureRuns(vertices, runs_, scene_depth != 0 ? soft : nullptr);
    for (const ParticleDrawRun& run : runs_) {
        u32 soft_bits = 0;
        std::memcpy(&soft_bits, &run.soft_fade, sizeof(soft_bits));
        const ParticleConstants constants{view_projection, {run.texture, scene_depth, soft_bits, 0u},
                                          {depth_to_view_[0], depth_to_view_[1], depth_to_view_[2], depth_to_view_[3]}};
        commands.SetPushConstants(&constants, sizeof(constants));
        commands.Draw(run.vertex_count, run.first_vertex);
    }
}

} // namespace aether::player
