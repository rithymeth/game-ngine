#include "aether/gfx/rhi/d3d12/d3d12_backend.h"

#include <limits>
#include <cmath>

#include "aether/core/log.h"
#include "aether/gfx/shader_compiler.h"

#include <stdexcept>

namespace aether::gfx::rhi::d3d12_backend {

namespace {

bool IsFiniteClearColor(const ClearColor& color) {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && std::isfinite(color.a);
}

bool IsValidResourceState(ResourceState state) {
    switch (state) {
        case ResourceState::Undefined:
        case ResourceState::RenderTarget:
        case ResourceState::Present:
        case ResourceState::CopyDest:
        case ResourceState::CopySource: return true;
        default: return false;
    }
}

bool IsValidVertexLayout(VertexLayout layout) {
    switch (layout) {
        case VertexLayout::PositionUv:
        case VertexLayout::PositionNormalUv:
        case VertexLayout::PositionNormalUvTangent: return true;
        default: return false;
    }
}

} // namespace

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
    deferred_command_allocators_.reserve(64);
    bindless_texture_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                               kMaxBindlessTextures, /*shader_visible=*/true);
    compute_storage_heap_ = std::make_unique<DescriptorHeap>(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                              8192, /*shader_visible=*/true);
    sampled_descriptor_indices_.resize(kMaxUserBindlessTextures, DescriptorHeap::kInvalidIndex);
    sampled_float_textures_.resize(kMaxUserBindlessTextures);
    sampled_textures_.reserve(kMaxUserBindlessTextures);

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

D3D12Device::~D3D12Device() {
    try {
        device_.WaitForFence(device_.Submit(nullptr, 0));
    } catch (const std::exception& error) {
        AETHER_LOG_WARN("D3D12", "Could not drain graphics queue before resource teardown: %s", error.what());
    } catch (...) {
        AETHER_LOG_WARN("D3D12", "Could not drain graphics queue before resource teardown");
    }
    try {
        device_.WaitForComputeFence(device_.SubmitCompute(nullptr, 0));
    } catch (const std::exception& error) {
        AETHER_LOG_WARN("D3D12", "Could not drain compute queue before resource teardown: %s", error.what());
    } catch (...) {
        AETHER_LOG_WARN("D3D12", "Could not drain compute queue before resource teardown");
    }
}

