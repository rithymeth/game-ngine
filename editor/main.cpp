// Phase 5 editor: a Dear ImGui overlay on top of the Phase 3/4 D3D12
// platform layer, driving a live ECS + Jolt physics simulation. Every body
// is an entity with Transform + RigidBody; the render loop reads Transform
// each frame to place an instanced colored quad — no bindless textures or
// GPU-driven culling here (Phase 4 already proved those out), just plain
// DrawInstanced, so the new surface area this phase adds (ImGui, physics,
// serialization) stays easy to follow.
//
// Panels: live entity list, spawn/reset controls, play/pause, and
// Save/Load Scene buttons exercising the Phase 5 serializer. Loading
// reconnects each RigidBody entity to a freshly-created Jolt body (the
// serializer deliberately does not — and cannot — persist a live body
// handle; see aether::RegisterPhysicsComponentSerializers).
//
// Editor-workflow follow-up: a loaded glTF model is now a normal ECS
// entity (Transform + ModelRenderer, see below), not a hardcoded, always-
// present render path bolted onto the side — it shows up in the same
// entity list the physics spheres do, spawns via an "Add Model" panel that
// lists whatever .gltf files sit under assets/models/ instead of a path
// baked into source, round-trips through Save/Load Scene exactly like a
// RigidBody entity does (ModelRenderer's asset_path is plain fixed-size
// data, so it needs no custom serializer — see GltfCache's comment), and
// can be selected (a "Select" button per entity row, not yet a true
// click-in-viewport pick — see the Inspector panel's comment) for a
// dedicated Inspector view and an on-screen highlight tint.
//
// Set AETHER_EDITOR_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.
// Set AETHER_EDITOR_SCREENSHOT=<path> to dump the final frame to a PNG on
// exit, for visual verification without a human watching the window live.

#include "aether/assets/asset_manager.h"
#include "aether/assets/gltf_loader.h"
#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/material.h"
#include "aether/gfx/render_graph.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/job/job_system.h"
#include "aether/math/math.h"
#include "aether/physics/physics_world.h"
#include "aether/platform/window.h"
#include "aether/scene/serialization.h"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace aether;
using namespace aether::gfx;

// imgui_impl_win32.h intentionally comments this declaration out (to avoid
// forcing <windows.h> on everyone who includes it) and asks callers to copy
// it in verbatim.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr u32 kMaxInstances = 256;
constexpr const char* kScenePath = "editor_scene.aesc";

// A model-carrying entity's only component beyond Transform — a reference to
// a glTF asset (relative to AETHER_ASSET_DIR, e.g. "models/foo.gltf"), not
// the loaded GPU/CPU data itself (that lives in GltfCache below, keyed by
// this same path, shared across every entity that references it). Plain
// fixed-size data (not std::string) specifically so it needs no custom
// (de)serializer to round-trip through SaveScene/LoadScene — the default
// raw-byte component serializer every ComponentInfo gets is already correct
// for it, the same way it already is for Transform.
struct ModelRenderer {
    char asset_path[128] = {};
};

void SetModelPath(ModelRenderer& renderer, const std::string& path) {
    std::memset(renderer.asset_path, 0, sizeof(renderer.asset_path));
    std::strncpy(renderer.asset_path, path.c_str(), sizeof(renderer.asset_path) - 1);
}

// World::ForEach/ForEachChunk give component references but not the Entity
// each one belongs to — fine for the physics sync system, not enough for an
// editor that needs to select/delete a specific entity by identity. Built on
// the same type-erased Archetype access the scene serializer uses (see
// World::ForEachArchetype's comment), just paired with EntityArray().
template <typename... Components, typename Func>
void ForEachWithEntity(World& world, Func&& func) {
    ComponentMask query_mask = ComponentMaskOf<Components...>();
    world.ForEachArchetype([&](Archetype& archetype) {
        if ((archetype.Mask() & query_mask) != query_mask) {
            return;
        }
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            u32 count = archetype.ChunkEntityCount(c);
            if (count == 0) {
                continue;
            }
            Entity* entities = archetype.EntityArray(c);
            std::tuple<Components*...> arrays{
                static_cast<Components*>(archetype.ComponentArray(c, GetComponentId<Components>()))...};
            for (u32 i = 0; i < count; ++i) {
                std::apply([&](Components*... arr) { func(entities[i], arr[i]...); }, arrays);
            }
        }
    });
}

struct EditorInstance {
    f32 center[3];
    f32 radius;
    f32 color[3];
    f32 pad;
};

constexpr const char* kShaderSource = R"(
struct InstanceData {
    float3 center;
    float radius;
    float3 color;
    float _pad;
};
StructuredBuffer<InstanceData> g_Instances : register(t0);
cbuffer ViewProj : register(b0) { float4x4 g_ViewProj; };

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

