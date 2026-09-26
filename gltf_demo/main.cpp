// Roadmap items: glTF mesh loading (aether::assets::LoadGltf) and the
// material system (aether::gfx::MaterialData / LoadMaterial). This demo is
// the end-to-end proof for both: load a *real* glTF 2.0 asset from disk
// (JSON parsed via nlohmann::json, vertex data pulled from an external .bin
// buffer file), resolve its material's texture through AssetManager into a
// bindless index via LoadMaterial, and actually sample that texture in the
// pixel shader — not just parse the file and stop.
//
// assets/models/test_textured_cube.gltf + test_cube.bin is a hand-written
// (not exported from a DCC tool) 24-vertex, 6-face cube with per-face
// normals and UVs, referencing assets/textures/checker_a.png (already used
// elsewhere — see the asset pipeline's README section) as its
// baseColorTexture.
//
// Shading is deliberately simple (Lambertian diffuse + Blinn-Phong
// specular, not the full Cook-Torrance PBR pipeline pbr_demo/ implements):
// this demo's job is proving the loaded mesh/material/texture data is
// genuinely usable, not re-proving the BRDF.
//
// Set AETHER_GLTF_DEMO_MAX_FRAMES=<N> to auto-close after N frames instead
// of waiting for the window to be closed. Set AETHER_GLTF_DEMO_SCREENSHOT=
// <path> to dump the final frame to a PNG on exit.

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
#include "aether/math/mat4.h"
#include "aether/math/math.h"
#include "aether/platform/window.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace aether;
using namespace aether::gfx;

namespace {

constexpr u32 kBindlessCapacity = 64;

constexpr const char* kShaderSource = R"(
cbuffer FrameConstants : register(b0) {
    float4x4 g_MVP;
    float4x4 g_Model;
    float3 g_CameraPos;
    float _pad0;
    float3 g_LightDir;
    float _pad1;
};

cbuffer MaterialConstants : register(b1) {
    float4 g_BaseColorFactor;
    float g_Metallic;
    float g_Roughness;
    uint g_BaseColorTexture;
    uint g_NormalTexture;
    uint g_MetallicRoughnessTexture;
};

Texture2D g_Textures[64] : register(t0, space1);
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

    // The material system's actual contract: a texture index of
    // kInvalidTextureIndex means "no such texture on this material", fall
    // back to the factor alone — same as the glTF spec's own
    // factor-times-texture-or-factor-alone semantics.
    float3 albedo = g_BaseColorFactor.rgb;
    if (g_BaseColorTexture != kInvalidTextureIndex) {
        albedo *= g_Textures[g_BaseColorTexture].Sample(g_Sampler, input.uv).rgb;
    }

    float ambient = 0.12;
    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(N, H), 0.0), 32.0) * (1.0 - g_Roughness) * 0.5;

    float3 color = albedo * (ambient + diffuse) + float3(1.0, 1.0, 1.0) * specular;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
)";

struct FrameConstants {
    Mat4 mvp;
    Mat4 model;
    Vec3 camera_pos;
    f32 pad0;
    Vec3 light_dir;
    f32 pad1;
};

struct SimpleVertex {
    f32 pos[3];
    f32 normal[3];
    f32 uv[2];
};