void D3D12Device::DeferCommandAllocator(ComPtr<ID3D12CommandAllocator> allocator) noexcept {
    if (!allocator) return;
    try {
        std::lock_guard<std::mutex> lock(deferred_allocator_mutex_);
        deferred_command_allocators_.push_back(std::move(allocator));
    } catch (...) {
        // If even deferred bookkeeping cannot grow, leaking the COM reference
        // is safer than releasing an allocator that the GPU may still use.
        (void)allocator.Detach();
        AETHER_LOG_ERROR("D3D12", "Could not retain an in-flight command allocator; keeping its COM reference alive");
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
    auto* rhi_cmd = dynamic_cast<D3D12CommandList*>(&cmd);
    if (!rhi_cmd || !rhi_cmd->BelongsTo(*this) || rhi_cmd->IsComputeList() || !rhi_cmd->NativeHandle()) {
        AETHER_LOG_ERROR("D3D12", "Submit requires a closed graphics command list created by this device");
        return 0;
    }
    std::lock_guard<std::mutex> command_lock(rhi_cmd->SubmissionMutex());
    if (!rhi_cmd->IsReadyForSubmit() || rhi_cmd->IsSubmissionInFlight()) {
        AETHER_LOG_ERROR("D3D12", "Submit requires a closed graphics command list that is not already in flight");
        return 0;
    }
    auto* native = static_cast<ID3D12GraphicsCommandList*>(rhi_cmd->NativeHandle());
    ID3D12CommandList* lists[] = {native};
    const u64 fence_value = device_.Submit(lists, 1);
    if (fence_value != 0) rhi_cmd->MarkSubmitted(fence_value);
    return fence_value;
}

void D3D12Device::WaitForFence(u64 fence_value) {
    device_.WaitForFence(fence_value);
}

std::unique_ptr<ICommandList> D3D12Device::CreateComputeCommandList() {
    return std::make_unique<D3D12CommandList>(*this, D3D12_COMMAND_LIST_TYPE_COMPUTE);
}

u64 D3D12Device::SubmitCompute(ICommandList& cmd) {
    auto* rhi_cmd = dynamic_cast<D3D12CommandList*>(&cmd);
    if (!rhi_cmd || !rhi_cmd->BelongsTo(*this) || !rhi_cmd->IsComputeList() || !rhi_cmd->NativeHandle()) {
        AETHER_LOG_ERROR("D3D12", "SubmitCompute requires a closed compute command list created by this device");
        return 0;
    }
    std::lock_guard<std::mutex> command_lock(rhi_cmd->SubmissionMutex());
    if (!rhi_cmd->IsReadyForSubmit() || rhi_cmd->IsSubmissionInFlight()) {
        AETHER_LOG_ERROR("D3D12", "SubmitCompute requires a closed compute command list that is not already in flight");
        return 0;
    }
    auto* native = static_cast<ID3D12GraphicsCommandList*>(rhi_cmd->NativeHandle());
    ID3D12CommandList* lists[] = {native};
    const u64 fence_value = device_.SubmitCompute(lists, 1);
    if (fence_value != 0) rhi_cmd->MarkSubmitted(fence_value);
    return fence_value;
}

TextureHandle D3D12Device::RegisterTexture(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    if (!resource || rtv.ptr == 0) {
        AETHER_LOG_ERROR("D3D12", "RegisterTexture requires a resource and render-target descriptor");
        return {};
    }
    if (textures_.size() >= static_cast<usize>(static_cast<u32>(-1))) {
        AETHER_LOG_ERROR("D3D12", "Texture registry exhausted its 32-bit handle space");
        return {};
    }
    TextureHandle handle{static_cast<u32>(textures_.size())};
    textures_.push_back({resource, rtv});
    return handle;
}

void D3D12Device::UpdateTexture(TextureHandle handle, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    if (!IsTextureValid(handle) || !resource || rtv.ptr == 0) {
        AETHER_LOG_ERROR("D3D12", "UpdateTexture received an invalid handle, resource, or render-target descriptor");
        return;
    }
    textures_[handle.index] = {resource, rtv};
}

PipelineHandle D3D12Device::CreatePipeline(const PipelineDesc& desc, ISwapChain& swap_chain) {
    auto* d3d_swap_ptr = dynamic_cast<D3D12SwapChain*>(&swap_chain);
    if (!d3d_swap_ptr || !d3d_swap_ptr->BelongsTo(*this)) {
        AETHER_LOG_ERROR("D3D12", "CreatePipeline requires a D3D12 swap chain created by this device");
        return {};
    }
    if (!IsValidVertexLayout(desc.vertex_layout)) {
        AETHER_LOG_ERROR("D3D12", "CreatePipeline received an unsupported vertex layout");
        return {};
    }
    auto& d3d_swap = *d3d_swap_ptr;
    DXGI_FORMAT rtv_format = d3d_swap.Native().Format();

    const u32 max_root_constants = desc.enable_bindless_textures ? 63u : 64u;
    if (desc.push_constant_size_bytes % 4 != 0 || desc.push_constant_size_bytes / 4 > max_root_constants) {
        AETHER_LOG_ERROR("D3D12", "Graphics push-constant size must be four-byte aligned and fit the root signature");
        throw std::invalid_argument("Graphics push-constant size exceeds D3D12 root-signature limits");
    }

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
    // The frame constants: a root CBV at b0, space2 (the HLSL's `register(b0, space2)`).
    u32 frame_constants_root_index = ~0u;
    if (desc.frame_constants_size_bytes > 0) {
        AETHER_ASSERT(desc.frame_constants_size_bytes % 16 == 0);
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor = {/*ShaderRegister=*/0, /*RegisterSpace=*/2};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        frame_constants_root_index = static_cast<u32>(params.size());
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
    record.frame_constants_root_index = frame_constants_root_index;
    record.push_constant_size_bytes = desc.push_constant_size_bytes;
    record.enable_bindless_textures = desc.enable_bindless_textures;
    record.use_vertex_buffer = desc.use_vertex_buffer;
    record.vertex_stride_bytes = desc.vertex_layout == VertexLayout::PositionNormalUvTangent
                                     ? 48u
                                     : desc.vertex_layout == VertexLayout::PositionNormalUv ? 32u : 20u;
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

    // D3D12_GRAPHICS_PIPELINE_STATE_DESC is zero-initialized above, but a
    // zeroed D3D12_RENDER_TARGET_BLEND_DESC is not the API's default state:
    // its blend factors and operations are invalid enum values. Set the
    // complete default render-target blend state before optionally enabling
    // alpha blending, otherwise CreateGraphicsPipelineState rejects the
    // editor viewport pipeline with E_INVALIDARG on Windows.
    auto& render_target_blend = pso_desc.BlendState.RenderTarget[0];
    render_target_blend.SrcBlend = D3D12_BLEND_ONE;
    render_target_blend.DestBlend = D3D12_BLEND_ZERO;
    render_target_blend.BlendOp = D3D12_BLEND_OP_ADD;
    render_target_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    render_target_blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    render_target_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    render_target_blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    render_target_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (desc.enable_blending) {
        // Standard non-premultiplied alpha blending, matching the Vulkan
        // backend's equivalent VkPipelineColorBlendAttachmentState exactly.
        render_target_blend.BlendEnable = TRUE;
        render_target_blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        render_target_blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
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

ComputePipelineHandle D3D12Device::CreateComputePipeline(const ComputePipelineDesc& desc) {
    if (desc.hlsl_source.empty() || desc.entry.empty()) {
        throw std::invalid_argument("ComputePipelineDesc requires HLSL source and an entry point");
    }
    if (desc.storage_buffer_count > 32) {
        throw std::invalid_argument("ComputePipelineDesc supports at most 32 storage buffers");
    }
    if (desc.sampled_texture_count > 32) {
        throw std::invalid_argument("ComputePipelineDesc supports at most 32 sampled textures");
    }
    if (desc.constant_buffer_count > 14 || desc.storage_buffer_count + desc.sampled_texture_count + desc.constant_buffer_count > 64) {
        throw std::invalid_argument("ComputePipelineDesc exceeds supported root buffer bindings");
    }
    ComputePipelineRecord record;
    record.storage_buffer_count = desc.storage_buffer_count;
    record.sampled_texture_count = desc.sampled_texture_count;
    record.constant_buffer_count = desc.constant_buffer_count;
    if (desc.storage_buffer_count + desc.sampled_texture_count + desc.constant_buffer_count * 2 > 64) {
        throw std::invalid_argument("ComputePipelineDesc exceeds the D3D12 root signature DWORD limit");
    }
    std::vector<D3D12_ROOT_PARAMETER> params(desc.storage_buffer_count + desc.sampled_texture_count + desc.constant_buffer_count);
    std::vector<D3D12_DESCRIPTOR_RANGE> storage_ranges(desc.storage_buffer_count);
    for (u32 i = 0; i < desc.storage_buffer_count; ++i) {
        storage_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        storage_ranges[i].NumDescriptors = 1;
        storage_ranges[i].BaseShaderRegister = i;
        storage_ranges[i].RegisterSpace = 0;
        storage_ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &storage_ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    std::vector<D3D12_DESCRIPTOR_RANGE> sampled_ranges(desc.sampled_texture_count);
    for (u32 i = 0; i < desc.sampled_texture_count; ++i) {
        sampled_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sampled_ranges[i].NumDescriptors = 1;
        sampled_ranges[i].BaseShaderRegister = i;
        sampled_ranges[i].RegisterSpace = 2; // shared compute sampled-texture bindings
        sampled_ranges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER& param = params[desc.storage_buffer_count + i];
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &sampled_ranges[i];
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (u32 i = 0; i < desc.constant_buffer_count; ++i) {
        D3D12_ROOT_PARAMETER& param = params[desc.storage_buffer_count + desc.sampled_texture_count + i];
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        param.Descriptor = {i, 1}; // HLSL register(bN, space1)
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = static_cast<UINT>(params.size());
    root_desc.pParameters = params.empty() ? nullptr : params.data();
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    const HRESULT serialize_hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                             &signature, &error);
    if (FAILED(serialize_hr)) {
        const char* message = error ? static_cast<const char*>(error->GetBufferPointer()) : "(no error blob)";
        AETHER_LOG_FATAL("RHI", "CreateComputePipeline: root signature serialization failed: %s", message);
        throw std::runtime_error("compute root signature serialization failed");
    }
    AETHER_D3D_CHECK(device_.Handle()->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                            IID_PPV_ARGS(&record.root_signature)));
    const gfx::ShaderBytecode shader = gfx::CompileHLSL(desc.hlsl_source, desc.entry.c_str(), "cs_5_1",
                                                        "rhi_compute_pipeline");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = record.root_signature.Get();
    pso_desc.CS = {shader.Data(), shader.Size()};
    AETHER_D3D_CHECK(device_.Handle()->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&record.pso)));
    compute_pipelines_.push_back(std::move(record));
    return ComputePipelineHandle{static_cast<u32>(compute_pipelines_.size() - 1)};
}

BufferHandle D3D12Device::CreateStorageBuffer(u64 size_bytes, u32 element_stride_bytes, const void* initial_data) {
    if (size_bytes == 0 || size_bytes > std::numeric_limits<UINT>::max()) {
        throw std::invalid_argument("Storage buffer size must be non-zero and fit the D3D12 buffer view limits");
    }
    if ((element_stride_bytes != 0 && (size_bytes % element_stride_bytes != 0 || element_stride_bytes > 2048)) ||
        (element_stride_bytes == 0 && size_bytes % 4 != 0)) {
        throw std::invalid_argument("Storage buffer size must be divisible by its valid element stride");
    }
    ReserveBufferRegistryEntry();
    if (!compute_storage_heap_->CanAllocate()) {
        throw std::runtime_error("D3D12 compute storage descriptor heap is full");
    }
    std::vector<u8> zeroes;
    if (!initial_data) {
        zeroes.resize(static_cast<usize>(size_bytes), 0);
        initial_data = zeroes.data();
    }
    auto buffer = std::make_unique<gfx::Buffer>(device_, size_bytes, gfx::BufferKind::Default, true);
    gfx::Buffer staging(device_, size_bytes, gfx::BufferKind::Upload);
    staging.Update(initial_data, size_bytes);
    gfx::CommandList copy(device_);
    copy.Reset();
    D3D12_RESOURCE_BARRIER to_copy = TransitionBarrier(buffer->Handle(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                       D3D12_RESOURCE_STATE_COPY_DEST);
    copy->ResourceBarrier(1, &to_copy);
    copy->CopyBufferRegion(buffer->Handle(), 0, staging.Handle(), 0, size_bytes);
    D3D12_RESOURCE_BARRIER to_uav = TransitionBarrier(buffer->Handle(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    copy->ResourceBarrier(1, &to_uav);
    copy.Close();
    ID3D12CommandList* lists[] = {copy.Get()};
    device_.WaitForFence(device_.Submit(lists, 1));
    const u32 descriptor_index = compute_storage_heap_->Allocate();
    if (descriptor_index == DescriptorHeap::kInvalidIndex) {
        throw std::length_error("D3D12 compute storage descriptor allocation failed");
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    if (element_stride_bytes == 0) {
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.Buffer.NumElements = static_cast<UINT>(size_bytes / 4);
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    } else {
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer.NumElements = static_cast<UINT>(size_bytes / element_stride_bytes);
        uav.Buffer.StructureByteStride = element_stride_bytes;
    }
    device_.Handle()->CreateUnorderedAccessView(buffer->Handle(), nullptr, &uav,
                                                 compute_storage_heap_->CPUHandle(descriptor_index));
    buffers_.push_back(std::move(buffer));
    storage_descriptor_indices_.push_back(descriptor_index);
    constant_buffer_flags_.push_back(false);
    buffer_vertex_flags_.push_back(false);
    index_buffer_formats_.push_back(std::nullopt);
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

void D3D12Device::UpdateStorageBuffer(BufferHandle buffer, const void* data, u64 size_bytes, u64 offset) {
    if (!buffer.IsValid() || buffer.index >= buffers_.size() || buffer.index >= storage_descriptor_indices_.size() ||
        storage_descriptor_indices_[buffer.index] == DescriptorHeap::kInvalidIndex || !data || size_bytes == 0 ||
        offset > buffers_[buffer.index]->Size() || size_bytes > buffers_[buffer.index]->Size() - offset) {
        AETHER_LOG_ERROR("D3D12", "UpdateStorageBuffer received an invalid handle, source, or range");
        return;
    }
    device_.WithComputeQueueIdle([&] {
        gfx::Buffer staging(device_, size_bytes, gfx::BufferKind::Upload);
        staging.Update(data, size_bytes);
        gfx::CommandList copy(device_);
        copy.Reset();
        ID3D12Resource* resource = buffers_[buffer.index]->Handle();
        D3D12_RESOURCE_BARRIER to_copy = TransitionBarrier(resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                           D3D12_RESOURCE_STATE_COPY_DEST);
        copy->ResourceBarrier(1, &to_copy);
        copy->CopyBufferRegion(resource, offset, staging.Handle(), 0, size_bytes);
        D3D12_RESOURCE_BARRIER to_uav = TransitionBarrier(resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        copy->ResourceBarrier(1, &to_uav);
        copy.Close();
        ID3D12CommandList* lists[] = {copy.Get()};
        device_.WaitForFence(device_.Submit(lists, 1));
    });
}

BufferHandle D3D12Device::CreateConstantBuffer(u64 size_bytes, const void* initial_data) {
    if (size_bytes == 0 || size_bytes > std::numeric_limits<UINT>::max() - 255) {
        throw std::invalid_argument("Constant buffer size must be non-zero and fit D3D12 limits");
    }
    ReserveBufferRegistryEntry();
    const u64 aligned_size = (size_bytes + 255u) & ~u64(255u);
    auto buffer = std::make_unique<gfx::Buffer>(device_, aligned_size, gfx::BufferKind::Upload);
    if (initial_data) {
        buffer->Update(initial_data, size_bytes);
    } else {
        std::vector<u8> zeroes(static_cast<usize>(aligned_size), 0);
        buffer->Update(zeroes.data(), aligned_size);
    }
    buffers_.push_back(std::move(buffer));
    storage_descriptor_indices_.push_back(DescriptorHeap::kInvalidIndex);
    constant_buffer_flags_.push_back(true);
    buffer_vertex_flags_.push_back(false);
    index_buffer_formats_.push_back(std::nullopt);
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

void D3D12Device::UpdateConstantBuffer(BufferHandle buffer, const void* data, u64 size_bytes, u64 offset) {
    if (!IsConstantBuffer(buffer) || !data || size_bytes == 0 ||
        offset > buffers_[buffer.index]->Size() || size_bytes > buffers_[buffer.index]->Size() - offset) {
        AETHER_LOG_ERROR("D3D12", "UpdateConstantBuffer received an invalid handle, source, or range");
        return;
    }
    device_.WithQueuesIdle([&] { buffers_[buffer.index]->Update(data, size_bytes, offset); });
}

BufferHandle D3D12Device::CreateVertexBuffer(const void* data, u64 size_bytes) {
    return CreateBufferInternal(data, size_bytes, true);
}

BufferHandle D3D12Device::CreateIndexBuffer(const void* data, u64 size_bytes, IndexFormat format) {
    const u32 index_size = format == IndexFormat::UInt16 ? 2u : format == IndexFormat::UInt32 ? 4u : 0u;
    if (index_size == 0 || size_bytes == 0 || size_bytes % index_size != 0) {
        throw std::invalid_argument("Index buffer size must be non-zero, aligned, and use a supported index format");
    }
    return CreateBufferInternal(data, size_bytes, false, format);
}

BufferHandle D3D12Device::CreateBufferInternal(const void* data, u64 size_bytes, bool is_vertex,
                                               std::optional<IndexFormat> index_format) {
    if (!data || size_bytes == 0) {
        AETHER_LOG_ERROR("D3D12", "RHI buffers require non-null initial data and a non-zero size");
        throw std::invalid_argument("RHI buffers require non-null initial data and a non-zero size");
    }
    if (size_bytes > std::numeric_limits<UINT>::max()) {
        AETHER_LOG_ERROR("D3D12", "Vertex/index buffer size exceeds the native view size limit");
        throw std::invalid_argument("Vertex/index buffer size exceeds the D3D12 view size limit");
    }
    ReserveBufferRegistryEntry();
    auto buffer = std::make_unique<gfx::Buffer>(device_, size_bytes, gfx::BufferKind::Upload);
    buffer->Update(data, size_bytes);
    buffers_.push_back(std::move(buffer));
    storage_descriptor_indices_.push_back(DescriptorHeap::kInvalidIndex);
    constant_buffer_flags_.push_back(false);
    buffer_vertex_flags_.push_back(is_vertex);
    index_buffer_formats_.push_back(index_format);
    return BufferHandle{static_cast<u32>(buffers_.size() - 1)};
}

BufferHandle D3D12Device::CreateFrameConstantBuffer(u64 size_bytes) {
    if (size_bytes == 0) {
        AETHER_LOG_ERROR("D3D12", "CreateConstantBuffer requires a non-zero size");
        throw std::invalid_argument("CreateConstantBuffer requires a non-zero size");
    }
    const u64 padded = (size_bytes + 255) / 256 * 256; // constant data is read in 256-byte units
    return CreateConstantBuffer(padded, nullptr);
}

void D3D12Device::ReserveBufferRegistryEntry() {
    const usize next_size = buffers_.size() + 1;
    if (next_size <= buffers_.size() || next_size > buffers_.max_size() ||
        next_size > storage_descriptor_indices_.max_size() || next_size > constant_buffer_flags_.max_size()) {
        throw std::length_error("D3D12 buffer registry capacity is exhausted");
    }
    buffers_.reserve(next_size);
    storage_descriptor_indices_.reserve(next_size);
    constant_buffer_flags_.reserve(next_size);
    buffer_vertex_flags_.reserve(next_size);
    index_buffer_formats_.reserve(next_size);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::ComputeStorageDescriptor(BufferHandle handle) const {
    if (!handle.IsValid() || handle.index >= storage_descriptor_indices_.size() ||
        storage_descriptor_indices_[handle.index] == DescriptorHeap::kInvalidIndex) return {};
    return compute_storage_heap_->GPUHandle(storage_descriptor_indices_[handle.index]);
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Device::ComputeSampledDescriptor(SampledTextureHandle handle) const {
    if (!handle.IsValid() || handle.index < kFirstUserBindlessTextureIndex) return {};
    const u32 index = handle.index - kFirstUserBindlessTextureIndex;
    if (index >= sampled_texture_count_ || index >= sampled_descriptor_indices_.size() ||
        sampled_descriptor_indices_[index] == DescriptorHeap::kInvalidIndex) return {};
    return compute_storage_heap_->GPUHandle(sampled_descriptor_indices_[index]);
}

void D3D12Device::UpdateVertexBuffer(BufferHandle buffer, const void* data, u64 size_bytes) {
    if (!IsVertexBuffer(buffer) || buffer.index >= buffers_.size()) {
        AETHER_LOG_ERROR("D3D12", "UpdateVertexBuffer received an invalid buffer handle (%u)", buffer.index);
        return;
    }
    if (!data || size_bytes == 0 || size_bytes > buffers_[buffer.index]->Size()) {
        AETHER_LOG_ERROR("D3D12", "UpdateVertexBuffer received a null source or invalid size (%llu bytes)",
                         static_cast<unsigned long long>(size_bytes));
        return;
    }
    device_.WithGraphicsQueueIdle([&] { buffers_[buffer.index]->Update(data, size_bytes); });
}

SampledTextureHandle D3D12Device::CreateTexture(u32 width, u32 height, const u8* rgba8_pixels) {
    if (width == 0 || height == 0 || !rgba8_pixels ||
        width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        static_cast<u64>(width) > std::numeric_limits<u64>::max() / 4 / height) {
        AETHER_LOG_ERROR("D3D12", "CreateTexture requires a supported extent and non-null RGBA8 pixel data");
        throw std::invalid_argument("CreateTexture requires a supported extent and non-null RGBA8 pixel data");
    }
    if (sampled_texture_count_ >= kMaxUserBindlessTextures || !bindless_texture_heap_->CanAllocate() ||
        !compute_storage_heap_->CanAllocate()) {
        AETHER_LOG_ERROR("D3D12", "CreateTexture exhausted sampled-texture or compute descriptors");
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
    u32 compute_index = DescriptorHeap::kInvalidIndex;
    device_.WithQueuesIdle([&] {
        compute_index = compute_storage_heap_->Allocate();
        if (compute_index == DescriptorHeap::kInvalidIndex) {
            // gfx::Texture already wrote its SRV into this slot. Restore the
            // initialized fallback before its resource owner is released.
            D3D12_SHADER_RESOURCE_VIEW_DESC fallback_srv{};
            fallback_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            fallback_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fallback_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fallback_srv.Texture2D.MipLevels = 1;
            device_.Handle()->CreateShaderResourceView(dummy_texture_->Handle(), &fallback_srv,
                                                        bindless_texture_heap_->CPUHandle(index));
            bindless_texture_heap_->Free(index);
            throw std::length_error("Cannot create texture: compute descriptor allocation failed");
        }
        device_.Handle()->CopyDescriptorsSimple(1, compute_storage_heap_->CPUHandle(compute_index),
                                                 bindless_texture_heap_->CPUHandle(index),
                                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    });
    const usize descriptor_slot = index - kFirstUserBindlessTextureIndex;
    sampled_descriptor_indices_[descriptor_slot] = compute_index;
    // Kept alive for bindless_texture_heap_'s lifetime — the descriptor at
    // `index` points at texture->Handle(), which must stay valid.
    sampled_textures_.push_back(std::move(texture));
    ++sampled_texture_count_;
    return SampledTextureHandle{index};
}

SampledTextureHandle D3D12Device::CreateCompressedTexture(CompressedTextureFormat format, u32 width, u32 height, std::span<const CompressedMip> mips) {
    const u32 block_bytes = CompressedBlockBytes(format);
    const u32 block = CompressedBlockWidth(format);
    if (width == 0 || height == 0 || mips.empty() || mips.size() > 16) {
        AETHER_LOG_ERROR("D3D12", "CreateCompressedTexture requires non-zero dimensions and 1 to 16 mip levels");
        throw std::invalid_argument("CreateCompressedTexture requires non-zero dimensions and 1 to 16 mip levels");
    }
    std::vector<gfx::Texture::MipData> levels;
    u32 w = width, h = height;
    for (const CompressedMip& mip : mips) {
        const u32 blocks_x = (w + block - 1) / block, blocks_y = (h + block - 1) / block;
        if (mip.data == nullptr || mip.width != w || mip.height != h || mip.size_bytes != static_cast<u64>(blocks_x) * blocks_y * block_bytes) {
            AETHER_LOG_ERROR("D3D12", "CreateCompressedTexture: a mip level does not match its size or format");
            throw std::invalid_argument("CreateCompressedTexture: a mip level does not match its size or format");
        }
        levels.push_back({mip.data, w, h, blocks_x * block_bytes, blocks_y});
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (!SupportsCompressedTexture(format)) return {}; // ASTC: D3D12 has none
    if (sampled_texture_count_ >= kMaxUserBindlessTextures || !bindless_texture_heap_->CanAllocate()) {
        AETHER_LOG_ERROR("D3D12", "CreateCompressedTexture exhausted the bindless texture table (capacity=%u real textures)", kMaxUserBindlessTextures);
        return {};
    }
    const DXGI_FORMAT dxgi = format == CompressedTextureFormat::BC1 ? DXGI_FORMAT_BC1_UNORM
                             : format == CompressedTextureFormat::BC3 ? DXGI_FORMAT_BC3_UNORM
                                                                      : DXGI_FORMAT_BC7_UNORM;
    gfx::CommandList setup_cmd(device_);
    setup_cmd.Reset();
    auto texture = std::make_unique<gfx::Texture>(device_, *bindless_texture_heap_, setup_cmd.Get(), dxgi, width, height, levels.data(),
                                                  static_cast<u32>(levels.size()));
    setup_cmd.Close();
    ID3D12CommandList* setup_lists[] = {setup_cmd.Get()};
    device_.WaitForFence(device_.Submit(setup_lists, 1));
    texture->ReleaseUploadStaging();
    const u32 index = texture->BindlessIndex();
    sampled_textures_.push_back(std::move(texture));
    ++sampled_texture_count_;
    return SampledTextureHandle{index};
}

bool D3D12Device::DestroyTexture(SampledTextureHandle texture) {
    if (!texture.IsValid() || texture.index < kFirstUserBindlessTextureIndex) return false;
    const usize descriptor_slot = texture.index - kFirstUserBindlessTextureIndex;
    if (descriptor_slot >= sampled_descriptor_indices_.size()) return false;
    const auto it = std::find_if(sampled_textures_.begin(), sampled_textures_.end(),
                                 [&](const std::unique_ptr<gfx::Texture>& t) { return t->BindlessIndex() == texture.index; });
    const bool owns_regular_texture = it != sampled_textures_.end();
    const bool owns_float_texture = sampled_float_textures_[descriptor_slot] != nullptr;
    if (!owns_regular_texture && !owns_float_texture) return false;

    device_.WithQueuesIdle([&] {
        // Keep the bindless table fully initialized and make stale handles
        // resolve to the white fallback before releasing the resource.
        D3D12_SHADER_RESOURCE_VIEW_DESC fallback_srv{};
        fallback_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        fallback_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        fallback_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        fallback_srv.Texture2D.MipLevels = 1;
        device_.Handle()->CreateShaderResourceView(dummy_texture_->Handle(), &fallback_srv,
                                                    bindless_texture_heap_->CPUHandle(texture.index));
        const u32 compute_index = sampled_descriptor_indices_[descriptor_slot];
        if (compute_index != DescriptorHeap::kInvalidIndex) {
            compute_storage_heap_->Free(compute_index);
            sampled_descriptor_indices_[descriptor_slot] = DescriptorHeap::kInvalidIndex;
        }
        sampled_float_textures_[descriptor_slot].Reset();
        bindless_texture_heap_->Free(texture.index);
        if (owns_regular_texture) sampled_textures_.erase(it);
        if (sampled_texture_count_ > 0) --sampled_texture_count_;
    });
    return true;
}
SampledTextureHandle D3D12Device::CreateR32FloatTexture(u32 width, u32 height, const f32* values) {
    if (width == 0 || height == 0 || !values ||
        width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        static_cast<u64>(width) > std::numeric_limits<u64>::max() / sizeof(f32) / height) {
        throw std::invalid_argument("CreateR32FloatTexture requires a supported extent and float data");
    }
    if (sampled_texture_count_ >= kMaxUserBindlessTextures || !bindless_texture_heap_->CanAllocate() ||
        !compute_storage_heap_->CanAllocate()) {
        AETHER_LOG_ERROR("D3D12", "CreateR32FloatTexture exhausted sampled-texture descriptors");
        return {};
    }

    const u64 row_bytes = static_cast<u64>(width) * sizeof(f32);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    AETHER_D3D_CHECK(device_.Handle()->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&texture)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 row_size = 0, upload_size = 0;
    device_.Handle()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &upload_size);
    std::vector<u8> upload(static_cast<usize>(upload_size), 0);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(upload.data() + footprint.Offset + static_cast<usize>(y) * footprint.Footprint.RowPitch,
                    reinterpret_cast<const u8*>(values) + static_cast<usize>(y) * row_bytes,
                    static_cast<usize>(row_size));
    }
    gfx::Buffer staging(device_, upload_size, gfx::BufferKind::Upload);
    staging.Update(upload.data(), upload_size);
    gfx::CommandList copy(device_);
    copy.Reset();
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = staging.Handle();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    copy->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER to_read{};
    to_read.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_read.Transition.pResource = texture.Get();
    to_read.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_read.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    to_read.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    copy->ResourceBarrier(1, &to_read);
    copy.Close();
    ID3D12CommandList* lists[] = {copy.Get()};
    device_.WaitForFence(device_.Submit(lists, 1));

    const u32 bindless_index = bindless_texture_heap_->Allocate();
    if (bindless_index == DescriptorHeap::kInvalidIndex) {
        throw std::length_error("D3D12 bindless texture descriptor allocation failed");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    u32 compute_index = DescriptorHeap::kInvalidIndex;
    device_.WithQueuesIdle([&] {
        compute_index = compute_storage_heap_->Allocate();
        if (compute_index == DescriptorHeap::kInvalidIndex) {
            // The bindless slot still contains its initialized fallback view.
            // Return the slot before the temporary resource owner is released.
            bindless_texture_heap_->Free(bindless_index);
            throw std::length_error("Cannot create R32F texture: compute descriptor allocation failed");
        }
        device_.Handle()->CreateShaderResourceView(texture.Get(), &srv, bindless_texture_heap_->CPUHandle(bindless_index));
        device_.Handle()->CopyDescriptorsSimple(1, compute_storage_heap_->CPUHandle(compute_index),
                                                 bindless_texture_heap_->CPUHandle(bindless_index),
                                                 D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    });
    const usize descriptor_slot = bindless_index - kFirstUserBindlessTextureIndex;
    sampled_descriptor_indices_[descriptor_slot] = compute_index;
    sampled_float_textures_[descriptor_slot] = std::move(texture);
    ++sampled_texture_count_;
    return SampledTextureHandle{bindless_index};
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
    if (width == 0 || height == 0 || (width == Width() && height == Height())) {
        return;
    }
    swap_chain_.Resize(width, height);
    for (u32 i = 0; i < swap_chain_.BufferCount(); ++i) {
        device_.UpdateTexture(handles_[i], swap_chain_.BackBuffer(i), swap_chain_.BackBufferRTV(i));
    }
    used_before_.assign(swap_chain_.BufferCount(), false);
    CreateDepthBuffer(swap_chain_.Width(), swap_chain_.Height());
}

bool D3D12SwapChain::ReadBack(std::vector<u8>& rgba8) {
    if (swap_chain_.Format() != DXGI_FORMAT_R8G8B8A8_UNORM || Width() == 0 || Height() == 0 ||
        swap_chain_.BufferCount() == 0) {
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
    : device_(device), type_(type), cmd_(device.Native(), type) {}

D3D12CommandList::~D3D12CommandList() {
    if (IsSubmissionInFlight()) {
        try {
            if (IsComputeList()) {
                device_.WaitForComputeFence(last_submitted_fence_);
            } else {
                device_.WaitForFence(last_submitted_fence_);
            }
        } catch (...) {
            AETHER_LOG_ERROR("D3D12", "Could not wait for command-list submission; deferring its allocator to device teardown");
            device_.DeferCommandAllocator(cmd_.ReleaseAllocator());
        }
    }
}

void D3D12CommandList::Reset() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    if (IsSubmissionInFlight()) {
        AETHER_LOG_ERROR("D3D12", "Reset requires the command list's last submission fence to complete");
        return;
    }
    cmd_.Reset();
    last_submitted_fence_ = 0;
    recording_ = true;
    has_closed_recording_ = false;
    render_pass_active_ = false;
    active_backbuffer_ = nullptr;
    graphics_pipeline_bound_ = false;
    graphics_bindless_enabled_ = false;
    graphics_bindless_bound_ = false;
    graphics_uses_vertex_buffer_ = false;
    graphics_vertex_stride_bytes_ = 0;
    vertex_buffer_bound_ = false;
    bound_vertex_count_ = 0;
    index_buffer_bound_ = false;
    bound_index_count_ = 0;
    graphics_push_constant_size_bytes_ = 0;
    compute_pipeline_bound_ = false;
    compute_storage_buffer_count_ = 0;
    compute_sampled_texture_count_ = 0;
    compute_constant_buffer_count_ = 0;
    compute_storage_bound_.clear();
    compute_sampled_texture_bound_.clear();
    compute_constant_buffer_bound_.clear();
}

void D3D12CommandList::Close() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "Close received a command list that is not recording");
        return;
    }
    if (render_pass_active_) {
        AETHER_LOG_ERROR("D3D12", "Close requires EndRenderPass before closing the command list");
        return;
    }
    cmd_.Close();
    recording_ = false;
    has_closed_recording_ = true;
}

void D3D12CommandList::TransitionTexture(TextureHandle texture, ResourceState before, ResourceState after) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "TransitionTexture requires a recording command list");
        return;
    }
    if (IsComputeList() || !device_.IsTextureValid(texture) || !IsValidResourceState(before) ||
        !IsValidResourceState(after)) {
        AETHER_LOG_ERROR("D3D12", "TransitionTexture requires a graphics list and valid texture/resource states");
        return;
    }
    const TextureRecord& record = device_.GetTexture(texture);
    D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(record.resource, ToD3D12(before), ToD3D12(after));
    cmd_.Get()->ResourceBarrier(1, &barrier);
}

void D3D12CommandList::ClearRenderTarget(TextureHandle texture, const ClearColor& color) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "ClearRenderTarget requires a recording command list");
        return;
    }
    if (IsComputeList() || !IsFiniteClearColor(color) || !device_.IsTextureValid(texture) ||
        device_.GetTexture(texture).rtv.ptr == 0) {
        AETHER_LOG_ERROR("D3D12", "ClearRenderTarget requires a graphics list and valid texture/color");
        return;
    }
    const TextureRecord& record = device_.GetTexture(texture);
    const f32 c[4] = {color.r, color.g, color.b, color.a};
    cmd_.Get()->ClearRenderTargetView(record.rtv, c, 0, nullptr);
}

void D3D12CommandList::SetViewportAndScissor(const Viewport& viewport, const Rect& scissor) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "SetViewportAndScissor requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !std::isfinite(viewport.x) || !std::isfinite(viewport.y) ||
        !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
        !std::isfinite(viewport.min_depth) || !std::isfinite(viewport.max_depth) || viewport.width <= 0.0f ||
        viewport.height <= 0.0f || viewport.min_depth < 0.0f || viewport.max_depth > 1.0f ||
        viewport.min_depth > viewport.max_depth || scissor.right <= scissor.left || scissor.bottom <= scissor.top) {
        AETHER_LOG_ERROR("D3D12", "SetViewportAndScissor requires an active pass and valid viewport/scissor bounds");
        return;
    }
    D3D12_VIEWPORT vp{viewport.x,    viewport.y,        viewport.width,
                       viewport.height, viewport.min_depth, viewport.max_depth};
    D3D12_RECT rect{scissor.left, scissor.top, scissor.right, scissor.bottom};
    cmd_.Get()->RSSetViewports(1, &vp);
    cmd_.Get()->RSSetScissorRects(1, &rect);
}

void D3D12CommandList::BeginRenderPass(ISwapChain& swap_chain, const ClearColor& clear_color) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BeginRenderPass requires a recording command list");
        return;
    }
    if (!IsFiniteClearColor(clear_color)) {
        AETHER_LOG_ERROR("D3D12", "BeginRenderPass requires a finite clear color");
        return;
    }
    auto* d3d_swap_ptr = dynamic_cast<D3D12SwapChain*>(&swap_chain);
    if (IsComputeList() || render_pass_active_ || !d3d_swap_ptr || !d3d_swap_ptr->BelongsTo(device_)) {
        AETHER_LOG_ERROR("D3D12", "BeginRenderPass requires a non-nested D3D12 swap chain from this device");
        return;
    }
    auto& d3d_swap = *d3d_swap_ptr;
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
    render_pass_active_ = true;
}

void D3D12CommandList::EndRenderPass() {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "EndRenderPass requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !active_backbuffer_) {
        AETHER_LOG_ERROR("D3D12", "EndRenderPass requires an active render pass");
        return;
    }
    D3D12_RESOURCE_BARRIER to_present =
        TransitionBarrier(active_backbuffer_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    cmd_.Get()->ResourceBarrier(1, &to_present);
    active_backbuffer_ = nullptr;
    render_pass_active_ = false;
    graphics_pipeline_bound_ = false;
    graphics_bindless_enabled_ = false;
    graphics_bindless_bound_ = false;
    graphics_uses_vertex_buffer_ = false;
    vertex_buffer_bound_ = false;
    index_buffer_bound_ = false;
    graphics_push_constant_size_bytes_ = 0;
}

void D3D12CommandList::BindPipeline(PipelineHandle pipeline) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindPipeline requires a recording command list");
        return;
    }
    if (!render_pass_active_) {
        AETHER_LOG_ERROR("D3D12", "BindPipeline requires an active render pass");
        graphics_pipeline_bound_ = false;
        graphics_bindless_enabled_ = false;
        graphics_bindless_bound_ = false;
        graphics_uses_vertex_buffer_ = false;
        vertex_buffer_bound_ = false;
        index_buffer_bound_ = false;
        graphics_push_constant_size_bytes_ = 0;
        return;
    }
    if (!pipeline.IsValid() || pipeline.index >= device_.PipelineCount()) {
        AETHER_LOG_ERROR("D3D12", "BindPipeline received an invalid graphics-pipeline handle");
        graphics_pipeline_bound_ = false;
        graphics_bindless_enabled_ = false;
        graphics_bindless_bound_ = false;
        graphics_uses_vertex_buffer_ = false;
        vertex_buffer_bound_ = false;
        index_buffer_bound_ = false;
        graphics_push_constant_size_bytes_ = 0;
        return;
    }
    const PipelineRecord& record = device_.GetPipeline(pipeline);
    cmd_.Get()->SetGraphicsRootSignature(record.root_signature.Get());
    cmd_.Get()->SetPipelineState(record.pso.Get());
    cmd_.Get()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    frame_constants_root_index_ = record.frame_constants_root_index;
    graphics_pipeline_bound_ = true;
    graphics_push_constant_size_bytes_ = record.push_constant_size_bytes;
    graphics_bindless_enabled_ = record.enable_bindless_textures;
    graphics_bindless_bound_ = false;
    graphics_uses_vertex_buffer_ = record.use_vertex_buffer;
    graphics_vertex_stride_bytes_ = record.vertex_stride_bytes;
    vertex_buffer_bound_ = false;
    index_buffer_bound_ = false;
}

void D3D12CommandList::SetPushConstants(const void* data, u32 size_bytes) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "SetPushConstants requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !data || size_bytes == 0 || size_bytes % 4 != 0 ||
        size_bytes > graphics_push_constant_size_bytes_) {
        AETHER_LOG_ERROR("D3D12", "SetPushConstants requires a valid pipeline and data within its aligned push-constant range");
        return;
    }
    cmd_.Get()->SetGraphicsRoot32BitConstants(0, (size_bytes + 3) / 4, data, 0);
}

