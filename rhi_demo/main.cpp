// Proves the RHI abstraction (engine/include/aether/gfx/rhi) genuinely
// swaps backends: the exact same frame loop, written entirely against
// IDevice/ISwapChain/ICommandList, runs on both D3D12 and Vulkan depending
// on the AETHER_RHI_BACKEND environment variable ("d3d12" or "vulkan",
// default d3d12).
//
// Four demo modes:
//
//  - Default: clear the backbuffer to a color and present, every frame. No
//    shaders, no pipelines, no draw calls — this is the "does the device/
//    swapchain/command-submission layer actually work on both backends"
//    proof (see the RHI header comments for why shaders/pipelines aren't
//    abstracted).
//
//  - AETHER_RHI_DEMO_DRAW_TRIANGLE=1: draws an actual rotating triangle,
//    using the *same* HLSL source compiled two ways — D3DCompile (fxc) to
//    DXIL for the D3D12 root-signature/PSO path, and DXC to SPIR-V for the
//    Vulkan render-pass/pipeline path (see aether/gfx/shader_compiler.h).
//    This intentionally does NOT go through ICommandList::TransitionTexture/
//    ClearRenderTarget: those two calls exist only to support the
//    clear-to-color mode above (see ToVkImageLayout's comment in
//    vulkan_backend.cpp for why RenderTarget maps to TRANSFER_DST there,
//    which is the wrong Vulkan layout for a render-pass color attachment).
//    Triangle mode instead records genuinely backend-specific commands via
//    each object's NativeHandle() escape hatch, exactly as the RHI headers'
//    "shaders/pipelines are not abstracted" comments anticipate.
//
//  - AETHER_RHI_DEMO_DRAW_CUBE=1 (takes priority over DRAW_TRIANGLE if both
//    are set): a depth-tested, indexed cube — real vertex/index buffers
//    (D3D12: gfx::Buffer upload-heap buffers; Vulkan: host-visible
//    VkBuffer/VkDeviceMemory) and a real depth buffer per swapchain image on
//    both backends, driven by an MVP matrix built with the engine's own math
//    (aether::Mat4, the same PerspectiveRH/LookAtRH sandbox/main.cpp uses).
//
//  - AETHER_RHI_DEMO_DRAW_UNIFIED=1 (highest priority — wins over both of the
//    above): the "Unified Cross-API Renderer" follow-up's actual proof, now
//    including its "vertex buffers + textures" and "depth buffer + blend
//    states" follow-ups. Triangle/Cube mode above still each pick between
//    two hand-written backend-specific implementations at startup
//    (`if (backend == ...)`); Unified mode instead calls
//    IDevice::CreatePipeline/CreateVertexBuffer/CreateIndexBuffer/
//    CreateTexture once and then records every frame purely through
//    ICommandList::BeginRenderPass/BindPipeline/BindBindlessTextures/
//    BindVertexBuffer/BindIndexBuffer/SetPushConstants/DrawIndexed/
//    EndRenderPass — the exact same calls, in the exact same order, with
//    zero backend branching in this file, render the same scene on both
//    D3D12 and Vulkan: a rotating textured quad (a real GPU vertex/index
//    buffer pair, a real uploaded GPU texture sampled through a
//    device-global bindless descriptor table), a red/blue quad pair proving
//    PipelineDesc::depth_test (drawn far-then-near in *reverse* depth order
//    — a broken depth test would let the later, farther draw incorrectly
//    win), and a translucent green quad proving PipelineDesc::enable_blending.
//    This used to be procedural-vertices-only with no textures/descriptors,
//    depth testing, or blending at all (see PipelineDesc's comment for
//    what's still narrow: one fixed vertex layout, one push-constant block,
//    a fixed-capacity bindless texture table, one shared depth buffer rather
//    than a general material/mesh/render-target system) — Cube mode's real
//    vertex buffers and depth buffer above still exist as a second, older
//    proof that predates the RHI having its own unified buffer/texture/
//    depth support, and are kept as-is for that historical comparison.
//
// Set AETHER_RHI_DEMO_MAX_FRAMES=<N> to auto-close after N frames instead of
// waiting for the window to be closed, for scripted/automated verification.

#include "aether/core/log.h"
#include "aether/gfx/buffer.h"
#include "aether/gfx/d3d12_common.h"
#include "aether/gfx/rhi/d3d12/d3d12_backend.h"
#include "aether/gfx/rhi/device.h"
#include "aether/gfx/shader_compiler.h"
#include "aether/gfx/swap_chain.h"
#include "aether/math/mat4.h"
#include "aether/math/math.h"
#include "aether/platform/window.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#if defined(AETHER_HAS_VULKAN)
#include "aether/gfx/rhi/vulkan/vulkan_backend.h"
#endif

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace aether;
using namespace aether::gfx::rhi;

namespace {

// Procedural triangle, no vertex buffer (indices come from SV_VertexID /
// gl_VertexIndex, which DXC maps automatically). A single root/push constant
// (the elapsed time) drives a 2D rotation, just so both backends visibly
// have to get *some* per-frame data to the shader correctly, not just a
// static draw.
//
// D3D12/Vulkan disagree on which way NDC +Y points (D3D12: up; Vulkan:
// down) — found to matter here by actually screenshotting both backends'
// output side by side and seeing the triangle come out vertically mirrored.
// Fixed with a negative-height viewport on the Vulkan side (the standard,
// well-known trick — VK_KHR_maintenance1/core-1.1+) rather than a
// shader-side flip constant: a second push-constant field was tried first
// and broke Vulkan rendering outright (pipeline "succeeded" but nothing
// rasterized, on a driver with no validation layers to explain why) for a
// reason not fully root-caused; the negative-viewport approach needs no
// shader or push-constant-layout change at all, sidestepping that entirely.
constexpr const char* kTriangleShaderSource = R"(
cbuffer PushConstants : register(b0) {
    float g_Time;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

static const float2 kPositions[3] = {
    float2(0.0, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5)
};
static const float3 kColors[3] = {
    float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.2), float3(0.2, 0.4, 1.0)
};

PSInput VSMain(uint vertexID : SV_VertexID) {
    float c = cos(g_Time);
    float s = sin(g_Time);
    float2 pos = kPositions[vertexID];
    float2 rotated = float2(pos.x * c - pos.y * s, pos.x * s + pos.y * c);

    PSInput result;
    result.position = float4(rotated, 0.0, 1.0);
    result.color = kColors[vertexID];
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

// ---------------------------------------------------------------------
// Unified mode's textured-quad pipeline: the "vertex buffers + textures"
// follow-up. g_Textures/g_Sampler deliberately live in different HLSL
// register spaces (t0 space0 vs s0 space1) — not because D3D12 needs that,
// but because DXC's default HLSL->SPIR-V binding assignment is
// `binding = register number, set = space number` *regardless of the
// resource's type letter*, so a texture array at t0 and a sampler at s0 in
// the SAME space would both map to Vulkan (set=0, binding=0) and collide
// (illegal SPIR-V — two descriptors can't share a binding). Putting the
// sampler in space1 puts it in a second Vulkan descriptor set instead, with
// zero effect on the D3D12 root signature (which already tracks (register,
// space) per-resource-type independently) — see IDevice::CreatePipeline's
// two backend implementations for how each side wires this up.
// ---------------------------------------------------------------------

constexpr const char* kUnifiedTexturedQuadShaderSource = R"(
// ConstantBuffer<T> + [[vk::push_constant]], not a plain `cbuffer` — DXC
// only recognizes the push_constant attribute on a global variable of
// struct type (a plain HLSL `cbuffer` doesn't qualify), and without it a
// cbuffer compiles to an ordinary Vulkan uniform-buffer descriptor instead
// of an actual push-constant block — silently: no validation layer is
// available in this environment to flag the mismatch (see VulkanDevice's
// class comment), it just reads back whatever garbage happens to occupy
// that unbound descriptor slot, which is how this was actually caught (the
// quad's rotation was permanently stuck at zero on Vulkan while D3D12 —
// unaffected, since D3D12 root 32-bit constants don't go through this
// DXC-specific mapping at all — rotated correctly). `[[vk::push_constant]]`
// on a `ConstantBuffer<T>` global is DXC/fxc-portable: fxc (the D3D12
// path's compiler) ignores the unrecognized `[[vk::...]]` attribute rather
// than erroring on it, and ConstantBuffer<T> at register(b0) still binds to
// the same root-constants slot a plain cbuffer would.
struct PushConstants {
    float g_Time;
    uint g_TextureIndex;
    float g_Depth;
    float g_Opacity;
};
// `[[vk::push_constant]]` uses attribute syntax fxc (the D3D12/D3DCompile
// path's compiler, unlike DXC) can't parse at all — a hard syntax error,
// not a harmless "unrecognized attribute" ignore, so it can't appear
// unconditionally in HLSL source compiled by both. `__spirv__` is a macro
// DXC predefines only when compiling with `-spirv` (see
// CompileHLSLToSPIRV) — never defined for fxc's D3DCompile path — so this
// `#ifdef` is resolved by the C preprocessor stage, before either
// compiler's parser ever sees the attribute token on the branch that
// doesn't apply to it.
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
    float c = cos(g_PC.g_Time);
    float s = sin(g_PC.g_Time);
    float2 rotated = float2(input.position.x * c - input.position.y * s,
                             input.position.x * s + input.position.y * c);
    PSInput result;
    // g_Depth: written straight through as clip-space Z (w=1, so NDC z ==
    // clip z) — the "Unified renderer: depth buffer + blend states"
    // follow-up's per-draw depth value. DXC's SPIR-V codegen keeps D3D's
    // [0,1] depth-range convention rather than OpenGL's [-1,1] (a real,
    // useful DXC/Vulkan quirk in this engine's favor), so a value here
    // means the same thing — "how close to the near plane" — on both
    // backends with no extra remapping needed.
    result.position = float4(rotated, g_PC.g_Depth, 1.0);
    result.uv = input.uv;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    float4 sampled = g_Textures[g_PC.g_TextureIndex].Sample(g_Sampler, input.uv);
    // g_Opacity: multiplies the sampled alpha, independent of the texture's
    // own (fully opaque) alpha — the knob the blend-states half of this
    // follow-up's demo uses to make a translucent draw visually obvious
    // rather than relying on a texture asset that happens to have partial
    // alpha baked in.
    return float4(sampled.rgb, sampled.a * g_PC.g_Opacity);
}
)";