ComPtr<ID3D12RootSignature> CreateRootSignature(Device& device) {
    D3D12_DESCRIPTOR_RANGE bindless_range{};
    bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindless_range.NumDescriptors = kBindlessCapacity;
    bindless_range.BaseShaderRegister = 0;
    bindless_range.RegisterSpace = 1;
    bindless_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/sizeof(FrameConstants) / 4};
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
        AETHER_LOG_FATAL("GltfDemo", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreatePSO(Device& device, ID3D12RootSignature* root_signature, DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kShaderSource, "VSMain", "vs_5_1", "gltf_vs");
    ShaderBytecode ps = CompileHLSL(kShaderSource, "PSMain", "ps_5_1", "gltf_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SimpleVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SimpleVertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(SimpleVertex, uv),
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

// --------------------------------------------------------------------------
// GPU vertex skinning follow-up: consumes the joint_indices/joint_weights
// GltfPrimitive already parses and the skin-matrix math EvaluateAnimation's
// sibling ComputeSkinMatrices already computes — the two pieces the glTF
// animation follow-up's README section flagged as "parsed and unit-tested,
// but no shader consumes them yet". This is a deliberately separate, unlit
// shader/pipeline/root-signature from the main textured-material path above
// (no bindless textures, no material system) — the point here is proving
// the skinning math renders correctly, not re-proving texture sampling.
// --------------------------------------------------------------------------

constexpr u32 kMaxSkinJoints = 8;

constexpr const char* kSkinnedShaderSource = R"(
cbuffer FrameConstants : register(b0) {
    float4x4 g_MVP;
    float4x4 g_Model;
    float3 g_CameraPos;
    float _pad0;
    float3 g_LightDir;
    float _pad1;
    float3 g_Color;
    float _pad2;
};

#define kMaxSkinJoints 8
cbuffer JointMatrices : register(b1) {
    float4x4 g_Joints[kMaxSkinJoints];
};

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
};

PSInput VSMain(VSInput input) {
    // Linear blend skinning: each joint's matrix scaled by this vertex's
    // weight for that joint, summed — the standard technique (matches
    // GltfPrimitive::joint_weights' documented per-vertex weight
    // semantics). Unused influences (this demo's mesh only ever uses 2 of
    // the 4 available slots) contribute zero automatically since their
    // weight is 0, regardless of what garbage/duplicate joint index sits in
    // that slot.
    float4x4 skinMat = g_Joints[input.joints.x] * input.weights.x + g_Joints[input.joints.y] * input.weights.y +
                        g_Joints[input.joints.z] * input.weights.z + g_Joints[input.joints.w] * input.weights.w;

    float4 skinnedPos = mul(skinMat, float4(input.position, 1.0));
    float3 skinnedNormal = mul((float3x3)skinMat, input.normal);

    float4 worldPos = mul(g_Model, skinnedPos);
    PSInput result;
    result.worldPos = worldPos.xyz;
    result.position = mul(g_MVP, skinnedPos);
    result.normal = mul((float3x3)g_Model, skinnedNormal);
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 L = normalize(-g_LightDir);
    float diffuse = max(dot(N, L), 0.0);
    float3 color = g_Color * (0.25 + diffuse * 0.9);
    color = pow(saturate(color), 1.0 / 2.2);
    return float4(color, 1.0);
}
)";

struct SkinFrameConstants {
    Mat4 mvp;
    Mat4 model;
    Vec3 camera_pos;
    f32 pad0;
    Vec3 light_dir;
    f32 pad1;
    Vec3 color;
    f32 pad2;
};

struct SkinnedVertex {
    f32 pos[3];
    f32 normal[3];
    u32 joints[4];
    f32 weights[4];
};

ComPtr<ID3D12RootSignature> CreateSkinRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0,
                            /*Num32BitValues=*/sizeof(SkinFrameConstants) / 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // A root CBV, not inline 32-bit constants: kMaxSkinJoints (8) 4x4
    // matrices is 512 bytes / 128 DWORDs, well past the 64-DWORD root-
    // constants budget a single root parameter can hold.
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {/*ShaderRegister=*/1, /*RegisterSpace=*/0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("GltfDemo", "Skin root signature serialization failed: %s", message);
        throw std::runtime_error("skin root signature serialization failed");
    }
    ComPtr<ID3D12RootSignature> root_signature;
    AETHER_D3D_CHECK(device.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                           IID_PPV_ARGS(&root_signature)));
    return root_signature;
}

