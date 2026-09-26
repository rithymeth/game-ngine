// Phase 4 proof-of-life: bindless textures + GPU-driven frustum culling +
// a render graph tying it together, extended with a transient depth buffer
// and true async compute (a follow-up pass).
//
// Scene: 64 textured quads scattered along Z, most of them beyond the far
// plane or outside the frustum. A compute pass culls them against 6 planes
// extracted from the view-projection matrix and writes one indirect-draw
// command per instance (InstanceCount 0 or 1) into a GPU buffer; a graphics
// pass then issues all 64 draws in a single ExecuteIndirect call — the CPU
// never learns which instances survived. Both passes go through a
// RenderGraph, which is what inserts the UAV<->indirect-argument barrier on
// the commands buffer, the depth buffer's transition into DEPTH_WRITE, and
// the backbuffer's RENDER_TARGET<->PRESENT barriers, instead of any pass
// hand-rolling ResourceBarrier calls.
//
// The depth buffer is a RenderGraph *transient* resource (RenderGraph
// allocates and owns it, unlike the imported swap chain backbuffer) —
// created once via CreateTransientTexture and recreated in place on resize;
// the Forward pass's returned ResourceHandle never changes.
//
// The culling pass is tagged QueueType::Compute and runs on
// Device::ComputeQueue(), a genuinely separate hardware queue from the
// Forward pass's graphics queue — real async compute, not just a compute
// pass sharing the graphics queue. The two queues are connected by a GPU-
// side fence wait (Device::GraphicsQueueWaitOnCompute), not a CPU stall, so
// they can overlap on hardware that supports it.
//
// The vertex/pixel shaders index a `Texture2D g_Textures[]` array by a
// per-instance integer (its bindless heap index) passed through a root
// constant — that's the whole bindless mechanism: bind the descriptor heap
// once, select an element per draw with an integer, no per-draw descriptor
// table rebinding.
//
// Set AETHER_SANDBOX_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.

#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/command_list.h"
#include "aether/gfx/descriptor_heap.h"
#include "aether/gfx/device.h"
#include "aether/gfx/render_graph.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/gfx/texture.h"
#include "aether/math/math.h"
#include "aether/platform/window.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace aether;
using namespace aether::gfx;

namespace {

constexpr u32 kInstanceCount = 64;
constexpr u32 kBindlessCapacity = 256;

// Layout mirrors the HLSL InstanceData struct exactly (32 bytes, no padding
// surprises: structured buffers pack like a C struct, unlike cbuffers).
struct InstanceData {
    f32 center[3];
    f32 radius;
    u32 texture_index;
    f32 pad[3];
};

// Layout mirrors HLSL IndirectCommand: 1 root-constant DWORD (instance
// index) immediately followed by a D3D12_DRAW_ARGUMENTS-shaped block — that
// pairing is exactly what the command signature below describes.
struct IndirectCommand {
    u32 instance_index;
    u32 vertex_count_per_instance;
    u32 instance_count;
    u32 start_vertex_location;
    u32 start_instance_location;
};

constexpr const char* kGraphicsShaderSource = R"(
struct InstanceData {
    float3 center;
    float radius;
    uint textureIndex;
    float3 _pad;
};
StructuredBuffer<InstanceData> g_Instances : register(t0, space0);

cbuffer RootConstants : register(b0) { uint g_InstanceIndex; };
cbuffer ViewProj : register(b1) { float4x4 g_ViewProj; };

Texture2D g_Textures[256] : register(t0, space1);
SamplerState g_Sampler : register(s0);

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureIndex : TEXCOORD1;
};

static const float2 kQuadPositions[6] = {
    float2(-0.5, -0.5), float2(-0.5, 0.5), float2(0.5, -0.5),
    float2(0.5, -0.5), float2(-0.5, 0.5), float2(0.5, 0.5),
};
static const float2 kQuadUVs[6] = {
    float2(0, 1), float2(0, 0), float2(1, 1),
    float2(1, 1), float2(0, 0), float2(1, 0),
};

PSInput VSMain(uint vertexID : SV_VertexID) {
    InstanceData inst = g_Instances[g_InstanceIndex];
    float2 localPos = kQuadPositions[vertexID] * inst.radius;
    float3 worldPos = inst.center + float3(localPos, 0.0);

    PSInput result;
    result.position = mul(g_ViewProj, float4(worldPos, 1.0));
    result.uv = kQuadUVs[vertexID];
    result.textureIndex = inst.textureIndex;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return g_Textures[input.textureIndex].Sample(g_Sampler, input.uv);
}
)";