struct UnifiedVertex {
    f32 pos[3];
    f32 uv[2];
};

struct UnifiedPushConstants {
    f32 time;
    u32 texture_index;
    f32 depth;
    f32 opacity;
};

// A flat solid-color texture — used by the depth/blend demo quads below,
// where the point is to tell three overlapping quads apart by color, not to
// re-prove texture sampling (the checkerboard already does that).
std::vector<u8> GenerateSolidColorPixels(u32 size, u8 r, u8 g, u8 b, u8 a) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4);
    for (usize i = 0; i < static_cast<usize>(size) * size; ++i) {
        pixels[i * 4 + 0] = r;
        pixels[i * 4 + 1] = g;
        pixels[i * 4 + 2] = b;
        pixels[i * 4 + 3] = a;
    }
    return pixels;
}

// A tiny procedural checkerboard — deliberately low-resolution and
// high-contrast so a screenshot makes it immediately obvious whether the
// texture actually reached the shader (as opposed to sampling garbage or
// the dummy white placeholder every unclaimed bindless slot starts with).
std::vector<u8> GenerateCheckerboardPixels(u32 size, u32 cell_size) {
    std::vector<u8> pixels(static_cast<usize>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            bool light = ((x / cell_size) + (y / cell_size)) % 2 == 0;
            u8 value = light ? 235 : 40;
            u8* p = &pixels[(static_cast<usize>(y) * size + x) * 4];
            p[0] = value;
            p[1] = light ? value : static_cast<u8>(80);
            p[2] = light ? value : static_cast<u8>(200);
            p[3] = 255;
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------
// D3D12 triangle pipeline
// ---------------------------------------------------------------------

struct D3D12TriangleResources {
    gfx::ComPtr<ID3D12RootSignature> root_signature;
    gfx::ComPtr<ID3D12PipelineState> pso;
};

D3D12TriangleResources CreateD3D12TriangleResources(ID3D12Device* device, DXGI_FORMAT rtv_format) {
    using gfx::ShaderBytecode;

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/1};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    gfx::ComPtr<ID3DBlob> signature;
    gfx::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("RHIDemo", "Root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }

    D3D12TriangleResources result;
    AETHER_D3D_CHECK(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                  IID_PPV_ARGS(&result.root_signature)));

    ShaderBytecode vs = gfx::CompileHLSL(kTriangleShaderSource, "VSMain", "vs_5_0", "triangle_vs");
    ShaderBytecode ps = gfx::CompileHLSL(kTriangleShaderSource, "PSMain", "ps_5_0", "triangle_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = result.root_signature.Get();
    pso_desc.VS = {vs.Data(), vs.Size()};
    pso_desc.PS = {ps.Data(), ps.Size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = rtv_format;
    pso_desc.SampleDesc.Count = 1;

    AETHER_D3D_CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&result.pso)));
    return result;
}

