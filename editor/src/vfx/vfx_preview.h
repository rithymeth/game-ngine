#pragma once

#include "aether/vfx/gpu.h"
#include "aether/vfx/render.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::editor {

// The particle editor's live preview (Phase 19 step 6): the system playing
// on the CPU from a fixed seed in fixed steps, so seeking to a time always
// shows the same particles (the timeline scrubber); an orbit camera; and
// per-emitter stats.
class ParticlePreview {
public:
    static constexpr f32 kStep = 1.0f / 60.0f;
    static constexpr f32 kMaxSeek = 60.0f;

    explicit ParticlePreview(u64 seed = 1) : seed_(seed) {}

    // From the start with this asset.
    void Reset(const vfx::ParticleSystemAsset& asset);
    // Runs on by `dt` (in whole steps; the rest carries over).
    void Advance(f32 dt);
    // Replays from the start to `time` (clamped to kMaxSeek).
    void Seek(const vfx::ParticleSystemAsset& asset, f32 time);
    f32 Time() const { return time_; }
    const vfx::ParticleSystemInstance* Instance() const { return instance_.get(); }
    usize Count() const { return instance_ ? instance_->Count() : 0; }

    struct EmitterStats {
        std::string name;
        usize count = 0;
        u64 spawned = 0;
        vfx::Bounds bounds;
        f64 update_ms = 0.0; // the last step's CPU time
        vfx::SimTarget target = vfx::SimTarget::Cpu; // where it would run with a GPU
        bool gpu_supported = true;
    };
    std::vector<EmitterStats> Stats() const;

    // The orbit camera: about `target`, `distance` away, turned by yaw and pitch (radians).
    f32 yaw = 0.6f, pitch = 0.35f, distance = 8.0f;
    Vec3 target{0.0f, 1.0f, 0.0f};
    vfx::ParticleCamera Camera() const;
    Mat4 ViewProjection(f32 aspect) const;
    // A world point on a canvas of the given size (pixels, y down); false behind the camera.
    bool Project(const Vec3& world, f32 width, f32 height, f32& x, f32& y, f32& depth) const;

private:
    u64 seed_;
    std::unique_ptr<vfx::ParticleSystemInstance> instance_;
    vfx::ParticleSystemAsset asset_;
    f32 time_ = 0.0f, carry_ = 0.0f;
    std::vector<f64> ms_;
};

} // namespace aether::editor