constexpr const char* kCullingShaderSource = R"(
struct InstanceData {
    float3 center;
    float radius;
    uint textureIndex;
    float3 _pad;
};
struct IndirectCommand {
    uint instanceIndex;
    uint vertexCountPerInstance;
    uint instanceCount;
    uint startVertexLocation;
    uint startInstanceLocation;
};

cbuffer FrustumPlanes : register(b0) { float4 g_Planes[6]; };
cbuffer InstanceCountCB : register(b1) { uint g_InstanceCount; };
StructuredBuffer<InstanceData> g_Instances : register(t0, space0);
RWStructuredBuffer<IndirectCommand> g_Commands : register(u0, space0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint index = id.x;
    // g_Instances is bound via a root descriptor (a raw GPU address, no view
    // metadata), so GetDimensions() has no size to report here — the count
    // has to come from somewhere else, hence this root constant instead.
    if (index >= g_InstanceCount) {
        return;
    }

    InstanceData inst = g_Instances[index];
    bool visible = true;
    [unroll]
    for (int i = 0; i < 6; ++i) {
        if (dot(g_Planes[i].xyz, inst.center) + g_Planes[i].w + inst.radius < 0.0) {
            visible = false;
        }
    }

    IndirectCommand cmd;
    cmd.instanceIndex = index;
    cmd.vertexCountPerInstance = 6;
    cmd.instanceCount = visible ? 1 : 0;
    cmd.startVertexLocation = 0;
    cmd.startInstanceLocation = 0;
    g_Commands[index] = cmd;
}
)";

// Gribb-Hartmann plane extraction for a column-vector convention (clip =
// M * point, matching Mat4::operator*(Vec4) and PerspectiveRH/LookAtRH),
// D3D depth range [0, w]. Each Vec4 column c's element `row` is the matrix
// entry at (row, c) — see Mat4's column-major storage.
f32 Elem(const Mat4& m, int row, int col) {
    const Vec4& c = m.cols[col];
    switch (row) {
        case 0: return c.x;
        case 1: return c.y;
        case 2: return c.z;
        default: return c.w;
    }
}

