#include "aether/gfx/device.h"

#include <d3d12sdklayers.h>

namespace aether::gfx {

namespace {

void ThrowIfFenceDeviceRemoved(ID3D12Device* device, ID3D12Fence* fence, const char* queue_name) {
    if (!device || !fence || fence->GetCompletedValue() != ~u64{0}) return;

    const HRESULT reason = device->GetDeviceRemovedReason();
    AETHER_LOG_FATAL("D3D12", "%s device was removed (reason=0x%08lX)", queue_name,
                     static_cast<unsigned long>(reason));
    throw std::runtime_error(std::string("D3D12 ") + queue_name + " device was removed");
}

ComPtr<IDXGIAdapter1> PickHardwareAdapter(IDXGIFactory6& factory) {
    ComPtr<IDXGIAdapter1> adapter;
    for (u32 i = 0; factory.EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue; // skip WARP; we want a real GPU
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) {
            AETHER_LOG_INFO("D3D12", "Selected adapter: %ls", desc.Description);
            return adapter;
        }
    }
    return nullptr; // caller falls back to WARP
}

} // namespace

Device::Device(bool enable_debug_layer) {
    bool debug_layer_active = false;
    if (enable_debug_layer) {
        ComPtr<ID3D12Debug> debug_controller;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
            debug_controller->EnableDebugLayer();
            debug_layer_active = true;
        } else {
            AETHER_LOG_WARN("D3D12", "D3D12 debug interface unavailable (Graphics Tools optional feature not "
                                      "installed?); continuing without it");
        }
    }

    // The DXGI debug factory flag additionally requires the DXGI debug
    // device (also part of the "Graphics Tools" optional Windows feature).
    // Fall back to a non-debug factory rather than failing outright if it's
    // missing, so the sandbox still runs on a machine without that feature.
    HRESULT hr = E_FAIL;
    if (debug_layer_active) {
        hr = CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            AETHER_LOG_WARN("D3D12", "DXGI debug factory unavailable (hr=0x%08lX); continuing without it",
                             static_cast<unsigned long>(hr));
        }
    }
    if (FAILED(hr)) {
        AETHER_D3D_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)));
    }

    ComPtr<IDXGIAdapter1> adapter = PickHardwareAdapter(*factory_.Get());
    if (adapter) {
        AETHER_D3D_CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)));
    } else {
        AETHER_LOG_WARN("D3D12", "No suitable hardware adapter found; falling back to WARP (software) device");
        ComPtr<IDXGIAdapter> warp_adapter;
        AETHER_D3D_CHECK(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter)));
        AETHER_D3D_CHECK(D3D12CreateDevice(warp_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)));
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    AETHER_D3D_CHECK(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)));

    AETHER_D3D_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AETHER_ASSERT(fence_event_ != nullptr);

    D3D12_COMMAND_QUEUE_DESC compute_queue_desc{};
    compute_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    AETHER_D3D_CHECK(device_->CreateCommandQueue(&compute_queue_desc, IID_PPV_ARGS(&compute_queue_)));

    AETHER_D3D_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&compute_fence_)));
    compute_fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AETHER_ASSERT(compute_fence_event_ != nullptr);
}

Device::~Device() {
    try {
        WaitForFence(next_fence_value_ - 1);
        WaitForComputeFence(next_compute_fence_value_ - 1);
    } catch (const std::exception& error) {
        // Destructors must stay noexcept even after device removal.
        AETHER_LOG_WARN("D3D12", "Could not drain queues during device teardown: %s", error.what());
    }
    if (fence_event_) {
        CloseHandle(fence_event_);
    }
    if (compute_fence_event_) {
        CloseHandle(compute_fence_event_);
    }
}

u64 Device::Submit(ID3D12CommandList* const* lists, u32 count) {
    queue_->ExecuteCommandLists(count, lists);
    u64 value = next_fence_value_++;
    AETHER_D3D_CHECK(queue_->Signal(fence_.Get(), value));
    return value;
}

void Device::WaitForFence(u64 fence_value) {
    if (fence_value >= next_fence_value_) {
        AETHER_LOG_ERROR("D3D12", "Cannot wait for graphics fence %llu: latest submitted value is %llu",
                         static_cast<unsigned long long>(fence_value),
                         static_cast<unsigned long long>(next_fence_value_ - 1));
        return;
    }
    ThrowIfFenceDeviceRemoved(device_.Get(), fence_.Get(), "graphics queue");
    if (IsFenceComplete(fence_value)) {
        return;
    }
    AETHER_D3D_CHECK(fence_->SetEventOnCompletion(fence_value, fence_event_));
    WaitForSingleObject(fence_event_, INFINITE);
    ThrowIfFenceDeviceRemoved(device_.Get(), fence_.Get(), "graphics queue");
}

u64 Device::SubmitCompute(ID3D12CommandList* const* lists, u32 count) {
    compute_queue_->ExecuteCommandLists(count, lists);
    u64 value = next_compute_fence_value_++;
    AETHER_D3D_CHECK(compute_queue_->Signal(compute_fence_.Get(), value));
    return value;
}

void Device::WaitForComputeFence(u64 fence_value) {
    if (fence_value >= next_compute_fence_value_) {
        AETHER_LOG_ERROR("D3D12", "Cannot wait for compute fence %llu: latest submitted value is %llu",
                         static_cast<unsigned long long>(fence_value),
                         static_cast<unsigned long long>(next_compute_fence_value_ - 1));
        return;
    }
    ThrowIfFenceDeviceRemoved(device_.Get(), compute_fence_.Get(), "compute queue");
    if (IsComputeFenceComplete(fence_value)) {
        return;
    }
    AETHER_D3D_CHECK(compute_fence_->SetEventOnCompletion(fence_value, compute_fence_event_));
    WaitForSingleObject(compute_fence_event_, INFINITE);
    ThrowIfFenceDeviceRemoved(device_.Get(), compute_fence_.Get(), "compute queue");
}

void Device::ComputeQueueWaitOnGraphics(u64 graphics_fence_value) {
    if (graphics_fence_value == 0) return;
    if (graphics_fence_value >= next_fence_value_) {
        AETHER_LOG_ERROR("D3D12", "Cannot queue compute wait for unsubmitted graphics fence %llu",
                         static_cast<unsigned long long>(graphics_fence_value));
        return;
    }
    AETHER_D3D_CHECK(compute_queue_->Wait(fence_.Get(), graphics_fence_value));
}

void Device::GraphicsQueueWaitOnCompute(u64 compute_fence_value) {
    if (compute_fence_value == 0) return;
    if (compute_fence_value >= next_compute_fence_value_) {
        AETHER_LOG_ERROR("D3D12", "Cannot queue graphics wait for unsubmitted compute fence %llu",
                         static_cast<unsigned long long>(compute_fence_value));
        return;
    }
    AETHER_D3D_CHECK(queue_->Wait(compute_fence_.Get(), compute_fence_value));
}

} // namespace aether::gfx