void D3D12CommandList::Draw(u32 vertex_count) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "Draw requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ ||
        (graphics_uses_vertex_buffer_ && (!vertex_buffer_bound_ || vertex_count > bound_vertex_count_)) ||
        (graphics_bindless_enabled_ && !graphics_bindless_bound_)) {
        AETHER_LOG_ERROR("D3D12", "Draw requires a valid bound graphics pipeline");
        return;
    }
    cmd_.Get()->DrawInstanced(vertex_count, 1, 0, 0);
}

void D3D12CommandList::BindVertexBuffer(BufferHandle buffer, u32 stride_bytes) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindVertexBuffer requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_uses_vertex_buffer_ ||
        !buffer.IsValid() || buffer.index >= device_.BufferCount() || device_.IsStorageBuffer(buffer) ||
        device_.IsConstantBuffer(buffer) || !device_.IsVertexBuffer(buffer) ||
        stride_bytes != graphics_vertex_stride_bytes_) {
        AETHER_LOG_ERROR("D3D12", "BindVertexBuffer received an invalid handle or a stride that does not match the pipeline");
        return;
    }
    const gfx::Buffer& buf = device_.GetBuffer(buffer);
    if (buf.Size() % stride_bytes != 0) {
        AETHER_LOG_ERROR("D3D12", "BindVertexBuffer size must contain whole vertices for the pipeline stride");
        return;
    }
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = buf.GPUAddress();
    vbv.SizeInBytes = static_cast<UINT>(buf.Size());
    vbv.StrideInBytes = stride_bytes;
    cmd_.Get()->IASetVertexBuffers(0, 1, &vbv);
    vertex_buffer_bound_ = true;
    bound_vertex_count_ = buf.Size() / stride_bytes;
}