void ExtractFrustumPlanes(const Mat4& view_proj, f32 out_planes[6][4]) {
    auto row = [&](int r) { return Vec4(Elem(view_proj, r, 0), Elem(view_proj, r, 1), Elem(view_proj, r, 2),
                                         Elem(view_proj, r, 3)); };
    Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    Vec4 planes[6] = {
        r3 + r0, // left
        r3 - r0, // right
        r3 + r1, // bottom
        r3 - r1, // top
        r2,      // near (D3D: z >= 0)
        r3 - r2, // far
    };
    for (int i = 0; i < 6; ++i) {
        f32 len = std::sqrt(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
        f32 inv = (len > 0.0f) ? 1.0f / len : 0.0f;
        out_planes[i][0] = planes[i].x * inv;
        out_planes[i][1] = planes[i].y * inv;
        out_planes[i][2] = planes[i].z * inv;
        out_planes[i][3] = planes[i].w * inv;
    }
}

ComPtr<ID3D12RootSignature> SerializeRootSignature(Device& device, const D3D12_ROOT_SIGNATURE_DESC& desc) {
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

ComPtr<ID3D12RootSignature> CreateGraphicsRootSignature(Device& device) {
    D3D12_DESCRIPTOR_RANGE bindless_range{};
    bindless_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindless_range.NumDescriptors = kBindlessCapacity;
    bindless_range.BaseShaderRegister = 0;
    bindless_range.RegisterSpace = 1;
    bindless_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/1};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {/*ShaderRegister=*/1, /*RegisterSpace=*/0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor = {/*ShaderRegister=*/0, /*RegisterSpace=*/0};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &bindless_range};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    return SerializeRootSignature(device, desc);
}

ComPtr<ID3D12RootSignature> CreateComputeRootSignature(Device& device) {
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor = {0, 0};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0};
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants = {/*ShaderRegister=*/1, /*RegisterSpace=*/0, /*Num32BitValues=*/1};
    for (auto& p : params) {
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = _countof(params);
    desc.pParameters = params;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    return SerializeRootSignature(device, desc);
}

ComPtr<ID3D12PipelineState> CreateGraphicsPSO(Device& device, ID3D12RootSignature* root_signature,
                                               DXGI_FORMAT rtv_format) {
    ShaderBytecode vs = CompileHLSL(kGraphicsShaderSource, "VSMain", "vs_5_1", "quad_vs");
    ShaderBytecode ps = CompileHLSL(kGraphicsShaderSource, "PSMain", "ps_5_1", "quad_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.VS = {vs.Data(), vs.Size()};
    desc.PS = {ps.Data(), ps.Size()};

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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

ComPtr<ID3D12PipelineState> CreateComputePSO(Device& device, ID3D12RootSignature* root_signature) {
    ShaderBytecode cs = CompileHLSL(kCullingShaderSource, "CSMain", "cs_5_1", "cull_cs");

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature;
    desc.CS = {cs.Data(), cs.Size()};

    ComPtr<ID3D12PipelineState> pso;
    AETHER_D3D_CHECK(device.Handle()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
    return pso;
}

ComPtr<ID3D12CommandSignature> CreateIndirectCommandSignature(Device& device, ID3D12RootSignature* graphics_root) {
    D3D12_INDIRECT_ARGUMENT_DESC args[2]{};
    args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    args[0].Constant = {/*RootParameterIndex=*/0, /*DestOffsetIn32BitValues=*/0, /*Num32BitValuesToSet=*/1};
    args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(IndirectCommand);
    desc.NumArgumentDescs = _countof(args);
    desc.pArgumentDescs = args;

    ComPtr<ID3D12CommandSignature> command_signature;
    AETHER_D3D_CHECK(
        device.Handle()->CreateCommandSignature(&desc, graphics_root, IID_PPV_ARGS(&command_signature)));
    return command_signature;
}

// A tiny procedural RGBA8 checkerboard, so the demo has something to sample
// without depending on an image loader (that's future asset-pipeline work).
std::vector<u8> MakeCheckerboardTexture(u32 size, u8 r0, u8 g0, u8 b0, u8 r1, u8 g1, u8 b1) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            bool even = ((x / 8) + (y / 8)) % 2 == 0;
            u8* p = &pixels[(static_cast<usize>(y) * size + x) * 4];
            p[0] = even ? r0 : r1;
            p[1] = even ? g0 : g1;
            p[2] = even ? b0 : b1;
            p[3] = 255;
        }
    }
    return pixels;
}

std::vector<InstanceData> MakeInstances() {
    std::vector<InstanceData> instances(kInstanceCount);
    for (u32 i = 0; i < kInstanceCount; ++i) {
        f32 z = -8.0f + static_cast<f32>(i) * 3.0f;
        InstanceData& inst = instances[i];
        inst.center[0] = std::sin(static_cast<f32>(i) * 0.9f) * 3.0f;
        inst.center[1] = std::cos(static_cast<f32>(i) * 1.3f) * 2.0f;
        inst.center[2] = z;
        inst.radius = 1.0f;
        inst.texture_index = i % 2;
    }
    return instances;
}

} // namespace

