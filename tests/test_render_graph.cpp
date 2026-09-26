#include "aether/gfx/command_list.h"
#include "aether/gfx/device.h"
#include "aether/gfx/render_graph.h"
#include "test_framework.h"

using namespace aether;
using namespace aether::gfx;

AETHER_TEST(RenderGraph_TransientTextureRecreatesInPlaceUnderSameHandle) {
    Device device(/*enable_debug_layer=*/false);
    RenderGraph graph(device);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 64;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;

    RenderGraph::ResourceHandle handle =
        graph.CreateTransientTexture(desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "TestDepth");
    ID3D12Resource* first_resource = graph.GetResource(handle);
    AETHER_CHECK(first_resource != nullptr);

    // A DSV can be created for it without crashing.
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = graph.GetOrCreateDSV(handle);
    AETHER_CHECK(dsv.ptr != 0);

    // "Resize": call again with the same name. Same handle, new resource.
    desc.Width = 128;
    desc.Height = 128;
    RenderGraph::ResourceHandle second_handle =
        graph.CreateTransientTexture(desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "TestDepth");
    AETHER_CHECK(second_handle == handle);
    AETHER_CHECK(graph.GetResource(handle) != first_resource);

    // The old DSV was freed; a fresh one for the new resource is created
    // without crashing or aliasing the freed descriptor slot incorrectly.
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_after_resize = graph.GetOrCreateDSV(handle);
    AETHER_CHECK(dsv_after_resize.ptr != 0);
}

AETHER_TEST(RenderGraph_GraphicsAndComputePassesRecordIntoSeparateCommandLists) {
    Device device(/*enable_debug_layer=*/false);
    RenderGraph graph(device);
    AETHER_CHECK(!graph.HasComputePasses());

    CommandList graphics_cmd(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    CommandList compute_cmd(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    graphics_cmd.Reset();
    compute_cmd.Reset();

    ID3D12GraphicsCommandList* seen_by_graphics_pass = nullptr;
    ID3D12GraphicsCommandList* seen_by_compute_pass = nullptr;

    graph.AddPass(
        "GraphicsPass", [](RenderGraph::PassBuilder&) {},
        [&](ID3D12GraphicsCommandList* cl) { seen_by_graphics_pass = cl; }, QueueType::Graphics);

    graph.AddPass(
        "ComputePass", [](RenderGraph::PassBuilder&) {},
        [&](ID3D12GraphicsCommandList* cl) { seen_by_compute_pass = cl; }, QueueType::Compute);

    AETHER_CHECK(graph.HasComputePasses());

    graph.Execute(graphics_cmd.Get(), compute_cmd.Get());

    AETHER_CHECK(seen_by_graphics_pass == graphics_cmd.Get());
    AETHER_CHECK(seen_by_compute_pass == compute_cmd.Get());
    AETHER_CHECK(seen_by_graphics_pass != seen_by_compute_pass);

    graphics_cmd.Close();
    compute_cmd.Close();
}

AETHER_TEST(Device_ComputeQueueFenceSignalsAndCompletes) {
    Device device(/*enable_debug_layer=*/false);

    u64 value = device.SubmitCompute(nullptr, 0);
    AETHER_CHECK(!device.IsComputeFenceComplete(value + 1));
    device.WaitForComputeFence(value);
    AETHER_CHECK(device.IsComputeFenceComplete(value));
}

AETHER_TEST(Device_CrossQueueGpuWaitDoesNotDeadlock) {
    Device device(/*enable_debug_layer=*/false);

    // Compute queue does some (empty) work; graphics queue is told to wait
    // for it (a real GPU-side wait, not a CPU one) before doing its own
    // (empty) work. If the wait is wired correctly this completes promptly;
    // if it were backwards or self-referential it would hang here.
    u64 compute_value = device.SubmitCompute(nullptr, 0);
    device.GraphicsQueueWaitOnCompute(compute_value);
    u64 graphics_value = device.Submit(nullptr, 0);
    device.WaitForFence(graphics_value);

    AETHER_CHECK(device.IsComputeFenceComplete(compute_value));
    AETHER_CHECK(device.IsFenceComplete(graphics_value));
}