void D3D12CommandList::BindIndexBuffer(BufferHandle buffer, IndexFormat format) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindIndexBuffer requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_uses_vertex_buffer_ ||
        (format != IndexFormat::UInt16 && format != IndexFormat::UInt32) ||
        !buffer.IsValid() || buffer.index >= device_.BufferCount() || device_.IsStorageBuffer(buffer) ||
        device_.IsConstantBuffer(buffer) || !device_.IsIndexBuffer(buffer, format)) {
        AETHER_LOG_ERROR("D3D12", "BindIndexBuffer received an invalid format, state, or buffer handle");
        return;
    }
    const gfx::Buffer& buf = device_.GetBuffer(buffer);
    const u32 index_size_bytes = format == IndexFormat::UInt16 ? 2u : 4u;
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = buf.GPUAddress();
    ibv.SizeInBytes = static_cast<UINT>(buf.Size());
    ibv.Format = format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    cmd_.Get()->IASetIndexBuffer(&ibv);
    index_buffer_bound_ = true;
    bound_index_count_ = buf.Size() / index_size_bytes;
}

void D3D12CommandList::DrawIndexed(u32 index_count) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "DrawIndexed requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_uses_vertex_buffer_ ||
        !vertex_buffer_bound_ || !index_buffer_bound_ || index_count > bound_index_count_ ||
        (graphics_bindless_enabled_ && !graphics_bindless_bound_)) {
        AETHER_LOG_ERROR("D3D12", "DrawIndexed requires a valid bound graphics pipeline");
        return;
    }
    cmd_.Get()->DrawIndexedInstanced(index_count, 1, 0, 0, 0);
}

