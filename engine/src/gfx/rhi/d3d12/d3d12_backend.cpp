#include "aether/gfx/rhi/d3d12/d3d12_backend.h"

#include <limits>

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

D3D12Device::D3D12Device(bool enable_debug_layer) : device_(enable_debug_layer) {
    bindless_texture_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                               kMaxBindlessTextures, /*shader_visible=*/true);

    // A 1x1 white dummy texture, uploaded once and duplicated (by
    // CreateShaderResourceView pointing at the same resource, not by
    // allocating extra GPU memory) into every remaining bindless slot — see
    // bindless_texture_heap_'s header comment for why every slot needs a
    // valid descriptor up front. This claims index 0 permanently; real
    // CreateTexture() calls start at index 1.
    const u8 white_pixel[4] = {255, 255, 255, 255};
    gfx::CommandList setup_cmd(device_);
    setup_cmd.Reset();
    dummy_texture_ =
        std::make_unique<gfx::Texture>(device_, *bindless_texture_heap_, setup_cmd.Get(), 1, 1, white_pixel);
    setup_cmd.Close();
    ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
    device_.WaitForFence(device_.Submit(setup_lists, 1));
    dummy_texture_->ReleaseUploadStaging();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    for (u32 i = 1; i < kMaxBindlessTextures; ++i) {
        device_.Handle()->CreateShaderResourceView(dummy_texture_->Handle(), &srv_desc,
                                                    bindless_texture_heap_->CPUHandle(i));
    }
}

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

    if (desc.enable_bindless_textures) {
        AETHER_ASSERT(desc.push_constant_size_bytes > 0);
    }

    u32 num_constants = (desc.push_constant_size_bytes + 3) / 4;

    // Param 0: 32-bit push constants (b0), when requested. Param 1: the
    // bindless texture table (t0, kMaxBindlessTextures descriptors), when
    // requested — see BindBindlessTextures for the matching root index.
    D3D12_DESCRIPTOR_RANGE texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = kMaxBindlessTextures;
    texture_range.BaseShaderRegister = 0;
    texture_range.RegisterSpace = 0;
    texture_range.OffsetInDescriptorsFromTableStart = 0;

    std::vector<D3D12_ROOT_PARAMETER> params;
    if (num_constants > 0) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants = {/*ShaderRegister=*/0, /*RegisterSpace=*/0, num_constants};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(param);
    }
    if (desc.enable_bindless_textures) {
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {1, &texture_range};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params.push_back(param);
    }

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    // space1, matching the shared HLSL's `register(s0, space1)` — see the
    // "Unified mode's textured-quad pipeline" comment in rhi_demo/main.cpp
    // for why the sampler and the texture array (space0) must not share a
    // space (it's a Vulkan/SPIR-V binding-collision concern, not a D3D12
    // one, but the HLSL source is shared between both backends).
    sampler.RegisterSpace = 1;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = static_cast<UINT>(params.size());
    rs_desc.pParameters = params.empty() ? nullptr : params.data();
    rs_desc.NumStaticSamplers = desc.enable_bindless_textures ? 1 : 0;
    rs_desc.pStaticSamplers = desc.enable_bindless_textures ? &sampler : nullptr;
    rs_desc.Flags = desc.use_vertex_buffer ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                            : D3D12_ROOT_SIGNATURE_FLAG_NONE;

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

    // vs_5_1/ps_5_1, not 5_0: register spaces (used by the bindless texture
    // table's HLSL — see the "Unified mode's textured-quad pipeline"
    // comment in rhi_demo/main.cpp) are only valid from shader model 5.1.
    gfx::ShaderBytecode vs = gfx::CompileHLSL(desc.hlsl_source, desc.vs_entry.c_str(), "vs_5_1", "rhi_pipeline_vs");
    gfx::ShaderBytecode ps = gfx::CompileHLSL(desc.hlsl_source, desc.ps_entry.c_str(), "ps_5_1", "rhi_pipeline_ps");

    // Keep POSITION / TEXCOORD as the default, with optional normal and
    // tangent elements for lit, normal-mapped model vertices.
    const bool has_normals = desc.vertex_layout == VertexLayout::PositionNormalUv;
    const bool has_tangents = desc.vertex_layout == VertexLayout::PositionNormalUvTangent;
    D3D12_INPUT_ELEMENT_DESC input_elements[4] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, has_tangents || has_normals ? 24u : 12u,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = record.root_signature.Get();
    pso_desc.VS = {vs.Data(), vs.Size()};
    pso_desc.PS = {ps.Data(), ps.Size()};
    if (desc.use_vertex_buffer) {
        if (!has_tangents && !has_normals) {
            // PositionUv has no NORMAL semantic. Its second element is UV.
            input_elements[1] = input_elements[2];
        }
        const u32 element_count = has_tangents ? 4u : has_normals ? 3u : 2u;
        pso_desc.InputLayout = {input_elements, element_count};
    }
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = desc.cull_back_face ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;

    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (desc.enable_blending) {
        // Standard non-premultiplied alpha blending, matching the Vulkan
        // backend's equivalent VkPipelineColorBlendAttachmentState exactly.
        pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }

    // Always declared with the swap chain's own D32_FLOAT depth format
    // (always bound in BeginRenderPass — see D3D12SwapChain::DepthDSV's
    // comment), even for a pipeline with depth_test = false: D3D12 requires
    // a PSO's DSVFormat to be compatible with whatever DSV is actually bound
    // at draw time, regardless of whether that PSO's own DepthEnable is set.
    pso_desc.DepthStencilState.DepthEnable = desc.depth_test ? TRUE : FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = desc.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL
                                                                  : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    pso_desc.SampleMask = UINT_MAX;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = rtv_format;
    pso_desc.SampleDesc.Count = 1;

    AETHER_D3D_CHECK(device_.Handle()->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&record.pso)));

    pipelines_.push_back(std::move(record));
    return PipelineHandle{static_cast<u32>(pipelines_.size() - 1)};
}