static const float2 kQuadPositions[6] = {
    float2(-0.5, -0.5), float2(-0.5, 0.5), float2(0.5, -0.5),
    float2(0.5, -0.5), float2(-0.5, 0.5), float2(0.5, 0.5),
};

PSInput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    InstanceData inst = g_Instances[instanceID];
    float2 localPos = kQuadPositions[vertexID] * inst.radius;
    float3 worldPos = inst.center + float3(localPos, 0.0);

    PSInput result;
    result.position = mul(g_ViewProj, float4(worldPos, 1.0));
    result.color = inst.color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

ComPtr<ID3D12RootSignature> CreateRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {0, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("D3D12", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreatePSO(Device& device, ID3D12RootSignature* root_signature, DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kShaderSource, "VSMain", "vs_5_0", "editor_vs");
    ShaderBytecode ps = CompileHLSL(kShaderSource, "PSMain", "ps_5_0", "editor_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    // Depth-tested (was DepthEnable=FALSE before the glTF integration
    // follow-up added a real 3D mesh into the same scene): the billboard
    // spheres and the glTF model now need to occlude each other correctly
    // by actual depth, not just by draw order.
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

// --------------------------------------------------------------------------
// glTF integration: loads real glTF assets (material + animation) into the
// same scene the ECS/physics entities above live in — connecting the asset
// pipeline gltf_demo already proved out to the actual gameplay editor, not
// just another standalone demo. Shading/root-signature/vertex-layout is the
// same shape gltf_demo uses (Lambertian + Blinn-Phong, bindless-texture
// material system via AssetManager/LoadMaterial), duplicated here rather
// than shared through a header: both are still demo-scale single-file
// programs, and factoring a shared renderer out is a larger refactor than
// this follow-up's scope.
//
// Editor-workflow follow-up: a model is now a ModelRenderer component on a
// normal entity (see its own comment above) rather than one hardcoded,
// always-present model — GltfRenderData/GltfCache below hold the actual
// GPU/CPU resources, keyed by asset path and shared across every entity
// referencing the same one, since a component can't itself own GPU buffers
// or a std::string account for its unpredictable size in the ECS's fixed-
// slot chunked storage.
// --------------------------------------------------------------------------

constexpr u32 kGltfBindlessCapacity = 16;

constexpr const char* kGltfShaderSource = R"(
cbuffer FrameConstants : register(b0) {
    float4x4 g_MVP;
    float4x4 g_Model;
    float3 g_CameraPos;
    float _pad0;
    float3 g_LightDir;
    float _pad1;
    // Selection follow-up: >0.5 when this draw's entity is the editor's
    // selected_entity, tinting toward gold in PSMain — the "click-to-select"
    // workflow's visual feedback (selection itself is a "Select" button per
    // entity-list row, not yet a true click-in-viewport pick).
    float g_Highlight;
    float3 _pad2;
};

cbuffer MaterialConstants : register(b1) {
    float4 g_BaseColorFactor;
    float g_Metallic;
    float g_Roughness;
    uint g_BaseColorTexture;
    uint g_NormalTexture;
    uint g_MetallicRoughnessTexture;
};

Texture2D g_Textures[16] : register(t0, space1);
SamplerState g_Sampler : register(s0);

static const uint kInvalidTextureIndex = 0xFFFFFFFF;

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    PSInput result;
    float4 worldPos = mul(g_Model, float4(input.position, 1.0));
    result.worldPos = worldPos.xyz;
    result.position = mul(g_MVP, float4(input.position, 1.0));
    result.normal = mul((float3x3)g_Model, input.normal);
    result.uv = input.uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 L = normalize(-g_LightDir);
    float3 V = normalize(g_CameraPos - input.worldPos);
    float3 H = normalize(V + L);

    float3 albedo = g_BaseColorFactor.rgb;
    if (g_BaseColorTexture != kInvalidTextureIndex) {
        albedo *= g_Textures[g_BaseColorTexture].Sample(g_Sampler, input.uv).rgb;
    }

    float ambient = 0.15;
    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(N, H), 0.0), 32.0) * (1.0 - g_Roughness) * 0.5;

    float3 color = albedo * (ambient + diffuse) + float3(1.0, 1.0, 1.0) * specular;
    color = lerp(color, float3(1.0, 0.85, 0.2), g_Highlight * 0.6);
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
)";

struct GltfFrameConstants {
    Mat4 mvp;
    Mat4 model;
    Vec3 camera_pos;
    f32 pad0;
    Vec3 light_dir;
    f32 pad1;
    f32 highlight;
    Vec3 pad2;
};

struct GltfSimpleVertex {
    f32 pos[3];
    f32 normal[3];
    f32 uv[2];
};