void D3D12CommandList::BindFrameConstants(BufferHandle buffer) {
    if (frame_constants_root_index_ == ~0u || !buffer.IsValid()) {
        AETHER_LOG_ERROR("D3D12", "BindFrameConstants needs a pipeline made with frame_constants_size_bytes and a valid buffer");
        return;
    }
    cmd_.Get()->SetGraphicsRootConstantBufferView(frame_constants_root_index_, device_.GetBuffer(buffer).GPUAddress());
}
void D3D12CommandList::BindComputePipeline(ComputePipelineHandle pipeline) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindComputePipeline requires a recording command list");
        return;
    }
    if (!IsComputeList() || !pipeline.IsValid() || pipeline.index >= device_.ComputePipelineCount()) {
        AETHER_LOG_ERROR("D3D12", "BindComputePipeline requires a compute command list and valid pipeline handle");
        compute_pipeline_bound_ = false;
        compute_storage_buffer_count_ = compute_sampled_texture_count_ = compute_constant_buffer_count_ = 0;
        compute_storage_bound_.clear();
        compute_sampled_texture_bound_.clear();
        compute_constant_buffer_bound_.clear();
        return;
    }
    const ComputePipelineRecord& record = device_.GetComputePipeline(pipeline);
    cmd_.Get()->SetComputeRootSignature(record.root_signature.Get());
    cmd_.Get()->SetPipelineState(record.pso.Get());
    compute_storage_buffer_count_ = record.storage_buffer_count;
    compute_sampled_texture_count_ = record.sampled_texture_count;
    compute_constant_buffer_count_ = record.constant_buffer_count;
    compute_pipeline_bound_ = true;
    compute_storage_bound_.assign(record.storage_buffer_count, false);
    compute_sampled_texture_bound_.assign(record.sampled_texture_count, false);
    compute_constant_buffer_bound_.assign(record.constant_buffer_count, false);
}