void RecordD3D12TriangleFrame(ICommandList& cmd, gfx::SwapChain& native_swap, const D3D12TriangleResources& res,
                               bool used_before, f32 time) {
    auto* native_cmd = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12Resource* backbuffer = native_swap.CurrentBackBuffer();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = native_swap.CurrentBackBufferRTV();

    D3D12_RESOURCE_STATES before_state = used_before ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER to_rt =
        gfx::TransitionBarrier(backbuffer, before_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    native_cmd->ResourceBarrier(1, &to_rt);

    const f32 clear[4] = {0.02f, 0.02f, 0.05f, 1.0f};
    native_cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
    native_cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{0.0f,
                             0.0f,
                             static_cast<f32>(native_swap.Width()),
                             static_cast<f32>(native_swap.Height()),
                             0.0f,
                             1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(native_swap.Width()), static_cast<LONG>(native_swap.Height())};
    native_cmd->RSSetViewports(1, &viewport);
    native_cmd->RSSetScissorRects(1, &scissor);

    native_cmd->SetGraphicsRootSignature(res.root_signature.Get());
    native_cmd->SetPipelineState(res.pso.Get());
    u32 time_bits;
    std::memcpy(&time_bits, &time, sizeof(time_bits));
    native_cmd->SetGraphicsRoot32BitConstant(0, time_bits, 0);
    native_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native_cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER to_present =
        gfx::TransitionBarrier(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    native_cmd->ResourceBarrier(1, &to_present);
}

// ---------------------------------------------------------------------
// Cube mode: real vertex/index buffers + depth testing (both backends)
// ---------------------------------------------------------------------
//
// Unlike the procedural triangle above (no buffers at all — vertices come
// from SV_VertexID), cube mode exercises actual GPU buffer creation/upload
// identically on both backends, plus a depth-tested 3D draw. One depth
// buffer per swapchain image (not a single shared one): this demo's frame
// loop only fences on a slot right before *reusing* it, so two backbuffers'
// GPU work can genuinely be in flight at once — a single shared depth
// buffer would be a real write/write hazard between them.

struct CubeVertex {
    f32 pos[3];
    f32 color[3];
};

constexpr CubeVertex kCubeVertices[8] = {
    {{-0.5f, -0.5f, -0.5f}, {1.0f, 0.2f, 0.2f}}, {{0.5f, -0.5f, -0.5f}, {0.2f, 1.0f, 0.2f}},
    {{0.5f, 0.5f, -0.5f}, {0.2f, 0.2f, 1.0f}},   {{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f, 0.2f}},
    {{-0.5f, -0.5f, 0.5f}, {1.0f, 0.2f, 1.0f}},  {{0.5f, -0.5f, 0.5f}, {0.2f, 1.0f, 1.0f}},
    {{0.5f, 0.5f, 0.5f}, {0.9f, 0.9f, 0.9f}},    {{-0.5f, 0.5f, 0.5f}, {0.2f, 0.2f, 0.2f}},
};

// CullMode is NONE for this demo (matching the triangle above), so winding
// order doesn't affect visibility — each face just needs its two triangles
// listed once.
constexpr u16 kCubeIndices[36] = {
    0, 1, 2, 2, 3, 0, // back   (z = -0.5)
    5, 4, 7, 7, 6, 5, // front  (z = +0.5)
    4, 0, 3, 3, 7, 4, // left   (x = -0.5)
    1, 5, 6, 6, 2, 1, // right  (x = +0.5)
    4, 5, 1, 1, 0, 4, // bottom (y = -0.5)
    3, 2, 6, 6, 7, 3, // top    (y = +0.5)
};

// FlipY compensates for D3D12/Vulkan's opposite NDC Y convention (D3D12: +Y
// up; Vulkan: +Y down) directly in the shared shader, so the same MVP matrix
// (built with the engine's D3D-style PerspectiveRH/LookAtRH, as sandbox/
// main.cpp already uses) looks right-side-up on both backends without a
// backend-specific projection matrix.
constexpr const char* kCubeShaderSource = R"(
cbuffer PushConstants : register(b0) {
    float4x4 g_MVP;
    float g_FlipY;
};

struct VSInput {
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

PSInput VSMain(VSInput input) {
    PSInput result;
    float4 clip = mul(g_MVP, float4(input.position, 1.0));
    clip.y *= g_FlipY;
    result.position = clip;
    result.color = input.color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

struct CubePushConstants {
    Mat4 mvp;
    f32 flip_y;
};

Mat4 MakeCubeMVP(f32 time, f32 aspect) {
    Mat4 model = Mat4::Translation(Vec3(0, 0, 0));
    f32 c = std::cos(time);
    f32 s = std::sin(time);
    // A simple hand-rolled Y-axis rotation (the engine's Mat4 has no
    // dedicated RotationY helper) composed with a small X tilt for a more
    // legibly-3D silhouette than a pure Y-spin.
    Mat4 rotate_y;
    rotate_y.cols[0] = Vec4(c, 0, -s, 0);
    rotate_y.cols[2] = Vec4(s, 0, c, 0);
    f32 tilt = 0.5f;
    f32 tc = std::cos(tilt);
    f32 ts = std::sin(tilt);
    Mat4 rotate_x;
    rotate_x.cols[1] = Vec4(0, tc, ts, 0);
    rotate_x.cols[2] = Vec4(0, -ts, tc, 0);

    Mat4 view = Mat4::LookAtRH(Vec3(0, 0, -3.0f), Vec3(0, 0, 0), Vec3(0, 1, 0));
    Mat4 proj = Mat4::PerspectiveRH(Radians(60.0f), aspect, 0.1f, 100.0f);
    return proj * view * model * rotate_x * rotate_y;
}

// ---------------------------------------------------------------------
// D3D12 cube pipeline
// ---------------------------------------------------------------------

struct D3D12CubeResources {
    gfx::ComPtr<ID3D12RootSignature> root_signature;
    gfx::ComPtr<ID3D12PipelineState> pso;
    gfx::ComPtr<ID3D12DescriptorHeap> dsv_heap;
    std::vector<gfx::ComPtr<ID3D12Resource>> depth_buffers; // one per swapchain image
    std::unique_ptr<gfx::Buffer> vertex_buffer;
    std::unique_ptr<gfx::Buffer> index_buffer;
    u32 dsv_descriptor_size = 0;
    u32 depth_width = 0;
    u32 depth_height = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle(u32 index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * dsv_descriptor_size;
        return handle;
    }

    void EnsureDepthBuffers(ID3D12Device* device, u32 width, u32 height, u32 buffer_count) {
        if (!depth_buffers.empty() && width == depth_width && height == depth_height &&
            depth_buffers.size() == buffer_count) {
            return;
        }

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear_value{};
        clear_value.Format = DXGI_FORMAT_D32_FLOAT;
        clear_value.DepthStencil.Depth = 1.0f;

        depth_buffers.clear();
        depth_buffers.resize(buffer_count);
        for (u32 i = 0; i < buffer_count; ++i) {
            AETHER_D3D_CHECK(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
                                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
                                                              IID_PPV_ARGS(&depth_buffers[i])));
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = DsvHandle(i);
            device->CreateDepthStencilView(depth_buffers[i].Get(), nullptr, dsv);
        }
        depth_width = width;
        depth_height = height;
    }
};

D3D12CubeResources CreateD3D12CubeResources(ID3D12Device* device, gfx::Device& native_device, DXGI_FORMAT rtv_format,
                                             u32 buffer_count) {
    using gfx::ShaderBytecode;

    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, /*Num32BitValues=*/sizeof(CubePushConstants) / 4};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = &param;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    gfx::ComPtr<ID3DBlob> signature;
    gfx::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("RHIDemo", "Cube root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }

    D3D12CubeResources result;
    AETHER_D3D_CHECK(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                  IID_PPV_ARGS(&result.root_signature)));

    ShaderBytecode vs = gfx::CompileHLSL(kCubeShaderSource, "VSMain", "vs_5_0", "cube_vs");
    ShaderBytecode ps = gfx::CompileHLSL(kCubeShaderSource, "PSMain", "ps_5_0", "cube_ps");

    D3D12_INPUT_ELEMENT_DESC input_elements[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = result.root_signature.Get();
    pso_desc.InputLayout = {input_elements, 2};
    pso_desc.VS = {vs.Data(), vs.Size()};
    pso_desc.PS = {ps.Data(), ps.Size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = rtv_format;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso_desc.SampleDesc.Count = 1;

    AETHER_D3D_CHECK(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&result.pso)));

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.NumDescriptors = buffer_count;
    AETHER_D3D_CHECK(device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&result.dsv_heap)));
    result.dsv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    result.vertex_buffer = std::make_unique<gfx::Buffer>(native_device, sizeof(kCubeVertices), gfx::BufferKind::Upload);
    result.vertex_buffer->Update(kCubeVertices, sizeof(kCubeVertices));
    result.index_buffer = std::make_unique<gfx::Buffer>(native_device, sizeof(kCubeIndices), gfx::BufferKind::Upload);
    result.index_buffer->Update(kCubeIndices, sizeof(kCubeIndices));

    return result;
}

