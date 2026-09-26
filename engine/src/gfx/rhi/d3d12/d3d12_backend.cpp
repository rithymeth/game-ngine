#include "aether/gfx/rhi/d3d12/d3d12_backend.h"

#include "aether/core/log.h"
#include "aether/gfx/shader_compiler.h"

#include <stdexcept>

namespace aether::gfx::rhi::d3d12_backend {

D3D12_RESOURCE_STATES ToD3D12(ResourceState state) {
    switch (state) {
        case ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
        case ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::Undefined: default: return D3D12_RESOURCE_STATE_COMMON;
    }
}

D3D12Device::D3D12Device(bool enable_debug_layer) : device_(enable_debug_layer) {}

std::unique_ptr<ISwapChain> D3D12Device::CreateSwapChain(void* native_window_handle, u32 width, u32 height,
                                                          u32 buffer_count) {
    return std::make_unique<D3D12SwapChain>(*this, native_window_handle, width, height, buffer_count);
}

std::unique_ptr<ICommandList> D3D12Device::CreateCommandList() {
    return std::make_unique<D3D12CommandList>(*this);
}

u64 D3D12Device::Submit(ICommandList& cmd, ISwapChain* wait_on_swap_chain) {
    (void)wait_on_swap_chain; // D3D12's flip-model swap chain needs no acquire/present semaphores
    auto* native = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12CommandList* lists[] = {native};
    return device_.Submit(lists, 1);
}

void D3D12Device::WaitForFence(u64 fence_value) {
    device_.WaitForFence(fence_value);
}

std::unique_ptr<ICommandList> D3D12Device::CreateComputeCommandList() {
    return std::make_unique<D3D12CommandList>(*this, D3D12_COMMAND_LIST_TYPE_COMPUTE);
}

u64 D3D12Device::SubmitCompute(ICommandList& cmd) {
    auto* native = static_cast<ID3D12GraphicsCommandList*>(cmd.NativeHandle());
    ID3D12CommandList* lists[] = {native};
    return device_.SubmitCompute(lists, 1);
}

TextureHandle D3D12Device::RegisterTexture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    TextureHandle handle{static_cast<u32>(textures_.size())};
    textures_.push_back({resource, rtv});
    return handle;
}

void D3D12Device::UpdateTexture(TextureHandle handle, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    textures_[handle.index] = {resource, rtv};
}

PipelineHandle D3D12Device::CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) {
    auto& d3d_swap = static_cast<D3D12SwapChain&>(swap_chain);
    DXGI_FORMAT rtv_format = d3d_swap.Native().Format();

    u32 num_constants = (desc.push_constant_size_bytes + 3) / 4;
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, num_constants};
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = num_constants > 0 ? 1 : 0;
    rs_desc.pParameters = num_constants > 0 ? &param : nullptr;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("RHI", "CreatePipeline: root signature serialization failed: %s", message);
        throw std::runtime_error("root signature serialization failed");
    }

    PipelineRecord record;
    AETHER_D3D_CHECK(device_.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                            IID_PPV_ARGS(&record.root_signature)));

    gfx::ShaderBytecode vs = gfx::CompileHLSL(desc.hlsl_source, desc.vs_entry.c_str(), "vs_5_0", "rhi_pipeline_vs");
    gfx::ShaderBytecode ps = gfx::CompileHLSL(desc.hlsl_source, desc.ps_entry.c_str(), "ps_5_0", "rhi_pipeline_ps");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = record.root_signature.Get();
    pso_desc.VS = {vs.Data(), vs.Size()};
    pso_desc.PS = {ps.Data(), ps.Size()};
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = desc.cull_back_face ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = rtv_format;
    pso_desc.SampleDesc.Count = 1;

    AETHER_D3D_CHECK(device_.Handle()->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&record.pso)));

    pipelines_.push_back(std::move(record));
    return PipelineHandle{static_cast<u32>(pipelines_.size() - 1)};
}

