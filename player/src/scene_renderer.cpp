#include "aether/player/scene_renderer.h"

#include "aether/assets/gltf_loader.h"
#include "aether/assets/image.h"
#include "aether/core/log.h"
#include "aether/scene/gameplay.h"
#include "aether/scene/hierarchy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>

namespace aether::player {
namespace {

using namespace gfx::rhi;
using json = nlohmann::json;

struct SceneVertex {
    f32 position[3];
    f32 uv[2];
};
static_assert(sizeof(SceneVertex) == sizeof(f32) * 5);

struct DrawConstants {
    Mat4 mvp;
    f32 color[4] = {1, 1, 1, 1};
    u32 texture_index = 0;
    u32 use_texture = 0;
    u32 padding[2] = {};
};
static_assert(sizeof(DrawConstants) == 96);

constexpr char kSceneShader[] = R"(
struct PushConstants {
    float4x4 g_Mvp;
    float4 g_Color;
    uint g_TextureIndex;
    uint g_UseTexture;
    uint2 g_Padding;
};
#ifdef __spirv__
[[vk::push_constant]]
#endif
ConstantBuffer<PushConstants> g_PC : register(b0);

struct VSInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
Texture2D g_Textures[32] : register(t0, space0);
SamplerState g_Sampler : register(s0, space1);

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(g_PC.g_Mvp, float4(input.position, 1.0));
    output.uv = input.uv;
    return output;
}
float4 PSMain(PSInput input) : SV_TARGET {
    float4 base = g_PC.g_UseTexture != 0
        ? g_Textures[g_PC.g_TextureIndex].Sample(g_Sampler, input.uv)
        : float4(1, 1, 1, 1);
    return base * g_PC.g_Color;
}
)";

std::string Base64(std::span<const u8> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (usize i = 0; i < bytes.size(); i += 3) {
        const u32 a = bytes[i];
        const u32 b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const u32 c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const u32 value = (a << 16) | (b << 8) | c;
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return out;
}

std::string ResolveContentPath(const std::string& owner_path, const std::string& uri) {
    const std::filesystem::path base = std::filesystem::path(owner_path).parent_path();
    return (base / std::filesystem::path(uri)).lexically_normal().generic_string();
}

Mat4 EntityTransform(const Transform& transform) {
    return Mat4::Translation(transform.position) * transform.rotation.ToMat4();
}

} // namespace

struct SceneRenderer::Impl {
    struct Primitive {
        BufferHandle vertices;
        BufferHandle indices;
        u32 index_count = 0;
        f32 color[4] = {1, 1, 1, 1};
        SampledTextureHandle texture;
        bool has_texture = false;
    };
    struct Model {
        assets::GltfScene scene;
        std::vector<std::vector<Primitive>> meshes;
        bool valid = false;
    };

    IDevice& device;
    ISwapChain& swap_chain;
    const GamePackage& package;
    PipelineHandle pipeline;
    SampledTextureHandle white_texture;
    std::unordered_map<std::string, Model> models;
    std::unordered_map<std::string, SampledTextureHandle> textures;

    Impl(IDevice& d, ISwapChain& s, const GamePackage& p) : device(d), swap_chain(s), package(p) {
        PipelineDesc desc;
        desc.hlsl_source = kSceneShader;
        desc.push_constant_size_bytes = sizeof(DrawConstants);
        desc.use_vertex_buffer = true;
        desc.enable_bindless_textures = true;
        desc.depth_test = true;
        pipeline = device.CreatePipeline(desc, swap_chain);

        constexpr std::array<u8, 4> white{255, 255, 255, 255};
        white_texture = device.CreateTexture(1, 1, white.data());
        if (white_texture.IsValid()) textures.emplace("", white_texture);
    }

    bool LoadTexture(const std::string& path, SampledTextureHandle& out) {
        if (path.empty()) {
            out = white_texture;
            return false;
        }
        if (const auto found = textures.find(path); found != textures.end()) {
            out = found->second;
            return out.IsValid() && out.index != white_texture.index;
        }
        if (textures.size() >= kMaxBindlessTextures) {
            AETHER_LOG_WARN("PlayerRenderer", "Bindless texture limit reached; using the white fallback for '%s'", path.c_str());
            out = white_texture;
            return false;
        }
        std::vector<u8> bytes;
        assets::ImageData image;
        if (!package.ReadContent(path, bytes) || !assets::DecodeImageFromMemory(bytes, image)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't load model texture '%s'; using the white fallback", path.c_str());
            out = white_texture;
            return false;
        }
        out = device.CreateTexture(image.width, image.height, image.pixels.data());
        if (out.IsValid()) textures.emplace(path, out);
        return out.IsValid() && out.index != white_texture.index;
    }

