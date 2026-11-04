#include "aether/vfx/gpu.h"

#include <algorithm>
#include <exception>
#include <limits>

namespace aether::vfx {
namespace {

constexpr u32 kParticleStride = 96;
constexpr u32 kListElementStride = sizeof(u32);
constexpr u32 kModuleFloat4Stride = 16;

u32 GroupsFor(u32 count, u32 group_size) {
    const u64 groups = (static_cast<u64>(count) + group_size - 1u) / group_size;
    return static_cast<u32>(std::max<u64>(1u, groups));
}

void BindResources(gfx::rhi::ICommandList& cmd, gfx::rhi::ComputePipelineHandle pipeline,
                   gfx::rhi::BufferHandle particles, gfx::rhi::BufferHandle dead_list,
                   gfx::rhi::BufferHandle alive_list, gfx::rhi::BufferHandle frame_constants,
                   gfx::rhi::BufferHandle module_constants) {
    cmd.BindComputePipeline(pipeline);
    cmd.BindComputeStorageBuffer(0, particles);
    cmd.BindComputeStorageBuffer(1, dead_list);
    cmd.BindComputeStorageBuffer(2, alive_list);
    cmd.BindComputeConstantBuffer(0, frame_constants);
    cmd.BindComputeConstantBuffer(1, module_constants);
}

} // namespace

bool GpuEmitterRuntime::Initialize(gfx::rhi::IDevice& device, const Emitter& emitter, u64 seed,
                                  std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (initialized_) return fail("GPU emitter runtime is already initialized");
    if (emitter.settings.max_particles == 0) return fail("GPU emitters require at least one particle slot");

    shader_ = GenerateParticleShader(emitter);
    if (!shader_.ok) {
        return fail(shader_.errors.empty() ? "emitter stack cannot run on the GPU" : shader_.errors.front());
    }
    const u32 max_particles = emitter.settings.max_particles;
    if (static_cast<u64>(max_particles) * kParticleStride > std::numeric_limits<u64>::max() - 16u) {
        return fail("GPU particle buffer size overflows");
    }
    const auto modules = PackModuleConstants(emitter);
    const usize module_float4s = std::max<usize>(1, (modules.size() + 3) / 4);
    if (module_float4s > std::numeric_limits<u32>::max() / kModuleFloat4Stride) {
        return fail("GPU module constant buffer size overflows");
    }

    try {
        device_ = &device;
        driver_ = GpuEmitterDriver(emitter, seed);
        max_particles_ = max_particles;
        particles_ = device.CreateStorageBuffer(static_cast<u64>(max_particles) * kParticleStride,
                                                kParticleStride);
        const u64 list_bytes = (static_cast<u64>(max_particles) + 1u) * kListElementStride;
        dead_list_ = device.CreateStorageBuffer(list_bytes, kListElementStride);
        alive_list_ = device.CreateStorageBuffer(list_bytes, kListElementStride);

        const auto frame_words = PackFrameConstants(GpuFrameConstants{});
        frame_constants_ = device.CreateFrameConstantBuffer(frame_words.size() * sizeof(u32));
        std::vector<f32> module_data(module_float4s * 4, 0.0f);
        std::copy(modules.begin(), modules.end(), module_data.begin());
        module_constants_ = device.CreateFrameConstantBuffer(module_data.size() * sizeof(f32));
        device.UpdateFrameConstantBuffer(module_constants_, module_data.data(), module_data.size() * sizeof(f32));

        auto make_pipeline = [&](const char* entry, u32 sampled_textures) {
            gfx::rhi::ComputePipelineDesc desc;
            desc.hlsl_source = shader_.hlsl;
            desc.entry = entry;
            desc.storage_buffer_count = 3;
            desc.sampled_texture_count = sampled_textures;
            desc.constant_buffer_count = 2;
            return device.CreateComputePipeline(desc);
        };
        reset_pipeline_ = make_pipeline("ResetMain", 0);
        emit_pipeline_ = make_pipeline("EmitMain", 0);
        update_pipeline_ = make_pipeline("UpdateMain", shader_.uses_depth ? 1u : 0u);
        if (!particles_.IsValid() || !dead_list_.IsValid() || !alive_list_.IsValid() ||
            !frame_constants_.IsValid() || !module_constants_.IsValid() ||
            !reset_pipeline_.IsValid() || !emit_pipeline_.IsValid() || !update_pipeline_.IsValid()) {
            return fail("RHI failed to create one or more GPU emitter resources");
        }
    } catch (const std::exception& ex) {
        return fail(std::string("GPU emitter resource creation failed: ") + ex.what());
    }

    initialized_ = true;
    return true;
}

void GpuEmitterRuntime::RecordInitialize(gfx::rhi::ICommandList& command_list) {
    if (!initialized_ || reset_recorded_) return;
    GpuFrameConstants frame;
    frame.max_particles = max_particles_;
    const auto words = PackFrameConstants(frame);
    device_->UpdateFrameConstantBuffer(frame_constants_, words.data(), words.size() * sizeof(u32));
    BindResources(command_list, reset_pipeline_, particles_, dead_list_, alive_list_, frame_constants_, module_constants_);
    command_list.Dispatch(GroupsFor(max_particles_, shader_.thread_group_size));
    command_list.ComputeBufferBarrier(particles_);
    command_list.ComputeBufferBarrier(dead_list_);
    command_list.ComputeBufferBarrier(alive_list_);
    reset_recorded_ = true;
}

bool GpuEmitterRuntime::RecordFrame(gfx::rhi::ICommandList& command_list, const GpuFrameConstants& input) {
    if (!initialized_ || !reset_recorded_) return false;
    if (shader_.uses_depth && (!scene_depth_.IsValid() || depth_width_ == 0 || depth_height_ == 0)) return false;
    GpuFrameConstants frame = input;
    frame.max_particles = max_particles_;
    if (shader_.uses_depth) {
        frame.depth_width = depth_width_;
        frame.depth_height = depth_height_;
    }
    const auto words = PackFrameConstants(frame);
    device_->UpdateFrameConstantBuffer(frame_constants_, words.data(), words.size() * sizeof(u32));
    const auto values = driver_.ModuleConstants();
    const usize float4s = std::max<usize>(1, (values.size() + 3) / 4);
    std::vector<f32> module_data(float4s * 4, 0.0f);
    std::copy(values.begin(), values.end(), module_data.begin());
    device_->UpdateFrameConstantBuffer(module_constants_, module_data.data(), module_data.size() * sizeof(f32));

    BindResources(command_list, emit_pipeline_, particles_, dead_list_, alive_list_, frame_constants_, module_constants_);
    command_list.Dispatch(GroupsFor(frame.spawn_count, shader_.thread_group_size));
    command_list.ComputeBufferBarrier(particles_);
    command_list.ComputeBufferBarrier(dead_list_);
    command_list.ComputeBufferBarrier(alive_list_);

    BindResources(command_list, update_pipeline_, particles_, dead_list_, alive_list_, frame_constants_, module_constants_);
    if (shader_.uses_depth) command_list.BindComputeSampledTexture(0, scene_depth_);
    command_list.Dispatch(GroupsFor(max_particles_, shader_.thread_group_size));
    return true;
}

} // namespace aether::vfx