void D3D12CommandList::BindComputeConstantBuffer(u32 slot, BufferHandle buffer) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindComputeConstantBuffer requires a recording command list");
        return;
    }
    if (!IsComputeList() || !compute_pipeline_bound_ || slot >= compute_constant_buffer_count_ || !buffer.IsValid() ||
        buffer.index >= device_.BufferCount() || !device_.IsConstantBuffer(buffer)) {
        AETHER_LOG_ERROR("D3D12", "Compute constant-buffer slot or handle is invalid");
        return;
    }
    const gfx::Buffer& resource = device_.GetBuffer(buffer);
    cmd_.Get()->SetComputeRootConstantBufferView(compute_storage_buffer_count_ + compute_sampled_texture_count_ + slot, resource.GPUAddress());
    compute_constant_buffer_bound_[slot] = true;
}

void D3D12CommandList::BindComputeStorageBuffer(u32 slot, BufferHandle buffer) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindComputeStorageBuffer requires a recording command list");
        return;
    }
    if (!IsComputeList() || !compute_pipeline_bound_ || slot >= compute_storage_buffer_count_ || !buffer.IsValid() ||
        buffer.index >= device_.BufferCount()) {
        AETHER_LOG_ERROR("D3D12", "Compute storage-buffer slot or handle is invalid");
        return;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE descriptor = device_.ComputeStorageDescriptor(buffer);
    if (descriptor.ptr == 0) {
        AETHER_LOG_ERROR("D3D12", "Compute storage-buffer handle is not a storage buffer");
        return;
    }
    ID3D12DescriptorHeap* heaps[] = {device_.ComputeStorageHeap()};
    cmd_.Get()->SetDescriptorHeaps(1, heaps);
    cmd_.Get()->SetComputeRootDescriptorTable(slot, descriptor);
    compute_storage_bound_[slot] = true;
}

