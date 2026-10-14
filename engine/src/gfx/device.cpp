#include "aether/gfx/device.h"

#include <d3d12sdklayers.h>

namespace aether::gfx {

namespace {

void ValidateSubmissionLists(ID3D12CommandList* const* lists, u32 count, const char* queue_name) {
    if (count == 0) return;
    if (!lists) {
        AETHER_LOG_FATAL("D3D12", "%s submission has %u command lists but a null list array", queue_name, count);
        throw std::invalid_argument(std::string("D3D12 ") + queue_name + " submission list array is null");
    }
    for (u32 i = 0; i < count; ++i) {
        if (lists[i]) continue;
        AETHER_LOG_FATAL("D3D12", "%s submission contains a null command list at index %u", queue_name, i);
        throw std::invalid_argument(std::string("D3D12 ") + queue_name + " submission contains a null command list");
    }
}

HANDLE CreateFenceEvent(const char* queue_name) {
    HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle) return event_handle;

    const DWORD error = GetLastError();
    AETHER_LOG_FATAL("D3D12", "Could not create %s fence event (Win32 error=%lu)", queue_name,
                     static_cast<unsigned long>(error));
    throw std::runtime_error(std::string("Could not create D3D12 ") + queue_name + " fence event");
}

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
    fence_event_ = CreateFenceEvent("graphics");
    try {
        D3D12_COMMAND_QUEUE_DESC compute_queue_desc{};
        compute_queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        AETHER_D3D_CHECK(device_->CreateCommandQueue(&compute_queue_desc, IID_PPV_ARGS(&compute_queue_)));

        AETHER_D3D_CHECK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&compute_fence_)));
        compute_fence_event_ = CreateFenceEvent("compute");
    } catch (...) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
        if (compute_fence_event_) {
            CloseHandle(compute_fence_event_);
            compute_fence_event_ = nullptr;
        }
        throw;
    }
}

Device::~Device() {
    try {
        WaitForFence(next_fence_value_ - 1);
    } catch (const std::exception& error) {
        // Destructors must stay noexcept even after device removal. Continue
        // with the independent compute drain if graphics synchronization fails.
        AETHER_LOG_WARN("D3D12", "Could not drain graphics queue during device teardown: %s", error.what());
    }
    try {
        WaitForComputeFence(next_compute_fence_value_ - 1);
    } catch (const std::exception& error) {
        AETHER_LOG_WARN("D3D12", "Could not drain compute queue during device teardown: %s", error.what());
    }
    if (fence_event_) {
        CloseHandle(fence_event_);
    }
    if (compute_fence_event_) {
        CloseHandle(compute_fence_event_);
    }
}

u64 Device::Submit(ID3D12CommandList* const* lists, u32 count) {
    ValidateSubmissionLists(lists, count, "graphics");
    std::lock_guard<std::mutex> submit_lock(submit_mutex_);
    if (next_fence_value_ == ~u64{0}) {
        AETHER_LOG_FATAL("D3D12", "Graphics fence value space is exhausted; refusing queue submission");
        throw std::overflow_error("D3D12 graphics fence value space is exhausted");
    }
    queue_->ExecuteCommandLists(count, lists);
    const u64 value = next_fence_value_;
    AETHER_D3D_CHECK(queue_->Signal(fence_.Get(), value));
    ++next_fence_value_;
    return value;
}

void Device::WithQueuesIdle(const std::function<void()>& operation) {
    if (!operation) {
        throw std::invalid_argument("D3D12 idle-queue operation must not be empty");
    }

    for (;;) {
        u64 graphics_fence = 0;
        u64 compute_fence = 0;
        {
            std::lock_guard<std::mutex> submit_lock(submit_mutex_);
            graphics_fence = next_fence_value_ - 1;
        }
        {
            std::lock_guard<std::mutex> submit_lock(compute_submit_mutex_);
            compute_fence = next_compute_fence_value_ - 1;
        }

        WaitForFence(graphics_fence);
        WaitForComputeFence(compute_fence);

        std::scoped_lock queue_lock(submit_mutex_, compute_submit_mutex_);
        if (graphics_fence != next_fence_value_ - 1 || compute_fence != next_compute_fence_value_ - 1) {
            continue;
        }

        operation();
        return;
    }
}

void Device::WithComputeQueueIdle(const std::function<void()>& operation) {
    if (!operation) {
        throw std::invalid_argument("D3D12 idle-compute operation must not be empty");
    }

    for (;;) {
        u64 compute_fence = 0;
        {
            std::lock_guard<std::mutex> submit_lock(compute_submit_mutex_);
            compute_fence = next_compute_fence_value_ - 1;
        }

        WaitForComputeFence(compute_fence);

        std::lock_guard<std::mutex> submit_lock(compute_submit_mutex_);
        if (compute_fence != next_compute_fence_value_ - 1) {
            continue;
        }

        operation();
        return;
    }
}

void Device::WithGraphicsQueueIdle(const std::function<void()>& operation) {
    if (!operation) {
        throw std::invalid_argument("D3D12 idle-graphics operation must not be empty");
    }

    for (;;) {
        u64 graphics_fence = 0;
        {
            std::lock_guard<std::mutex> submit_lock(submit_mutex_);
            graphics_fence = next_fence_value_ - 1;
        }

        WaitForFence(graphics_fence);

        std::lock_guard<std::mutex> submit_lock(submit_mutex_);
        if (graphics_fence != next_fence_value_ - 1) {
            continue;
        }

        operation();
        return;
    }
}

