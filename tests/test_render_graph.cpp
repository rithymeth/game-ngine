#include "aether/gfx/command_list.h"
#include "aether/gfx/device.h"
#include "aether/gfx/render_graph.h"
#include "test_framework.h"

#include <string>
#include <vector>

using namespace aether;
using namespace aether::gfx;

namespace {

D3D12_RESOURCE_DESC MakeDepthDesc(u32 width, u32 height) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return desc;
}

D3D12_CLEAR_VALUE MakeDepthClear() {
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    return clear;
}

// Allocation is deferred to Execute()'s internal Compile() step (needed so
// lifetime analysis can see the full pass list) — a transient resource only
// actually gets a GPU allocation once at least one pass has used it and
// Execute() has run. This helper does exactly that with a single throwaway
// pass, mirroring how a real caller would exercise the resource.
void ExecuteOnceUsingResource(RenderGraph& graph, RenderGraph::ResourceHandle handle, D3D12_RESOURCE_STATES state,
                               CommandList& cmd) {
    cmd.Reset();
    graph.AddPass(
        "UseResource", [&](RenderGraph::PassBuilder& builder) { builder.Write(handle, state); },
        [](ID3D12GraphicsCommandList*) {});
    graph.Execute(cmd.Get());
    graph.Reset();
    cmd.Close();
}

} // namespace

AETHER_TEST(RenderGraph_TransientTextureRecreatesInPlaceUnderSameHandle) {
    Device device(/*enable_debug_layer=*/false);
    RenderGraph graph(device);
    CommandList cmd(device);

    D3D12_CLEAR_VALUE clear = MakeDepthClear();

    RenderGraph::ResourceHandle handle =
        graph.CreateTransientTexture(MakeDepthDesc(64, 64), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "TestDepth");
    ExecuteOnceUsingResource(graph, handle, D3D12_RESOURCE_STATE_DEPTH_WRITE, cmd);

    ID3D12Resource* first_resource = graph.GetResource(handle);
    AETHER_CHECK(first_resource != nullptr);

    // A DSV can be created for it without crashing.
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = graph.GetOrCreateDSV(handle);
    AETHER_CHECK(dsv.ptr != 0);

    // "Resize": call again with the same name. Same handle, new resource.
    // (Not asserting the resource *pointer* changed: the old one is freed
    // before the new one is created, and the allocator is free to hand back
    // the same address — a real, unreliable false-negative risk for pointer-
    // identity comparisons here. Checking the actual dimensions changed is
    // what we really mean by "did the resize take effect".)
    RenderGraph::ResourceHandle second_handle =
        graph.CreateTransientTexture(MakeDepthDesc(128, 128), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "TestDepth");
    AETHER_CHECK(second_handle == handle);

    ExecuteOnceUsingResource(graph, handle, D3D12_RESOURCE_STATE_DEPTH_WRITE, cmd);
    D3D12_RESOURCE_DESC resized_desc = graph.GetResource(handle)->GetDesc();
    AETHER_CHECK(resized_desc.Width == 128);
    AETHER_CHECK(resized_desc.Height == 128);

    // The old DSV was freed; a fresh one for the new resource is created
    // without crashing or aliasing the freed descriptor slot incorrectly.
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_after_resize = graph.GetOrCreateDSV(handle);
    AETHER_CHECK(dsv_after_resize.ptr != 0);
}