void RecordD3D12CubeFrame(ID3D12Device* device, ICommandList& cmd, gfx::SwapChain& native_swap,
                           D3D12CubeResources& res, u32 slot, bool used_before, f32 time) {
    auto* native_cmd = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12Resource* backbuffer = native_swap.CurrentBackBuffer();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = native_swap.CurrentBackBufferRTV();

    res.EnsureDepthBuffers(device, native_swap.Width(), native_swap.Height(), native_swap.BufferCount());

    D3D12_RESOURCE_STATES before_state = used_before ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER to_rt =
        gfx::TransitionBarrier(backbuffer, before_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    native_cmd->ResourceBarrier(1, &to_rt);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = res.DsvHandle(slot);
    const f32 clear[4] = {0.02f, 0.02f, 0.05f, 1.0f};
    native_cmd->ClearRenderTargetView(rtv, clear, 0, nullptr);
    native_cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    native_cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT viewport{0.0f,
                             0.0f,
                             static_cast<f32>(native_swap.Width()),
                             static_cast<f32>(native_swap.Height()),
                             0.0f,
                             1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(native_swap.Width()), static_cast<LONG>(native_swap.Height())};
    native_cmd->RSSetViewports(1, &viewport);
    native_cmd->RSSetScissorRects(1, &scissor);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = res.vertex_buffer->GPUAddress();
    vbv.SizeInBytes = static_cast<UINT>(res.vertex_buffer->Size());
    vbv.StrideInBytes = sizeof(CubeVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = res.index_buffer->GPUAddress();
    ibv.SizeInBytes = static_cast<UINT>(res.index_buffer->Size());
    ibv.Format = DXGI_FORMAT_R16_UINT;

    native_cmd->SetGraphicsRootSignature(res.root_signature.Get());
    native_cmd->SetPipelineState(res.pso.Get());

    f32 aspect = static_cast<f32>(native_swap.Width()) / static_cast<f32>(native_swap.Height());
    CubePushConstants push{MakeCubeMVP(time, aspect), 1.0f};
    native_cmd->SetGraphicsRoot32BitConstants(0, sizeof(CubePushConstants) / 4, &push, 0);

    native_cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    native_cmd->IASetVertexBuffers(0, 1, &vbv);
    native_cmd->IASetIndexBuffer(&ibv);
    native_cmd->DrawIndexedInstanced(36, 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER to_present =
        gfx::TransitionBarrier(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    native_cmd->ResourceBarrier(1, &to_present);
}

// ---------------------------------------------------------------------
// Screenshot capture, for visually verifying that both backends actually
// render the same thing under Unified mode (not just that neither crashes).
// Takes the caller-tracked *last rendered* TextureHandle — same DXGI-flip-
// model reasoning as pbr_demo/gltf_demo's screenshot helpers: by the time
// the main loop has broken out and Present() was already called for that
// handle, its D3D12 resource state is PRESENT / its Vulkan layout is
// PRESENT_SRC_KHR, which both backends' capture code below assumes.
// ---------------------------------------------------------------------

void SaveD3D12Screenshot(IDevice& device, TextureHandle backbuffer, u32 width, u32 height, const std::string& path) {
    auto& d3d_device = static_cast<d3d12_backend::D3D12Device&>(device);
    ID3D12Resource* resource = d3d_device.GetTexture(backbuffer).resource;
    auto* native_device = static_cast<ID3D12Device*>(device.NativeHandle());

    D3D12_RESOURCE_DESC desc = resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    // The 7th output param (left null here) is the UNPADDED row size
    // (width * bytesPerPixel) — NOT the actual stride between rows in the
    // copied buffer, which is footprint.Footprint.RowPitch (256-byte
    // aligned). Using the unpadded value as the stride below was a real bug,
    // caught by actually running this at 800x600: 800*4=3200 isn't a
    // multiple of 256, so the real row pitch is padded up to 3328, and using
    // 3200 as the stride sheared the image diagonally, one row drifting
    // further per line. Never visible at pbr_demo's 1280x720 or gltf_demo's
    // 1024x768 — 1280*4=5120 and 1024*4=4096 both already happen to be
    // 256-aligned, hiding the exact same bug there by coincidence.
    native_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    gfx::Buffer readback(d3d_device.Native(), total_bytes, gfx::BufferKind::Readback);

    std::unique_ptr<ICommandList> cmd = device.CreateCommandList();
    cmd->Reset();
    auto* native_cmd = static_cast<ID3D12GraphicsCommandList*>(cmd->NativeHandle());

    D3D12_RESOURCE_BARRIER to_src =
        gfx::TransitionBarrier(resource, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    native_cmd->ResourceBarrier(1, &to_src);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Handle();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    native_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER to_present =
        gfx::TransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    native_cmd->ResourceBarrier(1, &to_present);

    cmd->Close();
    device.WaitForFence(device.Submit(*cmd));

    std::vector<u8> raw(total_bytes);
    readback.Read(raw.data(), total_bytes);

    std::vector<u8> tight(static_cast<usize>(width) * height * 4);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(&tight[static_cast<usize>(y) * width * 4],
                    &raw[static_cast<usize>(y) * footprint.Footprint.RowPitch], static_cast<usize>(width) * 4);
    }

    // D3D12's swapchain format is R8G8B8A8_UNORM (gfx::SwapChain's default) — already RGBA, no swizzle needed.
    stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, tight.data(),
                   static_cast<int>(width) * 4);
    AETHER_LOG_INFO("RHIDemo", "Wrote D3D12 screenshot to \"%s\"", path.c_str());
}

// (SaveVulkanScreenshot and the SaveScreenshot dispatcher are defined below,
// after FindMemoryType — see that comment.)

// ---------------------------------------------------------------------
// Vulkan triangle pipeline
// ---------------------------------------------------------------------

#if defined(AETHER_HAS_VULKAN)

namespace vulkan_backend = aether::gfx::rhi::vulkan_backend;

void VkDemoCheck(VkResult result, const char* expr) {
    if (result != VK_SUCCESS) {
        AETHER_LOG_FATAL("RHIDemo", "%s failed (VkResult=%d)", expr, static_cast<int>(result));
        throw std::runtime_error(expr);
    }
}
#define AETHER_RHI_DEMO_VK_CHECK(expr) ::VkDemoCheck((expr), #expr)

VkShaderModule CreateShaderModule(VkDevice device, const gfx::ShaderBytecode& bytecode) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytecode.Size();
    info.pCode = reinterpret_cast<const u32*>(bytecode.Data());
    VkShaderModule module = VK_NULL_HANDLE;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

// Owns a render pass + pipeline (both resize-independent: viewport/scissor
// are dynamic state) plus one framebuffer per swapchain image (NOT
// resize-independent — rebuilt via RebuildFramebuffers whenever the
// swapchain's images are recreated).
struct VulkanTriangleResources {
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    // The actual VkImageView handles each framebuffer[i] was built from —
    // NOT just width/height/count. VulkanSwapChain::Resize() unconditionally
    // destroys and recreates every VkImage/VkImageView, even when the
    // surface capabilities clamp the requested extent back to the same
    // width/height (e.g. this demo's programmatic resize test, which resizes
    // the swapchain without resizing the actual HWND — the surface's
    // min/maxImageExtent then just forces the old size straight back). A
    // dimension-only cache would keep a framebuffer bound to views that were
    // already destroyed underneath it — this is exactly that bug, found by
    // actually running the resize test rather than trusting the dimension
    // check to be sufficient.
    std::vector<VkImageView> framebuffer_views;

    VulkanTriangleResources(const VulkanTriangleResources&) = delete;
    VulkanTriangleResources& operator=(const VulkanTriangleResources&) = delete;
    VulkanTriangleResources() = default;

    ~VulkanTriangleResources() {
        DestroyFramebuffers();
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, render_pass, nullptr);
        }
    }

    void DestroyFramebuffers() {
        for (VkFramebuffer fb : framebuffers) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
        framebuffers.clear();
        framebuffer_views.clear();
    }

    // Called every frame; cheap no-op unless the swapchain actually
    // recreated its images (detected via a width/height change, since this
    // demo only resizes explicitly — see main()'s comment on test_resize).
    void EnsureFramebuffers(vulkan_backend::VulkanSwapChain& native_swap, u32 buffer_count) {
        u32 width = native_swap.Width();
        u32 height = native_swap.Height();

        bool needs_rebuild = framebuffers.empty() || framebuffer_views.size() != buffer_count;
        if (!needs_rebuild) {
            for (u32 i = 0; i < buffer_count; ++i) {
                if (framebuffer_views[i] != native_swap.ImageView(i)) {
                    needs_rebuild = true;
                    break;
                }
            }
        }
        if (!needs_rebuild) {
            return;
        }

        DestroyFramebuffers();
        framebuffers.resize(buffer_count);
        framebuffer_views.resize(buffer_count);
        for (u32 i = 0; i < buffer_count; ++i) {
            VkImageView view = native_swap.ImageView(i);
            VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb_info.renderPass = render_pass;
            fb_info.attachmentCount = 1;
            fb_info.pAttachments = &view;
            fb_info.width = width;
            fb_info.height = height;
            fb_info.layers = 1;
            AETHER_RHI_DEMO_VK_CHECK(vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffers[i]));
            framebuffer_views[i] = view;
        }
    }
};

std::unique_ptr<VulkanTriangleResources> CreateVulkanTriangleResources(VkDevice device, VkFormat color_format) {
    auto res = std::make_unique<VulkanTriangleResources>();
    res->device = device;

    // Every frame clears via loadOp=CLEAR and discards prior contents, so
    // the attachment's initialLayout can always be UNDEFINED regardless of
    // what layout the image was actually left in last frame — no per-frame
    // layout bookkeeping needed, unlike the clear-to-color path.
    VkAttachmentDescription attachment{};
    attachment.format = color_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    // Synchronizes subpass 0 with the swapchain's image-available semaphore,
    // which VulkanDevice::Submit waits on at COLOR_ATTACHMENT_OUTPUT — the
    // same stage this dependency gates, so the render pass never starts
    // writing before the image is actually available.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateRenderPass(device, &rp_info, nullptr, &res->render_pass));

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    AETHER_RHI_DEMO_VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &res->pipeline_layout));

    gfx::ShaderBytecode vs_spirv = gfx::CompileHLSLToSPIRV(kTriangleShaderSource, "VSMain", "vs_6_0", "triangle_vs_spirv");
    gfx::ShaderBytecode ps_spirv = gfx::CompileHLSLToSPIRV(kTriangleShaderSource, "PSMain", "ps_6_0", "triangle_ps_spirv");
    VkShaderModule vs_module = CreateShaderModule(device, vs_spirv);
    VkShaderModule ps_module = CreateShaderModule(device, ps_spirv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "VSMain";
    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_module;
    stages[1].pName = "PSMain";

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = res->pipeline_layout;
    pipeline_info.renderPass = res->render_pass;
    pipeline_info.subpass = 0;

    VkResult pipeline_result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &res->pipeline);

    vkDestroyShaderModule(device, vs_module, nullptr);
    vkDestroyShaderModule(device, ps_module, nullptr);

    AETHER_RHI_DEMO_VK_CHECK(pipeline_result);

    return res;
}