void D3D12CommandList::BindComputeSampledTexture(u32 slot, SampledTextureHandle texture) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindComputeSampledTexture requires a recording command list");
        return;
    }
    if (!IsComputeList() || !compute_pipeline_bound_ || slot >= compute_sampled_texture_count_ || !texture.IsValid()) {
        AETHER_LOG_ERROR("D3D12", "Compute sampled-texture slot or handle is invalid");
        return;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE descriptor = device_.ComputeSampledDescriptor(texture);
    if (descriptor.ptr == 0) {
        AETHER_LOG_ERROR("D3D12", "Compute sampled-texture handle is invalid");
        return;
    }
    ID3D12DescriptorHeap* heaps[] = {device_.ComputeStorageHeap()};
    cmd_.Get()->SetDescriptorHeaps(1, heaps);
    cmd_.Get()->SetComputeRootDescriptorTable(compute_storage_buffer_count_ + slot, descriptor);
    compute_sampled_texture_bound_[slot] = true;
}

void D3D12CommandList::ComputeBufferBarrier(BufferHandle buffer) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "ComputeBufferBarrier requires a recording command list");
        return;
    }
    if (!IsComputeList() || !buffer.IsValid() || buffer.index >= device_.BufferCount() ||
        !device_.IsStorageBuffer(buffer)) {
        AETHER_LOG_ERROR("D3D12", "ComputeBufferBarrier requires a compute command list and storage buffer");
        return;
    }
    const gfx::Buffer& resource = device_.GetBuffer(buffer);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource.Handle();
    cmd_.Get()->ResourceBarrier(1, &barrier);
}