BufferHandle D3D12Device::CreateBufferInternal(const void* data, u64 size_bytes) {
    if (!data || size_bytes == 0) {
        AETHER_LOG_ERROR("D3D12", "RHI buffers require non-null initial data and a non-zero size");
        throw std::invalid_argument("RHI buffers require non-null initial data and a non-zero size");
    }
    auto buffer = std::make_unique<gfx::Buffer>(device_, size_bytes, gfx::BufferKind::Upload);
    buffer->Update(data, size_bytes);
    buffers_.push_back(std::move(buffer));
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

void D3D12Device::UpdateVertexBuffer(BufferHandle buffer, const void* data, u64 size_bytes) {
    if (!buffer.IsValid() || buffer.index >= buffers_.size()) {
        AETHER_LOG_ERROR("D3D12", "UpdateVertexBuffer received an invalid buffer handle (%u)", buffer.index);
        return;
    }
    if (!data || size_bytes == 0 || size_bytes > buffers_[buffer.index]->Size()) {
        AETHER_LOG_ERROR("D3D12", "UpdateVertexBuffer received a null source or invalid size (%llu bytes)",
                         static_cast<unsigned long long>(size_bytes));
        return;
    }
    buffers_[buffer.index]->Update(data, size_bytes);
}

SampledTextureHandle D3D12Device::CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) {
    if (width == 0 || height == 0 || !rgba8_pixels ||
        static_cast<u64>(width) > std::numeric_limits<u64>::max() / 4 / height) {
        AETHER_LOG_ERROR("D3D12", "CreateTexture requires non-zero dimensions and non-null RGBA8 pixel data");
        throw std::invalid_argument("CreateTexture requires non-zero dimensions and non-null RGBA8 pixel data");
    }
    if (sampled_textures_.size() >= kMaxUserBindlessTextures) {
        AETHER_LOG_ERROR("D3D12", "CreateTexture exhausted the bindless texture table (capacity=%u real textures)",
                         kMaxUserBindlessTextures);
        return {};
    }

    gfx::CommandList setup_cmd(device_);
    setup_cmd.Reset();
    auto texture =
        std::make_unique<gfx::Texture>(device_, *bindless_texture_heap_, setup_cmd.Get(), width, height, rgba8_pixels);
    setup_cmd.Close();
    ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
    device_.WaitForFence(device_.Submit(setup_lists, 1));
    texture->ReleaseUploadStaging();

    u32 index = texture->BindlessIndex();
    // Kept alive for bindless_texture_heap_'s lifetime — the descriptor at
    // `index` points at texture->Handle(), which must stay valid.
    sampled_textures_.push_back(std::move(texture));
    return SampledTextureHandle{index};
}

D3D12SwapChain::D3D12SwapChain(D3D12Device& device, void* hwnd, u32 width, u32 height, u32 buffer_count)
    : device_(device), swap_chain_(device.Native(), hwnd, width, height, buffer_count) {
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        handles_.push_back(device_.RegisterTexture(swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i)));
    }
    used_before_.assign(swap_chain_.BufferCount(), false);
    CreateDepthBuffer(swap_chain_.Width(), swap_chain_.Height());
}