void Device::WaitForFence(u64 fence_value) {
    std::lock_guard<std::mutex> wait_lock(fence_wait_mutex_);
    {
        std::lock_guard<std::mutex> submit_lock(submit_mutex_);
        if (fence_value >= next_fence_value_) {
            AETHER_LOG_FATAL("D3D12", "Cannot wait for graphics fence %llu: latest submitted value is %llu",
                             static_cast<unsigned long long>(fence_value),
                             static_cast<unsigned long long>(next_fence_value_ - 1));
            throw std::invalid_argument("D3D12 graphics fence value was not submitted");
        }
    }
    ThrowIfFenceDeviceRemoved(device_.Get(), fence_.Get(), "graphics queue");
    if (IsFenceComplete(fence_value)) {
        return;
    }
    AETHER_D3D_CHECK(fence_->SetEventOnCompletion(fence_value, fence_event_));
    const DWORD wait_result = WaitForSingleObject(fence_event_, INFINITE);
    const DWORD wait_error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if (wait_result != WAIT_OBJECT_0 && !IsFenceComplete(fence_value)) {
        ThrowIfFenceDeviceRemoved(device_.Get(), fence_.Get(), "graphics queue");
        AETHER_LOG_FATAL("D3D12", "Graphics fence event wait failed (result=%lu, Win32 error=%lu)",
                         static_cast<unsigned long>(wait_result), static_cast<unsigned long>(wait_error));
        throw std::runtime_error("D3D12 graphics fence event wait failed");
    }
    ThrowIfFenceDeviceRemoved(device_.Get(), fence_.Get(), "graphics queue");
}

u64 Device::SubmitCompute(ID3D12CommandList* const* lists, u32 count) {
    ValidateSubmissionLists(lists, count, "compute");
    std::lock_guard<std::mutex> submit_lock(compute_submit_mutex_);
    if (next_compute_fence_value_ == ~u64{0}) {
        AETHER_LOG_FATAL("D3D12", "Compute fence value space is exhausted; refusing queue submission");
        throw std::overflow_error("D3D12 compute fence value space is exhausted");
    }
    compute_queue_->ExecuteCommandLists(count, lists);
    const u64 value = next_compute_fence_value_;
    AETHER_D3D_CHECK(compute_queue_->Signal(compute_fence_.Get(), value));
    ++next_compute_fence_value_;
    return value;
}

void Device::WaitForComputeFence(u64 fence_value) {
    std::lock_guard<std::mutex> wait_lock(compute_fence_wait_mutex_);
    {
        std::lock_guard<std::mutex> submit_lock(compute_submit_mutex_);
        if (fence_value >= next_compute_fence_value_) {
            AETHER_LOG_FATAL("D3D12", "Cannot wait for compute fence %llu: latest submitted value is %llu",
                             static_cast<unsigned long long>(fence_value),
                             static_cast<unsigned long long>(next_compute_fence_value_ - 1));
            throw std::invalid_argument("D3D12 compute fence value was not submitted");
        }
    }
    ThrowIfFenceDeviceRemoved(device_.Get(), compute_fence_.Get(), "compute queue");
    if (IsComputeFenceComplete(fence_value)) {
        return;
    }
    AETHER_D3D_CHECK(compute_fence_->SetEventOnCompletion(fence_value, compute_fence_event_));
    const DWORD wait_result = WaitForSingleObject(compute_fence_event_, INFINITE);
    const DWORD wait_error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if (wait_result != WAIT_OBJECT_0 && !IsComputeFenceComplete(fence_value)) {
        ThrowIfFenceDeviceRemoved(device_.Get(), compute_fence_.Get(), "compute queue");
        AETHER_LOG_FATAL("D3D12", "Compute fence event wait failed (result=%lu, Win32 error=%lu)",
                         static_cast<unsigned long>(wait_result), static_cast<unsigned long>(wait_error));
        throw std::runtime_error("D3D12 compute fence event wait failed");
    }
    ThrowIfFenceDeviceRemoved(device_.Get(), compute_fence_.Get(), "compute queue");
}

void Device::ComputeQueueWaitOnGraphics(u64 graphics_fence_value) {
    if (graphics_fence_value == 0) return;
    std::scoped_lock queue_lock(submit_mutex_, compute_submit_mutex_);
    if (graphics_fence_value >= next_fence_value_) {
        AETHER_LOG_FATAL("D3D12", "Cannot queue compute wait for unsubmitted graphics fence %llu",
                         static_cast<unsigned long long>(graphics_fence_value));
        throw std::invalid_argument("D3D12 compute queue dependency references an unsubmitted graphics fence");
    }
    AETHER_D3D_CHECK(compute_queue_->Wait(fence_.Get(), graphics_fence_value));
}

void Device::GraphicsQueueWaitOnCompute(u64 compute_fence_value) {
    if (compute_fence_value == 0) return;
    std::scoped_lock queue_lock(submit_mutex_, compute_submit_mutex_);
    if (compute_fence_value >= next_compute_fence_value_) {
        AETHER_LOG_FATAL("D3D12", "Cannot queue graphics wait for unsubmitted compute fence %llu",
                         static_cast<unsigned long long>(compute_fence_value));
        throw std::invalid_argument("D3D12 graphics queue dependency references an unsubmitted compute fence");
    }
    AETHER_D3D_CHECK(queue_->Wait(compute_fence_.Get(), compute_fence_value));
}

} // namespace aether::gfx