int main() {
    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_SANDBOX_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }

    try {
        WindowDesc window_desc;
        window_desc.title = "Aether Sandbox - Phase 4";
        window_desc.width = 1280;
        window_desc.height = 720;
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
                                                 "SandboxDepth");
        };
        RenderGraph::ResourceHandle depth_handle = create_depth_buffer(window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) {
            swap_chain.Resize(w, h);
            depth_handle = create_depth_buffer(w, h);
        };

        DescriptorHeap bindless_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kBindlessCapacity,
                                      /*shader_visible=*/true);

        ComPtr<ID3D12RootSignature> graphics_root = CreateGraphicsRootSignature(device);
        ComPtr<ID3D12RootSignature> compute_root = CreateComputeRootSignature(device);
        ComPtr<ID3D12PipelineState> graphics_pso = CreateGraphicsPSO(device, graphics_root.Get(), swap_chain.Format());
        ComPtr<ID3D12PipelineState> compute_pso = CreateComputePSO(device, compute_root.Get());
        ComPtr<ID3D12CommandSignature> command_signature = CreateIndirectCommandSignature(device, graphics_root.Get());

        // One-time setup command list: uploads both textures.
        CommandList setup_cmd(device);
        setup_cmd.Reset();

        std::vector<u8> checker_a = MakeCheckerboardTexture(64, 220, 60, 60, 40, 10, 10);
        std::vector<u8> checker_b = MakeCheckerboardTexture(64, 60, 140, 220, 10, 30, 60);
        Texture texture_a(device, bindless_heap, setup_cmd.Get(), 64, 64, checker_a.data());
        Texture texture_b(device, bindless_heap, setup_cmd.Get(), 64, 64, checker_b.data());

        setup_cmd.Close();
        ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
        device.WaitForFence(device.Submit(setup_lists, 1));

        std::vector<InstanceData> instance_data = MakeInstances();
        for (auto& inst : instance_data) {
            inst.texture_index = (inst.texture_index == 0) ? texture_a.BindlessIndex() : texture_b.BindlessIndex();
        }

        Buffer instance_buffer(device, sizeof(InstanceData) * kInstanceCount, BufferKind::Upload);
        instance_buffer.Update(instance_data.data(), sizeof(InstanceData) * kInstanceCount);

        Buffer view_proj_buffer(device, sizeof(f32) * 16, BufferKind::Upload);
        Buffer frustum_planes_buffer(device, sizeof(f32) * 4 * 6, BufferKind::Upload);

        Buffer commands_buffer(device, sizeof(IndirectCommand) * kInstanceCount, BufferKind::Default,
                                /*allow_uav=*/true);
        Buffer commands_readback(device, sizeof(IndirectCommand) * kInstanceCount, BufferKind::Readback);

        Mat4 view = Mat4::LookAtRH(Vec3(0, 0, -10), Vec3(0, 0, 0), Vec3(0, 1, 0));
        Mat4 proj = Mat4::PerspectiveRH(Radians(60.0f),
                                         static_cast<f32>(window.Width()) / static_cast<f32>(window.Height()), 0.1f,
                                         60.0f);
        Mat4 view_proj = proj * view;
        view_proj_buffer.Update(&view_proj, sizeof(f32) * 16);

        f32 planes[6][4];
        ExtractFrustumPlanes(view_proj, planes);
        frustum_planes_buffer.Update(planes, sizeof(planes));

        std::vector<std::unique_ptr<CommandList>> command_lists;
        std::vector<std::unique_ptr<CommandList>> compute_command_lists;
        std::vector<u64> frame_fences(swap_chain.BufferCount(), 0);
        std::vector<u64> compute_frame_fences(swap_chain.BufferCount(), 0);
        for (u32 i = 0; i < swap_chain.BufferCount(); ++i) {
            command_lists.push_back(std::make_unique<CommandList>(device));
            compute_command_lists.push_back(
                std::make_unique<CommandList>(device, D3D12_COMMAND_LIST_TYPE_COMPUTE));
        }

        AETHER_LOG_INFO("Sandbox", "Entering main loop (%u instances, bindless capacity %u)", kInstanceCount,
                         kBindlessCapacity);

        bool logged_visibility = false;
        i32 frame_index = 0;
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            u32 buffer_index = swap_chain.CurrentBackBufferIndex();
            device.WaitForFence(frame_fences[buffer_index]);
            device.WaitForComputeFence(compute_frame_fences[buffer_index]);

            CommandList& cmd = *command_lists[buffer_index];
            CommandList& compute_cmd = *compute_command_lists[buffer_index];
            cmd.Reset();
            compute_cmd.Reset();

            ID3D12Resource* back_buffer = swap_chain.CurrentBackBuffer();
            RenderGraph::ResourceHandle backbuffer_handle =
                graph.ImportResource(back_buffer, D3D12_RESOURCE_STATE_PRESENT, "BackBuffer");
            RenderGraph::ResourceHandle commands_handle =
                graph.ImportResource(commands_buffer.Handle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "IndirectCommands");

            // Culling runs on the async compute queue: it only ever needs
            // UNORDERED_ACCESS, a state compute command lists can transition
            // into, so this pass's barrier records cleanly onto compute_cmd.
            // The Forward pass's own transition into INDIRECT_ARGUMENT below
            // happens on the graphics queue instead (compute lists can't
            // transition into that state) — RenderGraph picks the right
            // command list for each automatically from the pass's QueueType.
            graph.AddPass(
                "Culling",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Write(commands_handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                },
                [&](ID3D12GraphicsCommandList* cl) {
                    cl->SetPipelineState(compute_pso.Get());
                    cl->SetComputeRootSignature(compute_root.Get());
                    cl->SetComputeRootConstantBufferView(0, frustum_planes_buffer.GPUAddress());
                    cl->SetComputeRootShaderResourceView(1, instance_buffer.GPUAddress());
                    cl->SetComputeRootUnorderedAccessView(2, commands_buffer.GPUAddress());
                    cl->SetComputeRoot32BitConstant(3, kInstanceCount, 0);
                    cl->Dispatch((kInstanceCount + 63) / 64, 1, 1);
                },
                QueueType::Compute);

            graph.AddPass(
                "Forward",
                [&](RenderGraph::PassBuilder& builder) {
                    builder.Write(backbuffer_handle, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    builder.Write(depth_handle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    builder.Read(commands_handle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                },
                [&](ID3D12GraphicsCommandList* cl) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap_chain.CurrentBackBufferRTV();
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv = graph.GetOrCreateDSV(depth_handle);
                    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                    const f32 clear_color[4] = {0.05f, 0.05f, 0.08f, 1.0f};
                    cl->ClearRenderTargetView(rtv, clear_color, 0, nullptr);
                    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

                    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(swap_chain.Width()),
                                             static_cast<f32>(swap_chain.Height()), 0.0f, 1.0f};
                    D3D12_RECT scissor{0, 0, static_cast<LONG>(swap_chain.Width()),
                                        static_cast<LONG>(swap_chain.Height())};
                    cl->RSSetViewports(1, &viewport);
                    cl->RSSetScissorRects(1, &scissor);

                    cl->SetPipelineState(graphics_pso.Get());
                    cl->SetGraphicsRootSignature(graphics_root.Get());
                    cl->SetGraphicsRootConstantBufferView(1, view_proj_buffer.GPUAddress());
                    cl->SetGraphicsRootShaderResourceView(2, instance_buffer.GPUAddress());
                    ID3D12DescriptorHeap* heaps[] = {bindless_heap.Heap()};
                    cl->SetDescriptorHeaps(1, heaps);
                    cl->SetGraphicsRootDescriptorTable(3, bindless_heap.GPUHandle(0));
                    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                    cl->ExecuteIndirect(command_signature.Get(), kInstanceCount, commands_buffer.Handle(), 0, nullptr,
                                         0);
                });

            if (!logged_visibility) {
                // Diagnostic-only: copy the freshly-culled commands buffer
                // out to a readback buffer so we can log how many of the 64
                // instances survived culling. The CPU never uses this to
                // drive rendering — ExecuteIndirect above already consumed
                // the GPU-only commands buffer directly.
                graph.AddPass(
                    "CullingReadback",
                    [&](RenderGraph::PassBuilder& builder) {
                        builder.Read(commands_handle, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    },
                    [&](ID3D12GraphicsCommandList* cl) {
                        cl->CopyResource(commands_readback.Handle(), commands_buffer.Handle());
                    });
            }

            graph.Execute(cmd.Get(), compute_cmd.Get());
            graph.Reset();

            // Submit compute first and get the graphics queue to wait on it
            // via a GPU-side fence wait (Device::GraphicsQueueWaitOnCompute)
            // rather than a CPU stall — this is the actual async-compute
            // primitive: the two queues' work can overlap on hardware that
            // supports it, with only the *dependency* (graphics needs
            // culling's output) enforced, not a full serialization.
            compute_cmd.Close();
            ID3D12CommandList* compute_lists[] = {compute_cmd.Get()};
            u64 compute_fence_value = device.SubmitCompute(compute_lists, 1);
            compute_frame_fences[buffer_index] = compute_fence_value;
            device.GraphicsQueueWaitOnCompute(compute_fence_value);

            cmd.Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            frame_fences[buffer_index] = device.Submit(lists, 1);

            swap_chain.Present(/*vsync=*/true);

            if (!logged_visibility) {
                device.WaitForFence(frame_fences[buffer_index]);
                std::vector<IndirectCommand> readback(kInstanceCount);
                commands_readback.Read(readback.data(), sizeof(IndirectCommand) * kInstanceCount);
                u32 visible_count = 0;
                for (auto& c : readback) {
                    visible_count += c.instance_count;
                }
                AETHER_LOG_INFO("Sandbox", "GPU-driven culling: %u/%u instances visible", visible_count,
                                 kInstanceCount);
                logged_visibility = true;
            }

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("Sandbox", "Reached AETHER_SANDBOX_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        AETHER_LOG_INFO("Sandbox", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("Sandbox", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