void RecordVulkanTriangleFrame(ICommandList& cmd, vulkan_backend::VulkanSwapChain& native_swap,
                                VulkanTriangleResources& res, f32 time) {
    auto native_cmd = static_cast<VkCommandBuffer>(cmd.NativeHandle());
    u32 image_index = native_swap.CurrentImageIndex();

    VkClearValue clear_value{};
    clear_value.color.float32[0] = 0.02f;
    clear_value.color.float32[1] = 0.02f;
    clear_value.color.float32[2] = 0.05f;
    clear_value.color.float32[3] = 1.0f;

    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = res.render_pass;
    rp_begin.framebuffer = res.framebuffers[image_index];
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {native_swap.Width(), native_swap.Height()};
    rp_begin.clearValueCount = 1;
    rp_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(native_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Negative-height viewport (y = height, height = -height): the standard
    // trick to make Vulkan's NDC +Y point up like D3D12's, entirely on the
    // host side — no shader or push-constant change needed. Requires
    // VK_KHR_maintenance1, core since Vulkan 1.1 (we require 1.2 already).
    VkViewport viewport{0.0f, static_cast<f32>(native_swap.Height()), static_cast<f32>(native_swap.Width()),
                         -static_cast<f32>(native_swap.Height()), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {native_swap.Width(), native_swap.Height()}};
    vkCmdSetViewport(native_cmd, 0, 1, &viewport);
    vkCmdSetScissor(native_cmd, 0, 1, &scissor);

    vkCmdBindPipeline(native_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res.pipeline);
    vkCmdPushConstants(native_cmd, res.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32), &time);
    vkCmdDraw(native_cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(native_cmd);
}

// ---------------------------------------------------------------------
// Vulkan cube pipeline
// ---------------------------------------------------------------------

u32 FindMemoryType(VkPhysicalDevice physical_device, u32 type_bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (u32 i = 0; i < mem_props.memoryTypeCount; ++i) {
        bool type_ok = (type_bits & (1u << i)) != 0;
        bool props_ok = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_ok && props_ok) {
            return i;
        }
    }
    AETHER_LOG_FATAL("RHIDemo", "No suitable Vulkan memory type found");
    throw std::runtime_error("no suitable Vulkan memory type");
}

// See SaveD3D12Screenshot's comment above (near the D3D12 cube pipeline)
// for what this is for and why the caller must pass the last-*rendered*
// TextureHandle, not a freshly-queried "current" one.
void SaveVulkanScreenshot(IDevice& device, TextureHandle backbuffer, u32 width, u32 height, const std::string& path) {
    auto& vk_device = static_cast<vulkan_backend::VulkanDevice&>(device);
    VkImage image = vk_device.GetTexture(backbuffer);
    VkDevice native_device = vk_device.Handle();

    VkDeviceSize buffer_size = static_cast<VkDeviceSize>(width) * height * 4;
    VkBufferCreateInfo buf_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_info.size = buffer_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buf_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateBuffer(native_device, &buf_info, nullptr, &staging_buffer));

    VkMemoryRequirements mem_reqs{};
    vkGetBufferMemoryRequirements(native_device, staging_buffer, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = FindMemoryType(vk_device.PhysicalDevice(), mem_reqs.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    AETHER_RHI_DEMO_VK_CHECK(vkAllocateMemory(native_device, &alloc_info, nullptr, &staging_memory));
    AETHER_RHI_DEMO_VK_CHECK(vkBindBufferMemory(native_device, staging_buffer, staging_memory, 0));

    std::unique_ptr<ICommandList> cmd = device.CreateCommandList();
    cmd->Reset();
    auto native_cmd = static_cast<VkCommandBuffer>(cmd->NativeHandle());

    VkImageMemoryBarrier to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(native_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(native_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer, 1, &region);
    // No transition back to PRESENT_SRC_KHR needed — this image is never presented again.

    cmd->Close();
    device.WaitForFence(device.Submit(*cmd));

    void* mapped = nullptr;
    AETHER_RHI_DEMO_VK_CHECK(vkMapMemory(native_device, staging_memory, 0, buffer_size, 0, &mapped));

    // Vulkan's swapchain format is B8G8R8A8_UNORM (VulkanSwapChain's
    // default) — swap R/B before writing an RGBA PNG.
    std::vector<u8> rgba(static_cast<usize>(buffer_size));
    const u8* bgra = static_cast<const u8*>(mapped);
    for (usize i = 0; i < static_cast<usize>(width) * height; ++i) {
        rgba[i * 4 + 0] = bgra[i * 4 + 2];
        rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rgba[i * 4 + 2] = bgra[i * 4 + 0];
        rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }
    vkUnmapMemory(native_device, staging_memory);

    stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                   static_cast<int>(width) * 4);
    AETHER_LOG_INFO("RHIDemo", "Wrote Vulkan screenshot to \"%s\"", path.c_str());

    vkDestroyBuffer(native_device, staging_buffer, nullptr);
    vkFreeMemory(native_device, staging_memory, nullptr);
}
#endif // defined(AETHER_HAS_VULKAN)

void SaveScreenshot(IDevice& device, TextureHandle backbuffer, u32 width, u32 height, const std::string& path) {
    if (device.GetBackend() == Backend::D3D12) {
        SaveD3D12Screenshot(device, backbuffer, width, height, path);
    }
#if defined(AETHER_HAS_VULKAN)
    else {
        SaveVulkanScreenshot(device, backbuffer, width, height, path);
    }
#endif
}

#if defined(AETHER_HAS_VULKAN)
// Host-visible + host-coherent: simplest correct approach at demo scale (a
// handful of KB), no staging buffer / transfer queue needed. A real asset
// pipeline would use a device-local buffer with a staging upload instead.
struct VulkanBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;

    VulkanBuffer() = default;
    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    ~VulkanBuffer() {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }
};

std::unique_ptr<VulkanBuffer> CreateVulkanHostVisibleBuffer(VkDevice device, VkPhysicalDevice physical_device,
                                                             VkDeviceSize size, VkBufferUsageFlags usage,
                                                             const void* data) {
    auto result = std::make_unique<VulkanBuffer>();
    result->device = device;

    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateBuffer(device, &buffer_info, nullptr, &result->buffer));

    VkMemoryRequirements mem_reqs{};
    vkGetBufferMemoryRequirements(device, result->buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex = FindMemoryType(
        physical_device, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    AETHER_RHI_DEMO_VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &result->memory));
    AETHER_RHI_DEMO_VK_CHECK(vkBindBufferMemory(device, result->buffer, result->memory, 0));

    void* mapped = nullptr;
    AETHER_RHI_DEMO_VK_CHECK(vkMapMemory(device, result->memory, 0, size, 0, &mapped));
    std::memcpy(mapped, data, static_cast<usize>(size));
    vkUnmapMemory(device, result->memory);

    return result;
}

struct VulkanDepthBuffer {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    VulkanDepthBuffer() = default;
    VulkanDepthBuffer(const VulkanDepthBuffer&) = delete;
    VulkanDepthBuffer& operator=(const VulkanDepthBuffer&) = delete;

    void Destroy() {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
    }

    ~VulkanDepthBuffer() { Destroy(); }
};

constexpr VkFormat kCubeDepthFormat = VK_FORMAT_D32_SFLOAT;

void CreateVulkanDepthBuffer(VulkanDepthBuffer& depth, VkDevice device, VkPhysicalDevice physical_device, u32 width,
                              u32 height) {
    depth.Destroy();
    depth.device = device;

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kCubeDepthFormat;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateImage(device, &image_info, nullptr, &depth.image));

    VkMemoryRequirements mem_reqs{};
    vkGetImageMemoryRequirements(device, depth.image, &mem_reqs);
    VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_reqs.size;
    alloc_info.memoryTypeIndex =
        FindMemoryType(physical_device, mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    AETHER_RHI_DEMO_VK_CHECK(vkAllocateMemory(device, &alloc_info, nullptr, &depth.memory));
    AETHER_RHI_DEMO_VK_CHECK(vkBindImageMemory(device, depth.image, depth.memory, 0));

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = depth.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kCubeDepthFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    AETHER_RHI_DEMO_VK_CHECK(vkCreateImageView(device, &view_info, nullptr, &depth.view));
}