AETHER_TEST(RenderGraph_NonOverlappingTransientsShareHeapMemory) {
    Device device(/*enable_debug_layer=*/false);
    RenderGraph graph(device);
    CommandList cmd(device);
    cmd.Reset();

    D3D12_CLEAR_VALUE clear = MakeDepthClear();
    RenderGraph::ResourceHandle a =
        graph.CreateTransientTexture(MakeDepthDesc(256, 256), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "AliasA");
    RenderGraph::ResourceHandle b =
        graph.CreateTransientTexture(MakeDepthDesc(256, 256), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "AliasB");

    // Non-overlapping lifetimes: pass 1 only uses A, pass 2 only uses B.
    graph.AddPass(
        "UseA", [&](RenderGraph::PassBuilder& builder) { builder.Write(a, D3D12_RESOURCE_STATE_DEPTH_WRITE); },
        [](ID3D12GraphicsCommandList*) {});
    graph.AddPass(
        "UseB", [&](RenderGraph::PassBuilder& builder) { builder.Write(b, D3D12_RESOURCE_STATE_DEPTH_WRITE); },
        [](ID3D12GraphicsCommandList*) {});

    graph.Execute(cmd.Get());
    cmd.Close();

    ID3D12Resource* resource_a = graph.GetResource(a);
    ID3D12Resource* resource_b = graph.GetResource(b);
    AETHER_CHECK(resource_a != nullptr);
    AETHER_CHECK(resource_b != nullptr);
    AETHER_CHECK(resource_a != resource_b); // distinct resource objects...
    AETHER_CHECK(graph.ShareHeapAllocation(a, b)); // ...but sharing one heap allocation
}

AETHER_TEST(RenderGraph_OverlappingTransientsDoNotShareHeapMemory) {
    Device device(/*enable_debug_layer=*/false);
    RenderGraph graph(device);
    CommandList cmd(device);
    cmd.Reset();

    D3D12_CLEAR_VALUE clear = MakeDepthClear();
    RenderGraph::ResourceHandle a =
        graph.CreateTransientTexture(MakeDepthDesc(256, 256), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "OverlapA");
    RenderGraph::ResourceHandle b =
        graph.CreateTransientTexture(MakeDepthDesc(256, 256), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "OverlapB");

    // Overlapping lifetimes: a single pass uses both at once (e.g. a
    // multi-render-target pass), so they must NOT be allowed to alias.
    graph.AddPass(
        "UseBoth",
        [&](RenderGraph::PassBuilder& builder) {
            builder.Write(a, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            builder.Write(b, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        },
        [](ID3D12GraphicsCommandList*) {});

    graph.Execute(cmd.Get());
    cmd.Close();

    AETHER_CHECK(graph.GetResource(a) != nullptr);
    AETHER_CHECK(graph.GetResource(b) != nullptr);
    AETHER_CHECK(!graph.ShareHeapAllocation(a, b));
}

// Documents a real finding rather than a feature: passes execute in exactly
// registration order per queue, always — see ComputeExecutionOrder's comment
// for why a naive same-queue topological sort over Read/Write usages (no
// resource versioning) is mathematically incapable of doing anything else.
AETHER_TEST(RenderGraph_PassesExecuteInRegistrationOrder) {
    Device device(/*enable_debug_layer=*/false);
    RenderGraph graph(device);
    CommandList cmd(device);
    cmd.Reset();

    D3D12_CLEAR_VALUE clear = MakeDepthClear();
    RenderGraph::ResourceHandle resource =
        graph.CreateTransientTexture(MakeDepthDesc(64, 64), D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, "OrderTest");

    std::vector<std::string> execution_log;

    graph.AddPass(
        "Producer",
        [&](RenderGraph::PassBuilder& builder) { builder.Write(resource, D3D12_RESOURCE_STATE_DEPTH_WRITE); },
        [&](ID3D12GraphicsCommandList*) { execution_log.push_back("Producer"); });
    graph.AddPass(
        "Consumer", [&](RenderGraph::PassBuilder& builder) { builder.Read(resource, D3D12_RESOURCE_STATE_DEPTH_READ); },
        [&](ID3D12GraphicsCommandList*) { execution_log.push_back("Consumer"); });

    graph.Execute(cmd.Get());
    cmd.Close();

    AETHER_CHECK(execution_log.size() == 2);
    AETHER_CHECK(execution_log[0] == "Producer");
    AETHER_CHECK(execution_log[1] == "Consumer");
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