D3D12SwapChain::D3D12SwapChain(D3D12Device& device, void* hwnd, u32 width, u32 height, u32 buffer_count)
    : device_(device), swap_chain_(device.Native(), hwnd, width, height, buffer_count) {
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        handles_.push_back(device_.RegisterTexture(swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i)));
    }
    used_before_.assign(swap_chain_.BufferCount(), false);
}

void D3D12SwapChain::Resize(u32 width, u32 height) {
    swap_chain_.Resize(width, height);
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        device_.UpdateTexture(handles_[i], swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i));
    }
    used_before_.assign(swap_chain_.BufferCount(), false);
}

D3D12CommandList::D3D12CommandList(D3D12Device& device, D3D12_COMMAND_LIST_TYPE type)
    : device_(device), cmd_(device.Native(), type) {}

void D3D12CommandList::TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) {
    const TextureRecord& record = device_.GetTexture(texture);
    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(record.resource, ToD3D12(before), ToD3D12(after));
    cmd_.Get()->ResourceBarrier(1, &barrier);
}

void D3D12CommandList::ClearRenderTarget(TextureHandle texture, const ClearColor& color) {
    const TextureRecord& record = device_.GetTexture(texture);
    const f32 c[4] = {color.r, color.g, color.b, color.a};
    cmd_.Get()->ClearRenderTargetView(record.rtv, c, 0, nullptr);
}

void D3D12CommandList::SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) {
    D3D12_VIEWPORT vp{viewport.x,    viewport.y,        viewport.width,
                       viewport.height, viewport.min_depth, viewport.max_depth};
    D3D12_RECT rect{scissor.left, scissor.top, scissor.right, scissor.bottom};
    cmd_.Get()->RSSetViewports(1, &vp);
    cmd_.Get()->RSSetScissorRects(1, &rect);
}

void D3D12CommandList::BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) {
    auto& d3d_swap = static_cast<D3D12SwapChain&>(swap_chain);
    u32 index = d3d_swap.CurrentIndex();
    ID3D12Resource* backbuffer = d3d_swap.Native().BackBuffer(index);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = d3d_swap.Native().BackBufferRTV(index);

    D3D12_RESOURCE_STATES before =
        d3d_swap.ConsumeUsedBefore(index) ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER to_rt = TransitionBarrier(backbuffer, before, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd_.Get()->ResourceBarrier(1, &to_rt);

    const f32 c[4] = {clear_color.r, clear_color.g, clear_color.b, clear_color.a};
    cmd_.Get()->ClearRenderTargetView(rtv, c, 0, nullptr);
    cmd_.Get()->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<f32>(d3d_swap.Width()), static_cast<f32>(d3d_swap.Height()),
                             0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(d3d_swap.Width()), static_cast<LONG>(d3d_swap.Height())};
    cmd_.Get()->RSSetViewports(1, &viewport);
    cmd_.Get()->RSSetScissorRects(1, &scissor);

    active_backbuffer_ = backbuffer;
}

void D3D12CommandList::EndRenderPass() {
    D3D12_RESOURCE_BARRIER to_present =
        TransitionBarrier(active_backbuffer_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd_.Get()->ResourceBarrier(1, &to_present);
    active_backbuffer_ = nullptr;
}

void D3D12CommandList::BindPipeline(PipelineHandle pipeline) {
    const PipelineRecord& record = device_.GetPipeline(pipeline);
    cmd_.Get()->SetGraphicsRootSignature(record.root_signature.Get());
    cmd_.Get()->SetPipelineState(record.pso.Get());
    cmd_.Get()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D12CommandList::SetPushConstants(const void* data, u32 size_bytes) {
    cmd_.Get()->SetGraphicsRoot32BitConstants(0, (size_bytes + 3) / 4, data, 0);
}

void D3D12CommandList::Draw(u32 vertex_count) {
    cmd_.Get()->DrawInstanced(vertex_count, 1, 0, 0);
}

} // namespace aether::gfx::rhi::d3d12_backend