ComPtr<ID3D12PipelineState> CreateSkinPSO(Device& device, ID3D12RootSignature* root_signature,
                                           DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kSkinnedShaderSource, "VSMain", "vs_5_1", "gltf_skin_vs");
    ShaderBytecode ps = CompileHLSL(kSkinnedShaderSource, "PSMain", "ps_5_1", "gltf_skin_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[4] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SkinnedVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SkinnedVertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(SkinnedVertex, joints),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(SkinnedVertex, weights),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 4};
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // the ribbon is a thin double-sided strip
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

// Same DXGI-flip-model-aware backbuffer readback as pbr_demo (see its
// comment on why the buffer index must be the caller-tracked "last
// rendered" one, not CurrentBackBuffer()).
void SaveBackbufferScreenshot(Device& device, SwapChain& swap_chain, u32 buffer_index, const std::string& path) {
    ID3D12Resource* back_buffer = swap_chain.BackBuffer(buffer_index);
    D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    // The 7th output param (left null here) is the UNPADDED row size, not
    // the actual stride between rows in the copied buffer
    // (footprint.Footprint.RowPitch, 256-byte aligned) — see rhi_demo's
    // SaveD3D12Screenshot for the real bug this caused there (not visible at
    // this demo's 1024x768, where 1024*4=4096 already happens to be
    // 256-aligned).
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
    AETHER_LOG_INFO("GltfDemo", "Wrote screenshot to \"%s\" (%ux%u)", path.c_str(), width, height);
}

// AETHER_GLTF_DEMO_SKIN=1's entire demo: loads assets/models/
// test_skinned_ribbon.gltf (a 2-joint, 12-vertex ribbon — see that file's
// comment) and plays back its animation with real GPU vertex skinning each
// frame, rather than the CPU-side node-hierarchy playback
// AETHER_GLTF_DEMO_ANIMATE=1 already proved. Deliberately a separate
// function (own window/device/pipeline), not a branch inside main()'s
// shared setup: the vertex layout, shader, and root signature are entirely
// different from the textured-material path, so there's very little to
// actually share.
int RunSkinnedDemo(i32 max_frames, const char* screenshot_path) {
    try {
        assets::GltfScene scene;
        std::string gltf_path = std::string(AETHER_ASSET_DIR) + "models/test_skinned_ribbon.gltf";
        if (!assets::LoadGltf(gltf_path, scene)) {
            AETHER_LOG_FATAL("GltfDemo", "Failed to load \"%s\"", gltf_path.c_str());
            return 1;
        }
        if (scene.meshes.empty() || scene.meshes[0].primitives.empty() || scene.skins.empty() ||
            scene.animations.empty()) {
            AETHER_LOG_FATAL("GltfDemo", "\"%s\" is missing a mesh/skin/animation", gltf_path.c_str());
            return 1;
        }
        const assets::GltfPrimitive& primitive = scene.meshes[0].primitives[0];
        if (primitive.joint_indices.empty() || primitive.joint_weights.empty()) {
            AETHER_LOG_FATAL("GltfDemo", "\"%s\"'s primitive has no JOINTS_0/WEIGHTS_0 data", gltf_path.c_str());
            return 1;
        }
        const assets::GltfSkin& skin = scene.skins[0];
        const assets::GltfAnimation& animation = scene.animations[0];
        AETHER_LOG_INFO("GltfDemo", "Loaded skinned mesh: %zu vertices, %zu indices, %zu joint(s), duration=%.2fs",
                         primitive.vertices.size(), primitive.indices.size(), skin.joints.size(), animation.duration);
        AETHER_ASSERT(skin.joints.size() <= kMaxSkinJoints);

        WindowDesc window_desc;
        window_desc.title = "Aether glTF Demo - GPU Skinning";
        window_desc.width = 1024;
        window_desc.height = 768;
        Window window(window_desc);

        Device device(/*enable_debug_layer=*/true);
        SwapChain swap_chain(device, window.NativeHandle(), window.Width(), window.Height());

        RenderGraph graph(device);

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
                                                 "GltfSkinDepth");
        };
        RenderGraph::ResourceHandle depth_handle = create_depth_buffer(window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) {
            swap_chain.Resize(w, h);
            depth_handle = create_depth_buffer(w, h);
        };

        ComPtr<ID3D12RootSignature> root_signature = CreateSkinRootSignature(device);
        ComPtr<ID3D12PipelineState> pso = CreateSkinPSO(device, root_signature.Get(), swap_chain.Format());

        std::vector<SkinnedVertex> vertices(primitive.vertices.size());
        for (usize i = 0; i < primitive.vertices.size(); ++i) {
            std::memcpy(vertices[i].pos, primitive.vertices[i].position, sizeof(f32) * 3);
            std::memcpy(vertices[i].normal, primitive.vertices[i].normal, sizeof(f32) * 3);
            for (usize k = 0; k < 4; ++k) {
                vertices[i].joints[k] = primitive.joint_indices[i][k];
            }
            std::memcpy(vertices[i].weights, primitive.joint_weights[i].data(), sizeof(f32) * 4);
        }

        Buffer vertex_buffer(device, vertices.size() * sizeof(SkinnedVertex), BufferKind::Upload);
        vertex_buffer.Update(vertices.data(), vertices.size() * sizeof(SkinnedVertex));
        Buffer index_buffer(device, primitive.indices.size() * sizeof(u32), BufferKind::Upload);
        index_buffer.Update(primitive.indices.data(), primitive.indices.size() * sizeof(u32));

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vertex_buffer.GPUAddress();
        vbv.SizeInBytes = static_cast<UINT>(vertex_buffer.Size());
        vbv.StrideInBytes = sizeof(SkinnedVertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = index_buffer.GPUAddress();
        ibv.SizeInBytes = static_cast<UINT>(index_buffer.Size());
        ibv.Format = DXGI_FORMAT_R32_UINT;

        Buffer joint_matrices_buffer(device, kMaxSkinJoints * sizeof(Mat4), BufferKind::Upload);

        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        AETHER_LOG_INFO("GltfDemo", "Entering main loop");

        i32 frame_index = 0;
        f32 t = 0.0f;
        u32 last_rendered_buffer_index = 0;
        std::vector<Mat4> skin_matrices;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            last_rendered_buffer_index = buffer_index;
            device.WaitForFence(frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            cmd.Reset();

            t += 0.01f;
            Vec3 camera_pos(std::sin(t * 0.3f) * 3.5f, 1.5f, std::cos(t * 0.3f) * 3.5f);
            Mat4 view = Mat4::LookAtRH(camera_pos, Vec3(0, 1.0f, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::PerspectiveRH(
                Radians(50.0f), static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()), 0.1f,
                100.0f);
            Mat4 view_proj = proj * view;

            f32 anim_time = animation.duration > 0.0f ? std::fmod(t, animation.duration) : 0.0f;
            assets::ComputeSkinMatrices(scene, &animation, anim_time, skin, skin_matrices);

            std::vector<Mat4> padded_joints(kMaxSkinJoints, Mat4::Identity());
            for (usize i = 0; i < skin_matrices.size() && i < kMaxSkinJoints; ++i) {
                padded_joints[i] = skin_matrices[i];
            }
            joint_matrices_buffer.Update(padded_joints.data(), padded_joints.size() * sizeof(Mat4));

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");

            graph.AddPass(
                "GltfSkinForward",
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

                    cl->SetPipelineState(pso.Get());
                    cl->SetGraphicsRootSignature(root_signature.Get());
                    cl->SetGraphicsRootConstantBufferView(1, joint_matrices_buffer.GPUAddress());
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    cl->IASetVertexBuffers(0, 1, &vbv);
                    cl->IASetIndexBuffer(&ibv);

                    SkinFrameConstants frame_constants{};
                    frame_constants.model = Mat4::Identity();
                    frame_constants.mvp = view_proj;
                    frame_constants.camera_pos = camera_pos;
                    frame_constants.light_dir = Vec3(-0.3f, -0.6f, -0.5f).Normalized();
                    frame_constants.color = Vec3(0.85f, 0.35f, 0.2f);
                    cl->SetGraphicsRoot32BitConstants(0, sizeof(SkinFrameConstants) / 4, &frame_constants, 0);
                    cl->DrawIndexedInstanced(static_cast<UINT>(primitive.indices.size()), 1, 0, 0, 0);
                });

            graph.Execute(cmd.Get(), nullptr);
            graph.Reset();

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("GltfDemo", "Reached AETHER_GLTF_DEMO_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device.WaitForFence(fence);
        }

        if (screenshot_path) {
            SaveBackbufferScreenshot(device, swap_chain, last_rendered_buffer_index, screenshot_path);
        }

        AETHER_LOG_INFO("GltfDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("GltfDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_GLTF_DEMO_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }
    const char* screenshot_path = std::getenv("AETHER_GLTF_DEMO_SCREENSHOT");

    // Set AETHER_GLTF_DEMO_SKIN=1 for the GPU vertex skinning demo (a
    // completely separate pipeline/shader/vertex layout from everything
    // below — see RunSkinnedDemo's comment) — highest priority, same
    // "most-specific mode wins" convention AETHER_RHI_DEMO_DRAW_UNIFIED uses
    // in rhi_demo.
    if (std::getenv("AETHER_GLTF_DEMO_SKIN") != nullptr) {
        return RunSkinnedDemo(max_frames, screenshot_path);
    }

    // Set AETHER_GLTF_DEMO_ANIMATE=1 to load test_animation.gltf (a single
    // cube whose node has a translation animation, see
    // assets/models/test_animation.gltf) and play it back every frame via
    // aether::assets::EvaluateAnimation instead of the static test_scene.gltf
    // hierarchy — the actual end-to-end proof of the glTF animation
    // follow-up (parse -> evaluate -> render), not just that it parses.
    bool animate = std::getenv("AETHER_GLTF_DEMO_ANIMATE") != nullptr;

    try {
        assets::GltfScene scene;
        std::string gltf_path =
            std::string(AETHER_ASSET_DIR) + (animate ? "models/test_animation.gltf" : "models/test_scene.gltf");
        if (!assets::LoadGltf(gltf_path, scene)) {
            AETHER_LOG_FATAL("GltfDemo", "Failed to load \"%s\"", gltf_path.c_str());
            return 1;
        }
        if (scene.meshes.empty() || scene.meshes[0].primitives.empty()) {
            AETHER_LOG_FATAL("GltfDemo", "Loaded glTF has no renderable primitives");
            return 1;
        }
        if (animate && scene.animations.empty()) {
            AETHER_LOG_FATAL("GltfDemo", "AETHER_GLTF_DEMO_ANIMATE=1 but \"%s\" has no animations",
                              gltf_path.c_str());
            return 1;
        }
        if (scene.node_instances.empty()) {
            // Fall back to a single identity-transform instance, for a glTF
            // with meshes but no node/scene graph at all (e.g. the
            // single-cube test assets from before this follow-up).
            scene.node_instances.push_back({0, Mat4::Identity()});
        }
        const assets::GltfPrimitive& primitive = scene.meshes[0].primitives[0];
        AETHER_LOG_INFO("GltfDemo", "Loaded mesh: %zu vertices, %zu indices, %zu node instance(s), %zu animation(s)",
                         primitive.vertices.size(), primitive.indices.size(), scene.node_instances.size(),
                         scene.animations.size());

        WindowDesc window_desc;
        window_desc.title = "Aether glTF Demo";
        window_desc.width = 1024;
        window_desc.height = 768;
        Window window(window_desc);

        Device device(/*enable_debug_layer=*/true);
        SwapChain swap_chain(device, window.NativeHandle(), window.Width(), window.Height());

        RenderGraph graph(device);

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
                                                 "GltfDepth");
        };
        RenderGraph::ResourceHandle depth_handle = create_depth_buffer(window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) {
            swap_chain.Resize(w, h);
            depth_handle = create_depth_buffer(w, h);
        };

        ComPtr<ID3D12RootSignature> root_signature = CreateRootSignature(device);
        ComPtr<ID3D12PipelineState> pso = CreatePSO(device, root_signature.Get(), swap_chain.Format());

        DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kBindlessCapacity,
                                      /*shader_visible=*/true);
        assets::AssetManager asset_manager(device, bindless_heap);

        std::vector<SimpleVertex> vertices(primitive.vertices.size());
        for (usize i = 0; i < primitive.vertices.size(); ++i) {
            std::memcpy(vertices[i].pos, primitive.vertices[i].position, sizeof(f32) * 3);
            std::memcpy(vertices[i].normal, primitive.vertices[i].normal, sizeof(f32) * 3);
            std::memcpy(vertices[i].uv, primitive.vertices[i].uv, sizeof(f32) * 2);
        }

        Buffer vertex_buffer(device, vertices.size() * sizeof(SimpleVertex), BufferKind::Upload);
        vertex_buffer.Update(vertices.data(), vertices.size() * sizeof(SimpleVertex));
        Buffer index_buffer(device, primitive.indices.size() * sizeof(u32), BufferKind::Upload);
        index_buffer.Update(primitive.indices.data(), primitive.indices.size() * sizeof(u32));

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = vertex_buffer.GPUAddress();
        vbv.SizeInBytes = static_cast<UINT>(vertex_buffer.Size());
        vbv.StrideInBytes = sizeof(SimpleVertex);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = index_buffer.GPUAddress();
        ibv.SizeInBytes = static_cast<UINT>(index_buffer.Size());
        ibv.Format = DXGI_FORMAT_R32_UINT;

        // The material system in action: resolve the glTF material's
        // texture reference through AssetManager (decodes + uploads +
        // caches) into a GPU-ready MaterialData with a real bindless index,
        // on a one-time setup command list.
        MaterialData material;
        CommandList setup_cmd(device);
        setup_cmd.Reset();
        if (primitive.material_index >= 0 &&
            static_cast<usize>(primitive.material_index) < scene.materials.size()) {
            material = LoadMaterial(scene.materials[primitive.material_index], asset_manager, setup_cmd.Get());
        }
        setup_cmd.Close();
        ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
        device.WaitForFence(device.Submit(setup_lists, 1));

        AETHER_LOG_INFO("GltfDemo",
                         "Material: baseColor=(%.2f,%.2f,%.2f) metallic=%.2f roughness=%.2f baseColorTexture=%u",
                         material.base_color[0], material.base_color[1], material.base_color[2], material.metallic,
                         material.roughness, material.base_color_texture);

        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
        }

        AETHER_LOG_INFO("GltfDemo", "Entering main loop");

        i32 frame_index = 0;
        f32 t = 0.0f;
        u32 last_rendered_buffer_index = 0;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            last_rendered_buffer_index = buffer_index;
            device.WaitForFence(frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            cmd.Reset();

            t += 0.01f;
            Vec3 camera_pos(std::sin(t) * 6.5f, 2.5f, std::cos(t) * 6.5f);
            Mat4 view = Mat4::LookAtRH(camera_pos, Vec3(0, 0, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::PerspectiveRH(
                Radians(50.0f), static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()), 0.1f,
                100.0f);
            Mat4 view_proj = proj * view;

            if (animate) {
                const assets::GltfAnimation& animation = scene.animations[0];
                f32 anim_time = animation.duration > 0.0f ? std::fmod(t * 2.0f, animation.duration) : 0.0f;
                assets::EvaluateAnimation(scene, animation, anim_time, scene.node_instances);
            }

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");

            graph.AddPass(
                "GltfForward",
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

                    cl->SetPipelineState(pso.Get());
                    cl->SetGraphicsRootSignature(root_signature.Get());
                    ID3D12DescriptorHeap* heaps[] = {bindless_heap.Heap()};
                    cl->SetDescriptorHeaps(1, heaps);
                    cl->SetGraphicsRootDescriptorTable(2, bindless_heap.GPUHandle(0));
                    cl->SetGraphicsRoot32BitConstants(1, sizeof(MaterialData) / 4, &material, 0);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    cl->IASetVertexBuffers(0, 1, &vbv);
                    cl->IASetIndexBuffer(&ibv);

                    // One draw per node instance (root constants are cheap
                    // to re-set between draws on the same command list) —
                    // this is what actually exercises the node hierarchy:
                    // three cubes, one mesh, three different world
                    // transforms coming straight out of GltfScene::node_instances.
                    for (const assets::GltfNodeInstance& instance : scene.node_instances) {
                        FrameConstants frame_constants{};
                        frame_constants.model = instance.world_transform;
                        frame_constants.mvp = view_proj * instance.world_transform;
                        frame_constants.camera_pos = camera_pos;
                        frame_constants.light_dir = Vec3(-0.4f, -0.8f, -0.3f).Normalized();
                        cl->SetGraphicsRoot32BitConstants(0, sizeof(FrameConstants) / 4, &frame_constants, 0);
                        cl->DrawIndexedInstanced(static_cast<UINT>(primitive.indices.size()), 1, 0, 0, 0);
                    }
                });

            graph.Execute(cmd.Get(), nullptr);
            graph.Reset();

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("GltfDemo", "Reached AETHER_GLTF_DEMO_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device.WaitForFence(fence);
        }

        if (screenshot_path) {
            SaveBackbufferScreenshot(device, swap_chain, last_rendered_buffer_index, screenshot_path);
        }

        AETHER_LOG_INFO("GltfDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("GltfDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
