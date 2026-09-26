// Roadmap item: glTF mesh loading (aether::assets::LoadGltf,
// engine/include/aether/assets/gltf_loader.h). This demo is the end-to-end
// proof: load a *real* glTF 2.0 asset from disk (JSON parsed via
// nlohmann::json, vertex data pulled from an external .bin buffer file —
// exercising the file-based buffer path the unit tests' embedded-base64
// examples don't cover), build real GPU vertex/index buffers directly from
// the loaded data, and render it — not just parse it successfully and stop.
//
// Deliberately simple shading (Lambertian diffuse + Blinn-Phong specular,
// not the full Cook-Torrance PBR pipeline pbr_demo/ implements): this demo's
// job is proving the loaded mesh/material data is genuinely usable, not
// re-proving the BRDF. The loaded material's baseColorFactor drives the
// albedo directly.
//
// assets/models/test_cube.gltf + test_cube.bin is a hand-written (not
// exported from a DCC tool) 24-vertex, 6-face cube with per-face normals and
// UVs, referencing its vertex data from the external .bin file.
//
// Set AETHER_GLTF_DEMO_MAX_FRAMES=<N> to auto-close after N frames instead
// of waiting for the window to be closed. Set AETHER_GLTF_DEMO_SCREENSHOT=
// <path> to dump the final frame to a PNG on exit.

#include "aether/assets/gltf_loader.h"
#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/device.h"
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

constexpr const char* kShaderSource = R"(
cbuffer Constants : register(b0) {
    float4x4 g_MVP;
    float4x4 g_Model;
    float3 g_Albedo;
    float _pad0;
    float3 g_CameraPos;
    float _pad1;
    float3 g_LightDir;
    float _pad2;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
};

PSInput VSMain(VSInput input) {
    PSInput result;
    float4 worldPos = mul(g_Model, float4(input.position, 1.0));
    result.worldPos = worldPos.xyz;
    result.position = mul(g_MVP, float4(input.position, 1.0));
    result.normal = mul((float3x3)g_Model, input.normal);
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float3 N = normalize(input.normal);
    float3 L = normalize(-g_LightDir);
    float3 V = normalize(g_CameraPos - input.worldPos);
    float3 H = normalize(V + L);

    float ambient = 0.12;
    float diffuse = max(dot(N, L), 0.0);
    float specular = pow(max(dot(N, H), 0.0), 32.0) * 0.5;

    float3 color = g_Albedo * (ambient + diffuse) + float3(1.0, 1.0, 1.0) * specular;
    color = color / (color + 1.0);
    color = pow(color, 1.0 / 2.2);
    return float4(color, 1.0);
}
)";

struct Constants {
    Mat4 mvp;
    Mat4 model;
    Vec3 albedo;
    f32 pad0;
    Vec3 camera_pos;
    f32 pad1;
    Vec3 light_dir;
    f32 pad2;
};

struct SimpleVertex {
    f32 pos[3];
    f32 normal[3];
};

ComPtr<ID3D12RootSignature> CreateRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/sizeof(Constants) / 4};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 1;
    desc.pParameters = &param;
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

    D3D12_INPUT_ELEMENT_DESC input_elements[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SimpleVertex, pos),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(SimpleVertex, normal),
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.InputLayout = {input_elements, 2};
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

// Same DXGI-flip-model-aware backbuffer readback as pbr_demo (see its
// comment on why the buffer index must be the caller-tracked "last
// rendered" one, not CurrentBackBuffer()).
void SaveBackbufferScreenshot(Device& device, SwapChain& swap_chain, u32 buffer_index, const std::string& path) {
    ID3D12Resource* back_buffer = swap_chain.BackBuffer(buffer_index);
    D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 row_pitch = 0;
    u64 total_bytes = 0;
    device.Handle()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, &row_pitch, &total_bytes);

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
        std::memcpy(&tight[static_cast<usize>(y) * width * 4], &raw[static_cast<usize>(y) * row_pitch],
                    static_cast<usize>(width) * 4);
    }

    stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, tight.data(),
                   static_cast<int>(width) * 4);
    AETHER_LOG_INFO("GltfDemo", "Wrote screenshot to \"%s\" (%ux%u)", path.c_str(), width, height);
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_GLTF_DEMO_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }
    const char* screenshot_path = std::getenv("AETHER_GLTF_DEMO_SCREENSHOT");

    try {
        assets::GltfScene scene;
        std::string gltf_path = std::string(AETHER_ASSET_DIR) + "models/test_cube.gltf";
        if (!assets::LoadGltf(gltf_path, scene)) {
            AETHER_LOG_FATAL("GltfDemo", "Failed to load \"%s\"", gltf_path.c_str());
            return 1;
        }
        if (scene.meshes.empty() || scene.meshes[0].primitives.empty()) {
            AETHER_LOG_FATAL("GltfDemo", "Loaded glTF has no renderable primitives");
            return 1;
        }
        const assets::GltfPrimitive& primitive = scene.meshes[0].primitives[0];
        Vec3 albedo(1.0f, 1.0f, 1.0f);
        if (primitive.material_index >= 0 &&
            static_cast<usize>(primitive.material_index) < scene.materials.size()) {
            const assets::GltfMaterial& material = scene.materials[primitive.material_index];
            albedo = Vec3(material.base_color[0], material.base_color[1], material.base_color[2]);
        }
        AETHER_LOG_INFO("GltfDemo", "Loaded mesh: %zu vertices, %zu indices, albedo=(%.2f,%.2f,%.2f)",
                         primitive.vertices.size(), primitive.indices.size(), albedo.x, albedo.y, albedo.z);

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

        std::vector<SimpleVertex> vertices(primitive.vertices.size());
        for (usize i = 0; i < primitive.vertices.size(); ++i) {
            std::memcpy(vertices[i].pos, primitive.vertices[i].position, sizeof(f32) * 3);
            std::memcpy(vertices[i].normal, primitive.vertices[i].normal, sizeof(f32) * 3);
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
            Vec3 camera_pos(std::sin(t) * 3.0f, 1.5f, std::cos(t) * 3.0f);
            Mat4 view = Mat4::LookAtRH(camera_pos, Vec3(0, 0, 0), Vec3(0, 1, 0));
            Mat4 proj = Mat4::PerspectiveRH(
                Radians(50.0f), static_cast<f32>(swap_chain.Width()) / static_cast<f32>(swap_chain.Height()), 0.1f,
                100.0f);
            Mat4 model = Mat4::Identity();

            Constants constants{};
            constants.mvp = proj * view * model;
            constants.model = model;
            constants.albedo = albedo;
            constants.camera_pos = camera_pos;
            constants.light_dir = Vec3(-0.4f, -0.8f, -0.3f).Normalized();

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
                    cl->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    cl->IASetVertexBuffers(0, 1, &vbv);
                    cl->IASetIndexBuffer(&ibv);
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