struct VulkanCubeResources {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> framebuffer_color_views;
    std::vector<std::unique_ptr<VulkanDepthBuffer>> depth_buffers; // one per swapchain image
    std::unique_ptr<VulkanBuffer> vertex_buffer;
    std::unique_ptr<VulkanBuffer> index_buffer;
    u32 framebuffer_width = 0;
    u32 framebuffer_height = 0;

    VulkanCubeResources(const VulkanCubeResources&) = delete;
    VulkanCubeResources& operator=(const VulkanCubeResources&) = delete;
    VulkanCubeResources() = default;

    ~VulkanCubeResources() {
        DestroyFramebuffers();
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, render_pass, nullptr);
        }
    }

    void DestroyFramebuffers() {
        for (VkFramebuffer fb : framebuffers) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
        framebuffers.clear();
        framebuffer_color_views.clear();
        depth_buffers.clear();
    }

    // See EnsureFramebuffers in VulkanTriangleResources for why this keys
    // off the actual VkImageView handles, not just width/height/count.
    void EnsureFramebuffers(vulkan_backend::VulkanSwapChain& native_swap, u32 buffer_count) {
        u32 width = native_swap.Width();
        u32 height = native_swap.Height();

        bool needs_rebuild = framebuffers.empty() || framebuffer_color_views.size() != buffer_count;
        if (!needs_rebuild) {
            for (u32 i = 0; i < buffer_count; ++i) {
                if (framebuffer_color_views[i] != native_swap.ImageView(i)) {
                    needs_rebuild = true;
                    break;
                }
            }
        }
        if (!needs_rebuild) {
            return;
        }

        DestroyFramebuffers();
        framebuffers.resize(buffer_count);
        framebuffer_color_views.resize(buffer_count);
        depth_buffers.resize(buffer_count);
        for (u32 i = 0; i < buffer_count; ++i) {
            depth_buffers[i] = std::make_unique<VulkanDepthBuffer>();
            CreateVulkanDepthBuffer(*depth_buffers[i], device, physical_device, width, height);

            VkImageView color_view = native_swap.ImageView(i);
            VkImageView attachments[2] = {color_view, depth_buffers[i]->view};

            VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb_info.renderPass = render_pass;
            fb_info.attachmentCount = 2;
            fb_info.pAttachments = attachments;
            fb_info.width = width;
            fb_info.height = height;
            fb_info.layers = 1;
            AETHER_RHI_DEMO_VK_CHECK(vkCreateFramebuffer(device, &fb_info, nullptr, &framebuffers[i]));
            framebuffer_color_views[i] = color_view;
        }
        framebuffer_width = width;
        framebuffer_height = height;
    }
};