    bool LoadModel(const std::string& path, Model& out) {
        std::vector<u8> source;
        if (!package.ReadContent(path, source)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't read cooked model '%s'", path.c_str());
            return false;
        }
        json document = json::parse(source.begin(), source.end(), nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            AETHER_LOG_WARN("PlayerRenderer", "Cooked model '%s' isn't valid glTF JSON", path.c_str());
            return false;
        }

        // A cooked package is a VFS, not a filesystem. Embed the already
        // cooked buffer bytes into the in-memory glTF document so the shared
        // parser can safely load it without opening arbitrary paths.
        if (document.contains("buffers") && document["buffers"].is_array()) {
            for (json& buffer : document["buffers"]) {
                if (!buffer.contains("uri") || !buffer["uri"].is_string()) continue;
                const std::string uri = buffer["uri"].get<std::string>();
                if (uri.rfind("data:", 0) == 0) continue;
                const std::string buffer_path = ResolveContentPath(path, uri);
                std::vector<u8> bytes;
                if (!package.ReadContent(buffer_path, bytes)) {
                    AETHER_LOG_WARN("PlayerRenderer", "Model '%s' references missing buffer '%s'", path.c_str(), buffer_path.c_str());
                    return false;
                }
                buffer["uri"] = "data:application/octet-stream;base64," + Base64(bytes);
            }
        }
        const std::string cooked_json = document.dump();
        if (!assets::LoadGltfFromMemory(std::span<const u8>(reinterpret_cast<const u8*>(cooked_json.data()), cooked_json.size()),
                                        out.scene)) {
            AETHER_LOG_WARN("PlayerRenderer", "Couldn't parse cooked model '%s'", path.c_str());
            return false;
        }

        std::vector<SampledTextureHandle> material_textures;
        std::vector<bool> material_has_textures;
        if (document.contains("materials") && document["materials"].is_array()) {
            material_textures.reserve(document["materials"].size());
            for (const json& material : document["materials"]) {
                SampledTextureHandle texture = white_texture;
                bool has_texture = false;
                const json* pbr = material.contains("pbrMetallicRoughness") ? &material["pbrMetallicRoughness"] : nullptr;
                if (pbr && pbr->contains("baseColorTexture")) {
                    const usize index = (*pbr)["baseColorTexture"].value("index", usize(-1));
                    if (index < document.value("textures", json::array()).size()) {
                        const json& texture_info = document["textures"][index];
                        const usize image_index = texture_info.value("source", usize(-1));
                        if (image_index < document.value("images", json::array()).size()) {
                            const json& image = document["images"][image_index];
                            if (image.contains("uri") && image["uri"].is_string()) {
                                const std::string uri = image["uri"].get<std::string>();
                                if (uri.rfind("data:", 0) != 0) {
                                    has_texture = LoadTexture(ResolveContentPath(path, uri), texture);
                                }
                            }
                        }
                    }
                }
                material_textures.push_back(texture);
                material_has_textures.push_back(has_texture);
            }
        }

        out.meshes.resize(out.scene.meshes.size());
        for (usize mesh_index = 0; mesh_index < out.scene.meshes.size(); ++mesh_index) {
            const auto& source_mesh = out.scene.meshes[mesh_index];
            auto& destination_mesh = out.meshes[mesh_index];
            destination_mesh.reserve(source_mesh.primitives.size());
            for (const assets::GltfPrimitive& primitive : source_mesh.primitives) {
                if (primitive.vertices.empty() || primitive.indices.empty()) continue;
                std::vector<SceneVertex> vertices;
                vertices.reserve(primitive.vertices.size());
                for (const assets::GltfVertex& vertex : primitive.vertices) {
                    vertices.push_back({{vertex.position[0], vertex.position[1], vertex.position[2]},
                                        {vertex.uv[0], vertex.uv[1]}});
                }
                Primitive draw;
                draw.vertices = device.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(SceneVertex));
                draw.indices = device.CreateIndexBuffer(primitive.indices.data(), primitive.indices.size() * sizeof(u32),
                                                        IndexFormat::UInt32);
                draw.index_count = static_cast<u32>(primitive.indices.size());
                if (primitive.material_index >= 0 && static_cast<usize>(primitive.material_index) < out.scene.materials.size()) {
                    const auto& material = out.scene.materials[static_cast<usize>(primitive.material_index)];
                    std::copy(std::begin(material.base_color), std::end(material.base_color), draw.color);
                    const usize material_index = static_cast<usize>(primitive.material_index);
                    if (material_index < material_textures.size()) {
                        draw.texture = material_textures[material_index];
                        draw.has_texture = material_has_textures[material_index];
                    }
                } else {
                    draw.texture = white_texture;
                }
                destination_mesh.push_back(draw);
            }
        }
        out.valid = true;
        AETHER_LOG_INFO("PlayerRenderer", "Loaded '%s' (%zu mesh(es), %zu node instance(s))", path.c_str(),
                        out.scene.meshes.size(), out.scene.node_instances.size());
        return true;
    }

    Model* FindModel(const std::string& path) {
        auto [it, inserted] = models.try_emplace(path);
        if (inserted) LoadModel(path, it->second);
        return it->second.valid ? &it->second : nullptr;
    }
};

