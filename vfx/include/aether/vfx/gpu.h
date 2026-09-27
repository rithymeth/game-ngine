#pragma once

#include "aether/vfx/simulation.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::vfx {

// The GPU path (Phase 19 step 5, §19.5): an emitter's stack generated as
// one HLSL compute shader, with the CPU simulation as its reference.
//
//   EmitMain   (one thread per new particle): takes a free slot from the
//              dead list and runs the Initialize modules.
//   UpdateMain (one thread per slot): runs the Update modules, kills what
//              died (back onto the dead list), and appends the living to
//              the alive list the renderer draws from.
//
// Module values aren't baked into the code: they're float4 constants the
// CPU fills each frame (PackModuleConstants), so parameters change them
// without a recompile. Curves and gradients are sampled into tables.
// Enums (shapes, modes, the `world` flags) are compiled in. The CPU keeps
// the clock and the spawn modules (GpuEmitterDriver) and tells the shader
// how many to emit. Dispatch belongs to the Windows renderer.

// Why an emitter can or can't simulate on the GPU.
struct GpuSupport {
    bool supported = true;
    std::vector<std::string> reasons; // what needs the CPU
};
// Sub-emitters and light renderers need particle data back on the CPU;
// ribbons need birth order. Everything else runs on the GPU. Scene
// collisions there use the depth buffer.
GpuSupport CheckGpuSupport(const Emitter& emitter);

// Where an emitter simulates: its setting, or for Auto the GPU when the
// device has compute, the stack is supported, and it's big enough to be
// worth it (max_particles at least `auto_threshold`).
SimTarget ChooseSimTarget(const Emitter& emitter, bool gpu_available, u32 auto_threshold = 4096);

constexpr u32 kCurveSamples = 16;    // per curve table (4 float4s)
constexpr u32 kGradientSamples = 16; // per gradient table (16 float4s)

struct GeneratedParticleShader {
    bool ok = false;
    std::vector<std::string> errors; // CheckGpuSupport's reasons when it isn't supported
    std::string hlsl;
    u32 module_constants = 0; // float4s in the Modules buffer
    std::vector<std::string> constant_names; // one per float4 ("update[0] Gravity.acceleration")
    u32 thread_group_size = 64;
    bool uses_depth = false; // binds SceneDepth (t0) for scene collisions
    bool uses_noise = false;
    u64 key = 0; // equal for equal code: shared by emitters with the same stack shape
};
GeneratedParticleShader GenerateParticleShader(const Emitter& emitter);

// The Modules buffer's float4s for the emitter as it is now (parameters
// applied), in the order the generated shader reads them.
std::vector<f32> PackModuleConstants(const Emitter& emitter);

// A curve or gradient sampled evenly over 0..1 (what the shader reads).
std::vector<f32> BakeCurve(const FloatCurve& curve, u32 samples = kCurveSamples);
std::vector<LinearColor> BakeGradient(const ColorGradient& gradient, u32 samples = kGradientSamples);
// Reading a baked table as the shader does (linear between samples).
f32 SampleBakedCurve(const std::vector<f32>& table, f32 t);

// The per-frame constants (cbuffer Frame, b0), packed as the shader
// declares them.
struct GpuFrameConstants {
    f32 delta_time = 0.0f, time = 0.0f;
    u32 spawn_count = 0, frame_seed = 0;
    Vec3 emitter_position;
    u32 max_particles = 0;
    Quaternion emitter_rotation;
    Vec3 previous_position;
    f32 depth_thickness = 0.5f;
    Vec3 emitter_velocity;
    u32 depth_width = 0, depth_height = 0;
    Mat4 view_projection, inverse_view_projection;
};
std::vector<u32> PackFrameConstants(const GpuFrameConstants& frame); // 32-bit words, 16-byte rows
constexpr usize kFrameConstantWords = 56;

// The CPU half of a GPU emitter: its clock, loops and spawn modules (the
// same code as EmitterInstance's), giving each frame's spawn count and
// constants.
class GpuEmitterDriver {
public:
    GpuEmitterDriver(const Emitter& emitter, u64 seed = 1);
    void SetPose(const EmitterPose& pose);
    void Teleport(const EmitterPose& pose);
    GpuFrameConstants Update(f32 dt);
    std::vector<f32> ModuleConstants() const { return PackModuleConstants(emitter_); }
    bool SetField(const std::string& field, const ParameterValue& value, std::string* error = nullptr);
    void Stop() { spawner_->Stop(); }
    void Restart() { spawner_->Restart(); }
    bool Spawning() const { return spawner_->Spawning(); }
    u64 TotalSpawned() const { return total_; }
    const Emitter& Asset() const { return emitter_; }

private:
    Emitter emitter_;
    std::unique_ptr<EmitterInstance> spawner_; // spawn modules only; its particles are counted and dropped
    Vec3 previous_; // the pose at the end of the last frame
    bool placed_ = false;
    u64 total_ = 0;
    u32 frame_ = 0;
    u64 seed_;
};

} // namespace aether::vfx