std::unique_ptr<VulkanCubeResources> CreateVulkanCubeResources(VkDevice device, VkPhysicalDevice physical_device,
                                                                 VkFormat color_format) {
    auto res = std::make_unique<VulkanCubeResources>();
    res->device = device;
    res->physical_device = physical_device;

    VkAttachmentDescription color_attachment{};
    color_attachment.format = color_format;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_attachment{};
    depth_attachment.format = kCubeDepthFormat;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription attachments[2] = {color_attachment, depth_attachment};

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp_info.attachmentCount = 2;
    rp_info.pAttachments = attachments;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;
    AETHER_RHI_DEMO_VK_CHECK(vkCreateRenderPass(device, &rp_info, nullptr, &res->render_pass));

    VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(CubePushConstants)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    AETHER_RHI_DEMO_VK_CHECK(vkCreatePipelineLayout(device, &layout_info, nullptr, &res->pipeline_layout));

    gfx::ShaderBytecode vs_spirv = gfx::CompileHLSLToSPIRV(kCubeShaderSource, "VSMain", "vs_6_0", "cube_vs_spirv");
    gfx::ShaderBytecode ps_spirv = gfx::CompileHLSLToSPIRV(kCubeShaderSource, "PSMain", "ps_6_0", "cube_ps_spirv");
    VkShaderModule vs_module = CreateShaderModule(device, vs_spirv);
    VkShaderModule ps_module = CreateShaderModule(device, ps_spirv);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_module;
    stages[0].pName = "VSMain";
    stages[1] = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_module;
    stages[1].pName = "PSMain";

    VkVertexInputBindingDescription binding{0, sizeof(CubeVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend_state{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend_attachment;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blend_state;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = res->pipeline_layout;
    pipeline_info.renderPass = res->render_pass;
    pipeline_info.subpass = 0;

    VkResult pipeline_result =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &res->pipeline);

    vkDestroyShaderModule(device, vs_module, nullptr);
    vkDestroyShaderModule(device, ps_module, nullptr);

    AETHER_RHI_DEMO_VK_CHECK(pipeline_result);

    res->vertex_buffer = CreateVulkanHostVisibleBuffer(device, physical_device, sizeof(kCubeVertices),
                                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, kCubeVertices);
    res->index_buffer = CreateVulkanHostVisibleBuffer(device, physical_device, sizeof(kCubeIndices),
                                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT, kCubeIndices);

    return res;
}

void RecordVulkanCubeFrame(ICommandList& cmd, vulkan_backend::VulkanSwapChain& native_swap, VulkanCubeResources& res,
                            f32 time) {
    auto native_cmd = static_cast<VkCommandBuffer>(cmd.NativeHandle());
    u32 image_index = native_swap.CurrentImageIndex();

    VkClearValue clear_values[2]{};
    clear_values[0].color = {{0.02f, 0.02f, 0.05f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp_begin.renderPass = res.render_pass;
    rp_begin.framebuffer = res.framebuffers[image_index];
    rp_begin.renderArea.offset = {0, 0};
    rp_begin.renderArea.extent = {native_swap.Width(), native_swap.Height()};
    rp_begin.clearValueCount = 2;
    rp_begin.pClearValues = clear_values;

    vkCmdBeginRenderPass(native_cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{
        0.0f, 0.0f, static_cast<f32>(native_swap.Width()), static_cast<f32>(native_swap.Height()), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {native_swap.Width(), native_swap.Height()}};
    vkCmdSetViewport(native_cmd, 0, 1, &viewport);
    vkCmdSetScissor(native_cmd, 0, 1, &scissor);

    vkCmdBindPipeline(native_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, res.pipeline);

    f32 aspect = static_cast<f32>(native_swap.Width()) / static_cast<f32>(native_swap.Height());
    CubePushConstants push{MakeCubeMVP(time, aspect), -1.0f};
    vkCmdPushConstants(native_cmd, res.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(CubePushConstants),
                        &push);

    VkBuffer vertex_buffers[] = {res.vertex_buffer->buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(native_cmd, 0, 1, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(native_cmd, res.index_buffer->buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(native_cmd, 36, 1, 0, 0, 0);

    vkCmdEndRenderPass(native_cmd);
}

#endif // defined(AETHER_HAS_VULKAN)

} // namespace

int main() {
    Backend backend = Backend::D3D12;
    if (const char* env = std::getenv("AETHER_RHI_BACKEND")) {
        if (std::strcmp(env, "vulkan") == 0) {
            backend = Backend::Vulkan;
        }
    }

    i32 max_frames = -1;
    if (const char* env = std::getenv("AETHER_RHI_DEMO_MAX_FRAMES")) {
        max_frames = std::atoi(env);
    }

    enum class DemoMode { Clear, Triangle, Cube, Unified };
    DemoMode mode = DemoMode::Clear;
    if (std::getenv("AETHER_RHI_DEMO_DRAW_UNIFIED") != nullptr) {
        // Highest priority: the "Unified Cross-API Renderer" follow-up's
        // proof — the exact same abstract-API code path (no `if (backend ==
        // ...)` branch anywhere in this mode's setup or per-frame recording)
        // renders on both backends, unlike Triangle/Cube mode below which
        // each still pick between two backend-specific implementations.
        mode = DemoMode::Unified;
    } else if (std::getenv("AETHER_RHI_DEMO_DRAW_CUBE") != nullptr) {
        mode = DemoMode::Cube;
    } else if (std::getenv("AETHER_RHI_DEMO_DRAW_TRIANGLE") != nullptr) {
        mode = DemoMode::Triangle;
    }

    try {
        if (!IsBackendAvailable(backend)) {
            AETHER_LOG_FATAL("RHIDemo", "Requested backend is not available in this build");
            return 1;
        }

        WindowDesc window_desc;
        window_desc.title = (backend == Backend::Vulkan) ? "Aether RHI Demo - Vulkan" : "Aether RHI Demo - D3D12";
        window_desc.width = 800;
        window_desc.height = 600;
        Window window(window_desc);

        std::unique_ptr<IDevice> device = CreateDevice(backend, /*enable_debug_layer=*/true);
        if (!device) {
            AETHER_LOG_FATAL("RHIDemo", "Failed to create device");
            return 1;
        }

        std::unique_ptr<ISwapChain> swap_chain =
            device->CreateSwapChain(window.NativeHandle(), window.Width(), window.Height());

        window.on_resize = [&](u32 w, u32 h) { swap_chain->Resize(w, h); };

        // Mode-specific pipeline objects, created once up front for whichever
        // backend is active. Declared here (rather than deeper in main) so
        // they're destroyed, in reverse order, before `device`/`swap_chain`
        // go out of scope.
        D3D12TriangleResources d3d12_triangle;
        D3D12CubeResources d3d12_cube;
#if defined(AETHER_HAS_VULKAN)
        std::unique_ptr<VulkanTriangleResources> vulkan_triangle;
        std::unique_ptr<VulkanCubeResources> vulkan_cube;
#endif

        if (mode == DemoMode::Triangle) {
            if (backend == Backend::D3D12) {
                auto* native_swap = static_cast<gfx::SwapChain*>(swap_chain->NativeHandle());
                d3d12_triangle = CreateD3D12TriangleResources(static_cast<ID3D12Device*>(device->NativeHandle()),
                                                               native_swap->Format());
            }
#if defined(AETHER_HAS_VULKAN)
            else {
                auto* native_swap = static_cast<vulkan_backend::VulkanSwapChain*>(swap_chain->NativeHandle());
                vulkan_triangle =
                    CreateVulkanTriangleResources(static_cast<VkDevice>(device->NativeHandle()), native_swap->Format());
                vulkan_triangle->EnsureFramebuffers(*native_swap, swap_chain->BufferCount());
            }
#endif
        } else if (mode == DemoMode::Cube) {
            if (backend == Backend::D3D12) {
                auto* native_swap = static_cast<gfx::SwapChain*>(swap_chain->NativeHandle());
                auto* d3d12_device = static_cast<d3d12_backend::D3D12Device*>(device.get());
                d3d12_cube = CreateD3D12CubeResources(static_cast<ID3D12Device*>(device->NativeHandle()),
                                                       d3d12_device->Native(), native_swap->Format(),
                                                       swap_chain->BufferCount());
            }
#if defined(AETHER_HAS_VULKAN)
            else {
                auto* native_swap = static_cast<vulkan_backend::VulkanSwapChain*>(swap_chain->NativeHandle());
                auto* vk_device = static_cast<vulkan_backend::VulkanDevice*>(device.get());
                vulkan_cube = CreateVulkanCubeResources(static_cast<VkDevice>(device->NativeHandle()),
                                                         vk_device->PhysicalDevice(), native_swap->Format());
                vulkan_cube->EnsureFramebuffers(*native_swap, swap_chain->BufferCount());
            }
#endif
        }

        // Unified mode's pipeline: ONE CreatePipeline call, no backend
        // branch — the D3D12/Vulkan-specific root signature/PSO vs. pipeline
        // layout/render pass/pipeline creation all happens inside
        // IDevice::CreatePipeline, hidden from this call site. Same for the
        // vertex/index buffers and texture below: CreateVertexBuffer/
        // CreateIndexBuffer/CreateTexture are the "vertex buffers + textures"
        // follow-up's actual proof — no backend branch here either.
        PipelineHandle unified_pipeline;
        BufferHandle unified_vertex_buffer;
        BufferHandle unified_index_buffer;
        SampledTextureHandle unified_texture;

        // The "depth buffer + blend states" follow-up's proof: two more
        // pipelines sharing the same HLSL source and vertex/index data
        // shape, differing only in PipelineDesc::depth_test/enable_blending.
        PipelineHandle unified_depth_pipeline;   // depth_test=true, opaque
        PipelineHandle unified_blend_pipeline;   // depth_test=true, alpha-blended
        BufferHandle unified_offset_vertex_buffer;
        BufferHandle unified_offset_index_buffer;
        SampledTextureHandle unified_red_texture;
        SampledTextureHandle unified_blue_texture;
        SampledTextureHandle unified_green_texture;

        if (mode == DemoMode::Unified) {
            PipelineDesc pipeline_desc;
            pipeline_desc.hlsl_source = kUnifiedTexturedQuadShaderSource;
            pipeline_desc.push_constant_size_bytes = sizeof(UnifiedPushConstants);
            pipeline_desc.use_vertex_buffer = true;
            pipeline_desc.enable_bindless_textures = true;
            unified_pipeline = device->CreatePipeline(pipeline_desc, *swap_chain);

            PipelineDesc depth_pipeline_desc = pipeline_desc;
            depth_pipeline_desc.depth_test = true;
            unified_depth_pipeline = device->CreatePipeline(depth_pipeline_desc, *swap_chain);

            PipelineDesc blend_pipeline_desc = depth_pipeline_desc;
            blend_pipeline_desc.enable_blending = true;
            unified_blend_pipeline = device->CreatePipeline(blend_pipeline_desc, *swap_chain);

            UnifiedVertex quad_vertices[4] = {
                {{-0.6f, -0.6f, 0.0f}, {0.0f, 1.0f}},
                {{0.6f, -0.6f, 0.0f}, {1.0f, 1.0f}},
                {{0.6f, 0.6f, 0.0f}, {1.0f, 0.0f}},
                {{-0.6f, 0.6f, 0.0f}, {0.0f, 0.0f}},
            };
            u32 quad_indices[6] = {0, 1, 2, 0, 2, 3};
            unified_vertex_buffer = device->CreateVertexBuffer(quad_vertices, sizeof(quad_vertices));
            unified_index_buffer = device->CreateIndexBuffer(quad_indices, sizeof(quad_indices), IndexFormat::UInt32);

            std::vector<u8> checker_pixels = GenerateCheckerboardPixels(/*size=*/64, /*cell_size=*/8);
            unified_texture = device->CreateTexture(64, 64, checker_pixels.data());

            // Offset to the right of the main quad, overlapping its right
            // half — this is what makes the depth-test-vs-blend distinction
            // visible in one screenshot: the red/blue pair (drawn at the
            // main quad's position) tests draw-order-independent occlusion,
            // and this offset quad (drawn with the blend pipeline, on top of
            // whichever of red/blue wins) shows translucency where it
            // overlaps them and pure green where it doesn't.
            UnifiedVertex offset_quad_vertices[4] = {
                {{-0.1f, -0.6f, 0.0f}, {0.0f, 1.0f}},
                {{0.9f, -0.6f, 0.0f}, {1.0f, 1.0f}},
                {{0.9f, 0.6f, 0.0f}, {1.0f, 0.0f}},
                {{-0.1f, 0.6f, 0.0f}, {0.0f, 0.0f}},
            };
            unified_offset_vertex_buffer =
                device->CreateVertexBuffer(offset_quad_vertices, sizeof(offset_quad_vertices));
            unified_offset_index_buffer =
                device->CreateIndexBuffer(quad_indices, sizeof(quad_indices), IndexFormat::UInt32);

            unified_red_texture = device->CreateTexture(4, 4, GenerateSolidColorPixels(4, 220, 40, 40, 255).data());
            unified_blue_texture = device->CreateTexture(4, 4, GenerateSolidColorPixels(4, 40, 90, 220, 255).data());
            unified_green_texture = device->CreateTexture(4, 4, GenerateSolidColorPixels(4, 60, 200, 90, 255).data());
        }

        std::vector<std::unique_ptr<ICommandList>> command_lists;
        std::vector<u64> frame_fences(swap_chain->BufferCount(), 0);
        // Vulkan swap chain images start life in an undefined layout;
        // D3D12's convention (matching the rest of this engine) is to treat
        // a fresh backbuffer as already PRESENT. Tracking "have we used this
        // slot yet" lets the same code transition correctly on both
        // backends without the RHI needing a full RenderGraph-style
        // automatic-state-tracking layer of its own.
        std::vector<bool> used_before(swap_chain->BufferCount(), false);
        for (u32 i = 0; i < swap_chain->BufferCount(); ++i) {
            command_lists.push_back(device->CreateCommandList());
        }

        const char* mode_name = mode == DemoMode::Unified   ? "unified"
                                 : mode == DemoMode::Cube    ? "cube"
                                 : mode == DemoMode::Triangle ? "triangle"
                                                              : "clear";
        AETHER_LOG_INFO("RHIDemo", "Entering main loop (backend=%s, %u buffers, mode=%s)",
                         backend == Backend::Vulkan ? "Vulkan" : "D3D12", swap_chain->BufferCount(), mode_name);

        bool test_resize = std::getenv("AETHER_RHI_DEMO_TEST_RESIZE") != nullptr;

        i32 frame_index = 0;
        f32 t = 0.0f;
        TextureHandle last_rendered_backbuffer{};
        while (window.PumpMessages()) {
            if (window.IsMinimized()) {
                continue;
            }

            // Programmatic resize exercise, since triggering a real window
            // resize needs interactive input this demo can't script: proves
            // ISwapChain::Resize() (swapchain/image/semaphore recreation on
            // Vulkan, RTV recreation on D3D12) doesn't crash on either
            // backend without needing a human to drag the window edge.
            if (test_resize && frame_index == 10) {
                AETHER_LOG_INFO("RHIDemo", "Testing programmatic resize to 400x300");
                for (u64 fence : frame_fences) {
                    device->WaitForFence(fence);
                }
                swap_chain->Resize(400, 300);
                used_before.assign(swap_chain->BufferCount(), false);
            }

            swap_chain->AcquireNextImage();
            TextureHandle back_buffer = swap_chain->CurrentBackBuffer();
            last_rendered_backbuffer = back_buffer;

            // There's no direct "which slot is this" query on ISwapChain
            // (backends differ on whether that's the acquired image index or
            // a separate frame-in-flight counter — see VulkanSwapChain's
            // comments) so we key bookkeeping off the TextureHandle's own
            // index, which is stable and unique per backbuffer on both
            // backends.
            u32 slot = back_buffer.index % static_cast<u32>(frame_fences.size());
            device->WaitForFence(frame_fences[slot]);

            ICommandList& cmd = *command_lists[slot];
            cmd.Reset();

            t += 0.01f;

            if (mode == DemoMode::Triangle) {
                if (backend == Backend::D3D12) {
                    auto* native_swap = static_cast<gfx::SwapChain*>(swap_chain->NativeHandle());
                    RecordD3D12TriangleFrame(cmd, *native_swap, d3d12_triangle, used_before[slot], t);
                }
#if defined(AETHER_HAS_VULKAN)
                else {
                    auto* native_swap = static_cast<vulkan_backend::VulkanSwapChain*>(swap_chain->NativeHandle());
                    vulkan_triangle->EnsureFramebuffers(*native_swap, swap_chain->BufferCount());
                    RecordVulkanTriangleFrame(cmd, *native_swap, *vulkan_triangle, t);
                }
#endif
                used_before[slot] = true;
            } else if (mode == DemoMode::Cube) {
                if (backend == Backend::D3D12) {
                    auto* native_swap = static_cast<gfx::SwapChain*>(swap_chain->NativeHandle());
                    RecordD3D12CubeFrame(static_cast<ID3D12Device*>(device->NativeHandle()), cmd, *native_swap,
                                         d3d12_cube, slot, used_before[slot], t);
                }
#if defined(AETHER_HAS_VULKAN)
                else {
                    auto* native_swap = static_cast<vulkan_backend::VulkanSwapChain*>(swap_chain->NativeHandle());
                    vulkan_cube->EnsureFramebuffers(*native_swap, swap_chain->BufferCount());
                    RecordVulkanCubeFrame(cmd, *native_swap, *vulkan_cube, t);
                }
#endif
                used_before[slot] = true;
            } else if (mode == DemoMode::Unified) {
                // The actual unification proof: this exact call sequence —
                // BeginRenderPass/BindPipeline/BindBindlessTextures/
                // BindVertexBuffer/BindIndexBuffer/SetPushConstants/
                // DrawIndexed/EndRenderPass, with NO D3D12- or Vulkan-
                // specific code anywhere in this file — renders the same
                // rotating textured quad, sampling a real GPU texture
                // through a real GPU vertex/index buffer pair, on both
                // backends. D3D12/Vulkan's opposite NDC +Y convention is
                // handled inside BeginRenderPass's Vulkan implementation (a
                // negative-height viewport, entirely host-side), not here —
                // so this call site really doesn't know or care which
                // backend it's talking to.
                cmd.BeginRenderPass(*swap_chain, {0.02f, 0.02f, 0.05f, 1.0f});
                cmd.BindPipeline(unified_pipeline);
                cmd.BindBindlessTextures();
                cmd.BindVertexBuffer(unified_vertex_buffer, sizeof(UnifiedVertex));
                cmd.BindIndexBuffer(unified_index_buffer, IndexFormat::UInt32);
                UnifiedPushConstants push_constants{t, unified_texture.index, 0.5f, 1.0f};
                cmd.SetPushConstants(&push_constants, sizeof(push_constants));
                cmd.DrawIndexed(6);

                // Depth-test proof: draw the FARTHER (red, depth=0.8) quad
                // AFTER the NEARER (blue, depth=0.2) one, both at the exact
                // same screen position. If depth_test is actually working,
                // blue must still be the one visible — a broken/absent
                // depth test would let red (drawn later) overwrite it
                // regardless of which is actually nearer. Not rotating
                // (time=0) so this trio reads as a clean, static composition
                // distinct from the spinning checkerboard quad above.
                cmd.BindPipeline(unified_depth_pipeline);
                cmd.BindBindlessTextures();
                cmd.BindVertexBuffer(unified_vertex_buffer, sizeof(UnifiedVertex));
                cmd.BindIndexBuffer(unified_index_buffer, IndexFormat::UInt32);
                UnifiedPushConstants blue_push{0.0f, unified_blue_texture.index, 0.2f, 1.0f};
                cmd.SetPushConstants(&blue_push, sizeof(blue_push));
                cmd.DrawIndexed(6);
                UnifiedPushConstants red_push{0.0f, unified_red_texture.index, 0.8f, 1.0f};
                cmd.SetPushConstants(&red_push, sizeof(red_push));
                cmd.DrawIndexed(6);

                // Blend-state proof: a translucent green quad, offset to
                // overlap the right half of the red/blue pair, closer than
                // both (depth=0.1) so it passes the depth test against them
                // — where it overlaps blue the result should read as a
                // blended blue-green, and where it doesn't overlap anything
                // it should read as pure (fully opaque-looking, since
                // there's only background behind it) green.
                cmd.BindPipeline(unified_blend_pipeline);
                cmd.BindBindlessTextures();
                cmd.BindVertexBuffer(unified_offset_vertex_buffer, sizeof(UnifiedVertex));
                cmd.BindIndexBuffer(unified_offset_index_buffer, IndexFormat::UInt32);
                UnifiedPushConstants green_push{0.0f, unified_green_texture.index, 0.1f, 0.5f};
                cmd.SetPushConstants(&green_push, sizeof(green_push));
                cmd.DrawIndexed(6);

                cmd.EndRenderPass();
            } else {
                ResourceState before = used_before[slot] ? ResourceState::Present : ResourceState::Undefined;
                cmd.TransitionTexture(back_buffer, before, ResourceState::RenderTarget);

                f32 pulse = 0.5f + 0.5f * std::sin(t);
                cmd.ClearRenderTarget(back_buffer, {0.05f, 0.05f * pulse, 0.15f + 0.1f * pulse, 1.0f});

                cmd.TransitionTexture(back_buffer, ResourceState::RenderTarget, ResourceState::Present);
                used_before[slot] = true;
            }

            cmd.Close();
            frame_fences[slot] = device->Submit(cmd, swap_chain.get());
            swap_chain->Present(/*vsync=*/true);

            ++frame_index;
            if (max_frames >= 0 && frame_index >= max_frames) {
                AETHER_LOG_INFO("RHIDemo", "Reached AETHER_RHI_DEMO_MAX_FRAMES=%d, exiting", max_frames);
                break;
            }
        }

        for (u64 fence : frame_fences) {
            device->WaitForFence(fence);
        }

        if (const char* screenshot_path = std::getenv("AETHER_RHI_DEMO_SCREENSHOT")) {
            SaveScreenshot(*device, last_rendered_backbuffer, swap_chain->Width(), swap_chain->Height(),
                           screenshot_path);
        }

        AETHER_LOG_INFO("RHIDemo", "Shutting down cleanly");
    } catch (const std::exception& e) {
        AETHER_LOG_FATAL("RHIDemo", "Unhandled exception: %s", e.what());
        return 1;
    }

    return 0;
}