void D3D12SwapChain::CreateDepthBuffer(u32 width, u32 height) {
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

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

    if (!depth_dsv_heap_) {
        depth_dsv_heap_ = std::make_unique<DescriptorHeap>(device_.Native(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                                                             swap_chain_.BufferCount(), /*shader_visible=*/false);
        for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) depth_dsv_heap_->Allocate();
    }

    depth_resources_.clear();
    depth_resources_.resize(swap_chain_.BufferCount());
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        AETHER_D3D_CHECK(device_.Native().Handle()->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value,
            IID_PPV_ARGS(&depth_resources_[i])));
        device_.Native().Handle()->CreateDepthStencilView(depth_resources_[i].Get(), &dsv_desc,
                                                           depth_dsv_heap_->CPUHandle(i));
    }
}

void D3D12SwapChain::Resize(u32 width, u32 height) {
    swap_chain_.Resize(width, height);
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        device_.UpdateTexture(handles_[i], swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i));
    }
    used_before_.assign(swap_chain_.BufferCount(), false);
    CreateDepthBuffer(swap_chain_.Width(), swap_chain_.Height());
}

bool D3D12SwapChain::ReadBack(std::vector<u8>& rgba8) {
    if (swap_chain_.Format() != DXGI_FORMAT_R8G8B8A8_UNORM || Width() == 0 || Height() == 0) {
        return false;
    }

    ID3D12Resource* backbuffer = swap_chain_.BackBuffer(CurrentIndex());
    const D3D12_RESOURCE_DESC texture_desc = backbuffer->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u32 row_count = 0;
    u64 row_size = 0;
    u64 total_bytes = 0;
    device_.Native().Handle()->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, &row_count, &row_size,
                                                       &total_bytes);
    if (row_count != Height() || row_size != static_cast<u64>(Width()) * 4 ||
        total_bytes > static_cast<u64>(std::numeric_limits<usize>::max())) {
        return false;
    }

    gfx::Buffer readback(device_.Native(), total_bytes, gfx::BufferKind::Readback);
    ComPtr<ID3D12CommandAllocator> allocator;
    AETHER_D3D_CHECK(device_.Native().Handle()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                        IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> command_list;
    AETHER_D3D_CHECK(device_.Native().Handle()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                   allocator.Get(), nullptr,
                                                                   IID_PPV_ARGS(&command_list)));

    D3D12_RESOURCE_BARRIER to_copy = TransitionBarrier(backbuffer, D3D12_RESOURCE_STATE_PRESENT,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    command_list->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.Handle();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = backbuffer;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    D3D12_RESOURCE_BARRIER to_present = TransitionBarrier(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                          D3D12_RESOURCE_STATE_PRESENT);
    command_list->ResourceBarrier(1, &to_present);
    AETHER_D3D_CHECK(command_list->Close());

    ID3D12CommandList* lists[] = {command_list.Get()};
    const u64 fence = device_.Native().Submit(lists, 1);
    device_.Native().WaitForFence(fence);

    const usize row_bytes = static_cast<usize>(row_size);
    rgba8.resize(row_bytes * Height());
    for (u32 row = 0; row < Height(); ++row) {
        readback.Read(rgba8.data() + static_cast<usize>(row) * row_bytes, row_bytes,
                      footprint.Offset + static_cast<u64>(row) * footprint.Footprint.RowPitch);
    }
    return true;
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

    // Always bound and cleared, regardless of whether the pipeline this
    // pass ends up drawing with actually depth-tests (PipelineDesc::
    // depth_test) — mirrors the Vulkan backend's default render pass always
    // having a depth attachment. A pipeline with depth_test=false simply
    // never reads or writes it.
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = d3d_swap.DepthDSV(index);
    cmd_.Get()->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd_.Get()->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

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

void D3D12CommandList::BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) {
    const gfx::Buffer& buf = device_.GetBuffer(buffer);
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = buf.GPUAddress();
    vbv.SizeInBytes = static_cast<UINT>(buf.Size());
    vbv.StrideInBytes = stride_bytes;
    cmd_.Get()->IASetVertexBuffers(0, 1, &vbv);
}

void D3D12CommandList::BindIndexBuffer(BufferHandle buffer, IndexFormat format) {
    const gfx::Buffer& buf = device_.GetBuffer(buffer);
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = buf.GPUAddress();
    ibv.SizeInBytes = static_cast<UINT>(buf.Size());
    ibv.Format = format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    cmd_.Get()->IASetIndexBuffer(&ibv);
}

void D3D12CommandList::DrawIndexed(u32 index_count) {
    cmd_.Get()->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
}

void D3D12CommandList::BindBindlessTextures() {
    ID3D12DescriptorHeap* heaps[] = {device_.BindlessTextureHeap().Heap()};
    cmd_.Get()->SetDescriptorHeaps(1, heaps);
    cmd_.Get()->SetGraphicsRootDescriptorTable(1, device_.BindlessTextureHeap().GPUHandle(0));
}

} // namespace aether::gfx::rhi::d3d12_backend