ComPtr<ID3D12RootSignature> CreateGltfRootSignature(Device& device) {
    D3D12_DESCRIPTOR_RANGE bindless_range{};
    bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindless_range.NumDescriptors = kGltfBindlessCapacity;
    bindless_range.BaseShaderRegister = 0;
    bindless_range.RegisterSpace = 1;
    bindless_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0,
                            /*Num32BitValues=*/sizeof(GltfFrameConstants) / 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {/*ShaderRegister=*/1, /*RegisterSpace=*/0, /*Num32BitValues=*/sizeof(MaterialData) / 4};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {1, &bindless_range};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("Editor", "glTF root signature serialization failed: %s", message);
        throw std::runtime_error("gltf root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreateGltfPSO(Device& device, ID3D12RootSignature* root_signature,
                                           DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kGltfShaderSource, "VSMain", "vs_5_1", "editor_gltf_vs");
    ShaderBytecode ps = CompileHLSL(kGltfShaderSource, "PSMain", "ps_5_1", "editor_gltf_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GltfSimpleVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GltfSimpleVertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GltfSimpleVertex, uv),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 3};
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = rtv_format;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

// Everything needed to draw one glTF asset: geometry buffers, the resolved
// material, and the parsed GltfScene itself (kept around for its
// node_instances/animations, re-evaluated in place each frame — see the
// main loop). Cached by asset path in a std::unordered_map (GltfCache,
// below) and shared by every ModelRenderer entity that references the same
// path, the same way AssetManager already shares a decoded texture across
// materials that reference it — spawning ten entities pointing at one
// .gltf file uploads its geometry/texture exactly once. `animate`/
// `anim_time` are per-asset, not per-entity: two entities sharing one
// animated asset play in lockstep, a scope choice rather than a limitation
// worth solving before this asset actually has two independent instances
// that need to desync.
struct GltfRenderData {
    assets::GltfScene scene;
    std::unique_ptr<Buffer> vertex_buffer;
    std::unique_ptr<Buffer> index_buffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    MaterialData material;
    usize index_count = 0;
    bool valid = false; // false if LoadGltf failed — draw calls skip it, the UI still shows it as an entry
    bool animate = true;
    f32 anim_time = 0.0f;
};

using GltfCache = std::unordered_map<std::string, std::unique_ptr<GltfRenderData>>;

GltfRenderData& GetOrLoadGltfRenderData(Device& device, assets::AssetManager& asset_manager, GltfCache& cache,
                                        const std::string& path) {
    auto existing = cache.find(path);
    if (existing != cache.end()) {
        return *existing->second;
    }

    auto data = std::make_unique<GltfRenderData>();
    std::string full_path = std::string(AETHER_ASSET_DIR) + path;
    bool ok = assets::LoadGltf(full_path, data->scene) && !data->scene.meshes.empty() &&
              !data->scene.meshes[0].primitives.empty();
    if (!ok) {
        AETHER_LOG_ERROR("Editor", "Failed to load glTF model \"%s\" — entity will render nothing", full_path.c_str());
        GltfRenderData& ref = *data;
        cache.emplace(path, std::move(data));
        return ref;
    }
    if (data->scene.node_instances.empty()) {
        data->scene.node_instances.push_back({0, Mat4::Identity()});
    }

    const assets::GltfPrimitive& primitive = data->scene.meshes[0].primitives[0];
    data->index_count = primitive.indices.size();

    std::vector<GltfSimpleVertex> vertices(primitive.vertices.size());
    for (usize i = 0; i < primitive.vertices.size(); ++i) {
        std::memcpy(vertices[i].pos, primitive.vertices[i].position, sizeof(f32) * 3);
        std::memcpy(vertices[i].normal, primitive.vertices[i].normal, sizeof(f32) * 3);
        std::memcpy(vertices[i].uv, primitive.vertices[i].uv, sizeof(f32) * 2);
    }
    data->vertex_buffer =
        std::make_unique<Buffer>(device, vertices.size() * sizeof(GltfSimpleVertex), BufferKind::Upload);
    data->vertex_buffer->Update(vertices.data(), vertices.size() * sizeof(GltfSimpleVertex));
    data->index_buffer = std::make_unique<Buffer>(device, data->index_count * sizeof(u32), BufferKind::Upload);
    data->index_buffer->Update(primitive.indices.data(), data->index_count * sizeof(u32));

    data->vbv.BufferLocation = data->vertex_buffer->GPUAddress();
    data->vbv.SizeInBytes = static_cast<UINT>(data->vertex_buffer->Size());
    data->vbv.StrideInBytes = sizeof(GltfSimpleVertex);
    data->ibv.BufferLocation = data->index_buffer->GPUAddress();
    data->ibv.SizeInBytes = static_cast<UINT>(data->index_buffer->Size());
    data->ibv.Format = DXGI_FORMAT_R32_UINT;

    CommandList setup_cmd(device);
    setup_cmd.Reset();
    if (primitive.material_index >= 0 &&
        static_cast<usize>(primitive.material_index) < data->scene.materials.size()) {
        data->material =
            LoadMaterial(data->scene.materials[primitive.material_index], asset_manager, setup_cmd.Get());
    }
    setup_cmd.Close();
    ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
    device.WaitForFence(device.Submit(setup_lists, 1));

    data->valid = true;
    AETHER_LOG_INFO("Editor", "Loaded glTF model \"%s\": %zu vert(s), %zu index(es), %zu animation(s)",
                     full_path.c_str(), primitive.vertices.size(), data->index_count, data->scene.animations.size());

    GltfRenderData& ref = *data;
    cache.emplace(path, std::move(data));
    return ref;
}

// Scans assets/models/ for .gltf files — the "in-editor asset picker"
// follow-up: the Add Model panel lists whatever this returns instead of a
// path hardcoded in source.
std::vector<std::string> ListAvailableGltfModels() {
    std::vector<std::string> paths;
    std::filesystem::path models_dir = std::filesystem::path(AETHER_ASSET_DIR) / "models";
    if (!std::filesystem::exists(models_dir)) {
        return paths;
    }
    for (const auto& entry : std::filesystem::directory_iterator(models_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".gltf") {
            paths.push_back("models/" + entry.path().filename().string());
        }
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

Entity SpawnSphere(World& world, PhysicsWorld& physics, const Vec3& position, f32 radius, f32 mass) {
    JPH::BodyID body_id = physics.CreateSphere(position, radius, mass, /*is_static=*/false);
    RigidBody body;
    body.body_id = body_id;
    body.radius = radius;
    body.mass = mass;
    body.is_static = false;
    return world.CreateEntity(Transform{position, Quaternion::Identity()}, body);
}

// Same DXGI-flip-model-aware backbuffer readback as gltf_demo/pbr_demo (see
// their comment on why the buffer index must be the caller-tracked "last
// rendered" one, not CurrentBackBuffer()).
void SaveBackbufferScreenshot(Device& device, SwapChain& swap_chain, u32 buffer_index, const std::string& path) {
    ID3D12Resource* back_buffer = swap_chain.BackBuffer(buffer_index);
    D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    Buffer readback(device, total_bytes, BufferKind::Readback);

    CommandList cmd(device);
    cmd.Reset();

    D3D12_RESOURCE_BARRIER to_copy_src =
        TransitionBarrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &to_copy_src);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = back_buffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Handle();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER to_present =
        TransitionBarrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    cmd->ResourceBarrier(1, &to_present);

    cmd.Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    device.WaitForFence(device.Submit(lists, 1));

    std::vector<u8> raw(total_bytes);
    readback.Read(raw.data(), total_bytes);

    u32 width = static_cast<u32>(desc.Width);
    u32 height = desc.Height;
    std::vector<u8> tight(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(&tight[static_cast<usize>(y) * width * 4],
                    &raw[static_cast<usize>(y) * footprint.Footprint.RowPitch], static_cast<usize>(width) * 4);
    }

    stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, tight.data(),
                   static_cast<int>(width) * 4);
    AETHER_LOG_INFO("Editor", "Wrote screenshot to \"%s\" (%ux%u)", path.c_str(), width, height);
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_EDITOR_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }
    const char* screenshot_path = std::getenv("AETHER_EDITOR_SCREENSHOT");

    try {
        RegisterPhysicsComponentSerializers();

        WindowDesc window_desc;
        window_desc.title = "Aether Editor - Phase 5";
        window_desc.width = 1280;
        window_desc.height = 800;
        Window window(window_desc);

        Device device(/*enable_debug_layer=*/true);
        SwapChain swap_chain(device, window.NativeHandle(), window.Width(), window.Height());

        RenderGraph graph(device);

        // A real depth buffer — added alongside the glTF integration
        // follow-up's 3D mesh, since the billboard-only editor before this
        // never needed one (see CreatePSO's comment).
        auto create_depth_buffer = [&](u32 width, u32 height) {
            D3D12_RESOURCE_DESC depth_desc{};
            depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depth_desc.Width = width;
            depth_desc.Height = height;
            depth_desc.DepthOrArraySize = 1;
            depth_desc.MipLevels = 1;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.SampleDesc.Count = 1;
            depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = DXGI_FORMAT_D32_FLOAT;
            clear_value.DepthStencil.Depth = 1.0f;

            return graph.CreateTransientTexture(depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                                 "EditorDepth");
        };
        RenderGraph::ResourceHandle depth_handle = create_depth_buffer(window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) {
            swap_chain.Resize(w, h);
            depth_handle = create_depth_buffer(w, h);
        };

        window.native_message_hook = [](void* hwnd, u32 msg, u64 wparam, i64 lparam) {
            ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), msg, static_cast<WPARAM>(wparam),
                                            static_cast<LPARAM>(lparam));
        };

        // A dedicated heap for ImGui's own SRVs (font atlas plus whatever
        // dynamic textures 1.92's texture-update system needs), kept
        // separate from any future bindless texture heap so the two
        // allocation schemes never collide. Sized generously since this
        // backend version can allocate more than just the one font
        // descriptor older versions needed.
        DescriptorHeap imgui_srv_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, /*shader_visible=*/true);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGui_ImplWin32_Init(window.NativeHandle());

        ImGui_ImplDX12_InitInfo init_info{};
        init_info.Device = device.Handle();
        init_info.CommandQueue = device.Queue();
        init_info.NumFramesInFlight = static_cast<int>(swap_chain.BufferCount());
        init_info.RTVFormat = swap_chain.Format();
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = imgui_srv_heap.Heap();
        init_info.UserData = &imgui_srv_heap;
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                                             D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
            auto* heap = static_cast<DescriptorHeap*>(info->UserData);
            u32 slot = heap->Allocate();
            *out_cpu = heap->CPUHandle(slot);
            *out_gpu = heap->GPUHandle(slot);
        };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                            D3D12_GPU_DESCRIPTOR_HANDLE) {
            // No-op: this demo never unloads ImGui textures during its
            // lifetime, so leaking the (tiny, fixed-capacity) heap slot back
            // to DescriptorHeap isn't needed. A long-running editor reusing
            // dynamic textures would want a real CPU<->GPU handle lookup here.
        };
        ImGui_ImplDX12_Init(&init_info);

        ComPtr<ID3D12RootSignature> root_signature = CreateRootSignature(device);
        ComPtr<ID3D12PipelineState> pso = CreatePSO(device, root_signature.Get(), swap_chain.Format());

        Buffer view_proj_buffer(device, sizeof(f32) * 16, BufferKind::Upload);
        Buffer instance_buffer(device, sizeof(EditorInstance) * kMaxInstances, BufferKind::Upload);

        World world;
        JobSystem job_system;
        PhysicsWorld physics(job_system);
        physics.CreateBox(Vec3(0, 0, 0), Vec3(10, 0.5f, 10));

        std::mt19937 rng(1234);
        std::uniform_real_distribution<f32> spread(-3.0f, 3.0f);
        std::uniform_real_distribution<f32> height(4.0f, 10.0f);
        for (int i = 0; i < 8; ++i) {
            SpawnSphere(world, physics, Vec3(spread(rng), height(rng), spread(rng)), 0.5f, 1.0f);
        }

        // glTF integration, now via the ECS: the rendering infrastructure
        // (pipeline, bindless heap, asset manager, cache) is built
        // unconditionally, since models are spawned/removed at runtime
        // rather than one being hardcoded and always present.
        ComPtr<ID3D12RootSignature> gltf_root_signature = CreateGltfRootSignature(device);
        ComPtr<ID3D12PipelineState> gltf_pso = CreateGltfPSO(device, gltf_root_signature.Get(), swap_chain.Format());
        DescriptorHeap gltf_bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kGltfBindlessCapacity,
                                           /*shader_visible=*/true);
        assets::AssetManager gltf_asset_manager(device, gltf_bindless_heap);
        GltfCache gltf_cache;
        std::vector<std::string> available_gltf_models = ListAvailableGltfModels();

        // The one model the earlier glTF integration follow-up hardcoded is
        // now just the first entity spawned into a scene that can hold any
        // number of them, added/removed at runtime like the physics spheres
        // already are.
        {
            Entity model_entity =
                world.CreateEntity(Transform{Vec3(-2.0f, 1.2f, 3.5f), Quaternion::Identity()}, ModelRenderer{});
            SetModelPath(*world.GetComponent<ModelRenderer>(model_entity), "models/test_animation.gltf");
        }

        // The editor's one form of "selection" so far — see the Inspector
        // panel below and the top-of-file comment for what this does and
        // doesn't cover yet.
        Entity selected_entity = kNullEntity;

        bool playing = true;
        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        AETHER_LOG_INFO("Editor", "Entering main loop");

        i32 frame_index = 0;
        u32 last_rendered_buffer_index = 0;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            constexpr f32 kDt = 1.0f / 60.0f;
            if (playing) {
                SyncPhysicsToTransforms(world, physics, kDt);
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            // Every currently-referenced model asset must be loaded before
            // this frame's draw commands are recorded — loading uploads via
            // its own short-lived command list + fence wait (see
            // GetOrLoadGltfRenderData), which can't happen from inside the
            // RenderGraph pass lambda below.
            ForEachWithEntity<ModelRenderer>(world, [&](Entity, ModelRenderer& renderer) {
                GetOrLoadGltfRenderData(device, gltf_asset_manager, gltf_cache, renderer.asset_path);
            });

            std::vector<EditorInstance> instances;
            ForEachWithEntity<Transform, RigidBody>(world, [&](Entity e, Transform& t, RigidBody& b) {
                if (instances.size() >= kMaxInstances) {
                    return;
                }
                bool is_selected = (e == selected_entity);
                EditorInstance inst{};
                inst.center[0] = t.position.x;
                inst.center[1] = t.position.y;
                inst.center[2] = t.position.z;
                inst.radius = b.radius;
                if (is_selected) {
                    inst.color[0] = 1.0f;
                    inst.color[1] = 0.85f;
                    inst.color[2] = 0.2f;
                } else {
                    inst.color[0] = 0.3f;
                    inst.color[1] = 0.6f + 0.05f * static_cast<f32>(instances.size() % 5);
                    inst.color[2] = 0.9f;
                }
                instances.push_back(inst);
            });

            std::vector<Entity> entities_to_delete;

            ImGui::Begin("Aether Editor");
            ImGui::Text("Entities: %zu", world.EntityCount());
            ImGui::Checkbox("Playing", &playing);
            if (ImGui::Button("Spawn Sphere")) {
                SpawnSphere(world, physics, Vec3(spread(rng), 8.0f, spread(rng)), 0.5f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Scene")) {
                SaveScene(world, kScenePath);
            }
            ImGui::SameLine();
            if (ImGui::Button("Load Scene")) {
                World loaded;
                if (LoadScene(loaded, kScenePath)) {
                    world = std::move(loaded);
                    selected_entity = kNullEntity; // stale after a full world swap
                    // The serializer never persists a live Jolt body handle
                    // (see RegisterPhysicsComponentSerializers) — reconnect
                    // every loaded RigidBody to a fresh body in this
                    // PhysicsWorld, seeded from its Transform + saved shape.
                    // ModelRenderer needs no such reconnection: asset_path is
                    // plain data, and GetOrLoadGltfRenderData above re-loads
                    // (or finds already-cached) GPU resources for it lazily.
                    world.ForEach<Transform, RigidBody>([&](Transform& t, RigidBody& b) {
                        b.body_id = physics.CreateSphere(t.position, b.radius, b.mass, b.is_static);
                    });
                }
            }

            // In-editor asset picker: lists whatever ListAvailableGltfModels()
            // found under assets/models/ at startup, instead of a path
            // hardcoded in source — click one to spawn a new entity
            // referencing it.
            ImGui::Separator();
            ImGui::Text("Add Model");
            if (available_gltf_models.empty()) {
                ImGui::TextDisabled("(no .gltf files found under assets/models/)");
            }
            for (const std::string& model_path : available_gltf_models) {
                ImGui::PushID(model_path.c_str());
                if (ImGui::Button(model_path.c_str())) {
                    Vec3 spawn_pos(spread(rng), 1.2f, spread(rng) + 3.5f);
                    Entity new_entity = world.CreateEntity(Transform{spawn_pos, Quaternion::Identity()},
                                                            ModelRenderer{});
                    SetModelPath(*world.GetComponent<ModelRenderer>(new_entity), model_path);
                }
                ImGui::PopID();
            }

            ImGui::Separator();
            ImGui::Text("Bodies (live-edit; changes apply to the running simulation immediately)");
            int index = 0;
            ForEachWithEntity<Transform, RigidBody>(world, [&](Entity e, Transform& t, RigidBody& b) {
                ImGui::PushID(index);
                ImGui::Text("#%d", index);
                ImGui::SameLine();
                if (ImGui::SmallButton(e == selected_entity ? "Selected" : "Select")) {
                    selected_entity = e;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    entities_to_delete.push_back(e);
                }

                bool position_changed = ImGui::DragFloat3("Position", &t.position.x, 0.05f);
                bool radius_changed = ImGui::DragFloat("Radius", &b.radius, 0.01f, 0.05f, 5.0f, "%.2f");
                bool mass_changed = ImGui::DragFloat("Mass", &b.mass, 0.05f, 0.01f, 100.0f, "%.2f");
                bool static_changed = ImGui::Checkbox("Static", &b.is_static);

                if (position_changed) {
                    // Only re-teleport the body if the shape/mass didn't also
                    // change this frame — that branch below already recreates
                    // it at the (already updated) Transform position.
                    if (!radius_changed && !mass_changed && !static_changed) {
                        physics.SetPosition(b.body_id, t.position);
                    }
                }
                if (radius_changed || mass_changed || static_changed) {
                    // Jolt shapes and motion type are effectively immutable
                    // once a body is created; the simplest correct way to
                    // "edit" them live is to recreate the body in place.
                    physics.DestroyBody(b.body_id);
                    b.body_id = physics.CreateSphere(t.position, b.radius, b.mass, b.is_static);
                }

                ImGui::Separator();
                ImGui::PopID();
                ++index;
            });

            // Same list treatment for model entities, now that a model is a
            // normal (Transform, ModelRenderer) entity instead of a special
            // case rendered outside the ECS entirely.
            ImGui::Text("Models");
            int model_index = 0;
            ForEachWithEntity<Transform, ModelRenderer>(world, [&](Entity e, Transform& t, ModelRenderer& renderer) {
                ImGui::PushID(1000 + model_index); // offset so IDs never collide with the Bodies loop above
                ImGui::Text("#%d %s", model_index, renderer.asset_path);
                ImGui::SameLine();
                if (ImGui::SmallButton(e == selected_entity ? "Selected" : "Select")) {
                    selected_entity = e;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    entities_to_delete.push_back(e);
                }
                ImGui::DragFloat3("Position", &t.position.x, 0.05f);
                ImGui::Separator();
                ImGui::PopID();
                ++model_index;
            });
            ImGui::End();

            // Inspector: shows only the selected entity, regardless of which
            // list it was selected from — the "click-to-select" workflow's
            // payoff panel. Selection itself is still list-based (a "Select"
            // button per row above), not a true click-in-viewport pick —
            // that needs screen-to-world ray casting against each entity's
            // bounds, real future work.
            if (!selected_entity.IsNull()) {
                ImGui::Begin("Inspector");
                if (world.HasComponent<RigidBody>(selected_entity)) {
                    Transform& t = *world.GetComponent<Transform>(selected_entity);
                    RigidBody& b = *world.GetComponent<RigidBody>(selected_entity);
                    ImGui::Text("Physics body");
                    ImGui::DragFloat3("Position##inspector", &t.position.x, 0.05f);
                    ImGui::Text("Radius: %.2f  Mass: %.2f  Static: %s", b.radius, b.mass, b.is_static ? "yes" : "no");
                } else if (world.HasComponent<ModelRenderer>(selected_entity)) {
                    Transform& t = *world.GetComponent<Transform>(selected_entity);
                    ModelRenderer& renderer = *world.GetComponent<ModelRenderer>(selected_entity);
                    ImGui::Text("glTF model");
                    ImGui::Text("Source: %s", renderer.asset_path);
                    ImGui::DragFloat3("Position##inspector", &t.position.x, 0.05f);
                    GltfRenderData& data = GetOrLoadGltfRenderData(device, gltf_asset_manager, gltf_cache,
                                                                    renderer.asset_path);
                    if (!data.valid) {
                        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failed to load — see the log");
                    } else {
                        ImGui::Text("%zu index(es), material baseColor=(%.2f,%.2f,%.2f)", data.index_count,
                                    data.material.base_color[0], data.material.base_color[1],
                                    data.material.base_color[2]);
                        if (!data.scene.animations.empty()) {
                            ImGui::Checkbox("Animate", &data.animate);
                            ImGui::Text("Time: %.2f / %.2fs", data.anim_time, data.scene.animations[0].duration);
                        } else {
                            ImGui::Text("(no animations in this asset)");
                        }
                    }
                }
                if (ImGui::Button("Deselect")) {
                    selected_entity = kNullEntity;
                }
                ImGui::End();
            }

            ImGui::Render();

            for (Entity e : entities_to_delete) {
                if (world.HasComponent<RigidBody>(e)) {
                    physics.DestroyBody(world.GetComponent<RigidBody>(e)->body_id);
                }
                if (e == selected_entity) {
                    selected_entity = kNullEntity;
                }
                world.DestroyEntity(e);
            }

            // Advance each cached asset's animation once per frame (not once
            // per entity referencing it — see GltfRenderData's comment on
            // why that's shared, not per-instance).
            for (auto& [path, data] : gltf_cache) {
                if (!data->valid || data->scene.animations.empty()) {
                    continue;
                }
                const assets::GltfAnimation& animation = data->scene.animations[0];
                if (data->animate) {
                    data->anim_time += kDt;
                    if (animation.duration > 0.0f) {
                        data->anim_time = std::fmod(data->anim_time, animation.duration);
                    }
                }
                assets::EvaluateAnimation(data->scene, animation, data->anim_time, data->scene.node_instances);
            }

            Mat4 view = Mat4::LookAtRH(Vec3(0, 6, -14), Vec3(0, 1, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::PerspectiveRH(Radians(60.0f),
                                             static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()),
                                             0.1f, 100.0f);
            Mat4 view_proj = proj * view;
            view_proj_buffer.Update(&view_proj, sizeof(f32) * 16);
            if (!instances.empty()) {
                instance_buffer.Update(instances.data(), sizeof(EditorInstance) * instances.size());
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            last_rendered_buffer_index = buffer_index;
            device.WaitForFence(frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            cmd.Reset();

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");

            graph.AddPass(
                "EditorForward",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Write(backbuffer_handle, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    builder.Write(depth_handle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                },
                [&](ID3D12GraphicsCommandList* cl) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap_chain.CurrentBackBufferRTV();
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv = graph.GetOrCreateDSV(depth_handle);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                    const f32 clear_color[4] = {0.03f, 0.03f, 0.05f, 1.0f};
                    cl->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
                    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(swap_chain.Width()),
                                             static_cast<f32>(swap_chain.Height()), 0.0f, 1.0f};
                    D3D12_RECT scissor{0, 0, static_cast<LONG>(swap_chain.Width()),
                                        static_cast<LONG>(swap_chain.Height())};
                    cl->RSSetViewports(1, &viewport);
                    cl->RSSetScissorRects(1, &scissor);

                    if (!instances.empty()) {
                        cl->SetPipelineState(pso.Get());
                        cl->SetGraphicsRootSignature(root_signature.Get());
                        cl->SetGraphicsRootConstantBufferView(0, view_proj_buffer.GPUAddress());
                        cl->SetGraphicsRootShaderResourceView(1, instance_buffer.GPUAddress());
                        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                        cl->DrawInstanced(6, static_cast<UINT>(instances.size()), 0, 0);
                    }

                    // Every (Transform, ModelRenderer) entity draws with its
                    // own Transform as the model's world placement — no more
                    // single hardcoded offset, since a model is now a normal
                    // entity that can be spawned/moved/deleted like any
                    // other.
                    bool any_model_bound = false;
                    ForEachWithEntity<Transform, ModelRenderer>(
                        world, [&](Entity e, Transform& t, ModelRenderer& renderer) {
                            auto it = gltf_cache.find(renderer.asset_path);
                            if (it == gltf_cache.end() || !it->second->valid) {
                                return;
                            }
                            GltfRenderData& data = *it->second;

                            if (!any_model_bound) {
                                cl->SetPipelineState(gltf_pso.Get());
                                cl->SetGraphicsRootSignature(gltf_root_signature.Get());
                                ID3D12DescriptorHeap* gltf_heaps[] = {gltf_bindless_heap.Heap()};
                                cl->SetDescriptorHeaps(1, gltf_heaps);
                                cl->SetGraphicsRootDescriptorTable(2, gltf_bindless_heap.GPUHandle(0));
                                cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                                any_model_bound = true;
                            }
                            cl->IASetVertexBuffers(0, 1, &data.vbv);
                            cl->IASetIndexBuffer(&data.ibv);
                            cl->SetGraphicsRoot32BitConstants(1, sizeof(MaterialData) / 4, &data.material, 0);

                            Mat4 entity_transform = Mat4::Translation(t.position) * t.rotation.ToMat4();
                            f32 highlight = (e == selected_entity) ? 1.0f : 0.0f;
                            for (const assets::GltfNodeInstance& node_instance : data.scene.node_instances) {
                                Mat4 model = entity_transform * node_instance.world_transform;
                                GltfFrameConstants gltf_frame_constants{};
                                gltf_frame_constants.model = model;
                                gltf_frame_constants.mvp = view_proj * model;
                                gltf_frame_constants.camera_pos = Vec3(0, 6, -14);
                                gltf_frame_constants.light_dir = Vec3(-0.4f, -0.8f, -0.3f).Normalized();
                                gltf_frame_constants.highlight = highlight;
                                cl->SetGraphicsRoot32BitConstants(0, sizeof(GltfFrameConstants) / 4,
                                                                   &gltf_frame_constants, 0);
                                cl->DrawIndexedInstanced(static_cast<UINT>(data.index_count), 1, 0, 0, 0);
                            }
                        });

                    ID3D12DescriptorHeap* imgui_heaps[] = {imgui_srv_heap.Heap()};
                    cl->SetDescriptorHeaps(1, imgui_heaps);
                    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
                });

            graph.Execute(cmd.Get());
            graph.Reset();

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("Editor", "Reached AETHER_EDITOR_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device.WaitForFence(fence);
        }

        if (screenshot_path) {
            SaveBackbufferScreenshot(device, swap_chain, last_rendered_buffer_index, screenshot_path);
        }

        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        AETHER_LOG_INFO("Editor", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("Editor", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