SceneRenderer::SceneRenderer(IDevice& device, ISwapChain& swap_chain, const GamePackage& package)
    : impl_(std::make_unique<Impl>(device, swap_chain, package)) {}
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::Draw(Game& game, ICommandList& commands) {
    Impl& renderer = *impl_;
    World& world = game.GetWorld();
    GuidIndex& guids = game.Guids();

    // Resolve model assets before the render pass: texture uploads submit
    // their own short GPU command list and must not interrupt a frame pass.
    world.ForEach<ModelRenderer>([&](const ModelRenderer& model) {
        if (model.asset_path[0] != '\0') renderer.FindModel(model.asset_path);
    });

    commands.BeginRenderPass(renderer.swap_chain, {0.015f, 0.025f, 0.045f, 1.0f});
    const Entity camera_entity = FindActiveCamera(world, guids);
    const Camera* camera = world.IsAlive(camera_entity) ? world.GetComponent<Camera>(camera_entity) : nullptr;
    if (camera) {
        const f32 aspect = static_cast<f32>(renderer.swap_chain.Width()) /
                           static_cast<f32>(std::max(renderer.swap_chain.Height(), 1u));
        const Mat4 view_projection = CameraProjection(*camera, aspect) * CameraView(world, guids, camera_entity);
        commands.BindPipeline(renderer.pipeline);
        commands.BindBindlessTextures();
        world.ForEachArchetype([&](const Archetype& archetype) {
            if (!archetype.Mask().test(GetComponentId<Transform>()) || !archetype.Mask().test(GetComponentId<ModelRenderer>())) return;
            for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
                const Entity* entities = archetype.EntityArray(chunk);
                const auto* transforms = static_cast<const Transform*>(archetype.ComponentArray(chunk, GetComponentId<Transform>()));
                const auto* models = static_cast<const ModelRenderer*>(archetype.ComponentArray(chunk, GetComponentId<ModelRenderer>()));
                const usize count = archetype.ChunkEntityCount(chunk);
                for (usize row = 0; row < count; ++row) {
                    const Impl::Model* model = renderer.FindModel(models[row].asset_path);
                    if (!model) continue;
                    const Mat4 entity_transform = ComputeWorldTransform(world, guids, entities[row]);
                    (void)transforms; // ComputeWorldTransform also includes any parent chain.
                    for (const assets::GltfNodeInstance& node : model->scene.node_instances) {
                        if (node.mesh_index >= model->meshes.size()) continue;
                        const Mat4 mvp = view_projection * entity_transform * node.world_transform;
                        for (const Impl::Primitive& primitive : model->meshes[node.mesh_index]) {
                            DrawConstants constants;
                            constants.mvp = mvp;
                            std::copy(std::begin(primitive.color), std::end(primitive.color), constants.color);
                            constants.texture_index = primitive.texture.IsValid() ? primitive.texture.index : renderer.white_texture.index;
                            constants.use_texture = primitive.has_texture ? 1u : 0u;
                            commands.BindVertexBuffer(primitive.vertices, sizeof(SceneVertex));
                            commands.BindIndexBuffer(primitive.indices, IndexFormat::UInt32);
                            commands.SetPushConstants(&constants, sizeof(constants));
                            commands.DrawIndexed(primitive.index_count);
                        }
                    }
                }
            }
        });
    }
    commands.EndRenderPass();
}

} // namespace aether::player