void D3D12CommandList::Dispatch(u32 groups_x, u32 groups_y, u32 groups_z) {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "Dispatch requires a recording command list");
        return;
    }
    constexpr u32 kMaxDispatchGroupsPerDimension = 65535;
    if (groups_x == 0 || groups_y == 0 || groups_z == 0 || groups_x > kMaxDispatchGroupsPerDimension ||
        groups_y > kMaxDispatchGroupsPerDimension || groups_z > kMaxDispatchGroupsPerDimension) {
        AETHER_LOG_ERROR("D3D12", "Compute dispatch group counts are zero or exceed the D3D12 per-dimension limit");
        return;
    }
    if (!IsComputeList() || !compute_pipeline_bound_) {
        AETHER_LOG_ERROR("D3D12", "Dispatch requires a bound compute pipeline");
        return;
    }
    for (bool bound : compute_storage_bound_) {
        if (!bound) {
            AETHER_LOG_ERROR("D3D12", "Dispatch requires every compute storage buffer to be bound");
            return;
        }
    }
    for (bool bound : compute_sampled_texture_bound_) {
        if (!bound) {
            AETHER_LOG_ERROR("D3D12", "Dispatch requires every compute sampled texture to be bound");
            return;
        }
    }
    for (bool bound : compute_constant_buffer_bound_) {
        if (!bound) {
            AETHER_LOG_ERROR("D3D12", "Dispatch requires every compute constant buffer to be bound");
            return;
        }
    }
    cmd_.Get()->Dispatch(groups_x, groups_y, groups_z);
}

void D3D12CommandList::BindBindlessTextures() {
    if (!recording_) {
        AETHER_LOG_ERROR("D3D12", "BindBindlessTextures requires a recording command list");
        return;
    }
    if (!render_pass_active_ || !graphics_pipeline_bound_ || !graphics_bindless_enabled_) {
        AETHER_LOG_ERROR("D3D12", "BindBindlessTextures requires an active pass and a bindless graphics pipeline");
        return;
    }
    ID3D12DescriptorHeap* heaps[] = {device_.BindlessTextureHeap().Heap()};
    cmd_.Get()->SetDescriptorHeaps(1, heaps);
    cmd_.Get()->SetGraphicsRootDescriptorTable(1, device_.BindlessTextureHeap().GPUHandle(0));
    graphics_bindless_bound_ = true;
}

} // namespace aether::gfx::rhi::d3d12_backend
