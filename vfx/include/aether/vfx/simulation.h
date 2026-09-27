#pragma once

#include "aether/math/quaternion.h"
#include "aether/vfx/emitter.h"

#include <memory>
#include <vector>

namespace aether::vfx {

// Live particles, one array per attribute (Phase 19 step 1, §19.1), in the
// emitter's simulation space. Only the first `count` are alive; deaths
// swap the last one in, so order isn't kept (sorting is for drawing).
struct ParticleBuffer {
    std::vector<Vec3> position, velocity;
    std::vector<f32> age, lifetime;
    std::vector<f32> size, base_size;
    std::vector<f32> rotation, spin;
    std::vector<LinearColor> color, base_color;
    std::vector<u32> seed; // per particle, for noise and variation
    std::vector<u64> id;   // unique within the emitter, in birth order
    usize count = 0;

    void Reserve(usize capacity);
    usize Capacity() const { return position.size(); }
    void Kill(usize i); // the last particle takes its place
    f32 NormalizedAge(usize i) const { return lifetime[i] > 0.0f ? age[i] / lifetime[i] : 1.0f; }
};

struct Bounds {
    Vec3 min, max;
    bool valid = false;
};

// Where an emitter is: a position and a rotation (world).
struct EmitterPose {
    Vec3 position;
    Quaternion rotation;
    Vec3 Right() const;
    Vec3 Up() const;
    Vec3 Forward() const; // +Z
    Vec3 ToWorld(const Vec3& local) const;
    Vec3 ToLocal(const Vec3& world) const;
    Vec3 DirectionToWorld(const Vec3& local) const;
    Vec3 DirectionToLocal(const Vec3& world) const;
};

// One emitter playing: its clock, spawning, and the particle simulation.
// The same asset, seed and time steps give the same particles.
class EmitterInstance {
public:
    EmitterInstance(const Emitter& emitter, u64 seed = 1);

    // Moves the emitter. The first call places it; later ones are motion
    // (particles spawned along the way, SpawnPerDistance, InheritVelocity).
    void SetPose(const EmitterPose& pose);
    void Teleport(const EmitterPose& pose); // moves without motion
    const EmitterPose& Pose() const { return pose_; }

    void Update(f32 dt);
    // Stops spawning; what's alive lives out its life.
    void Stop() { stopped_ = true; }
    void Clear(); // kills every particle
    // From the start again (particles cleared, the random sequence restarted, warmup run).
    void Restart();
    // Births `count` particles now, outside the spawn modules (scripts, events). How many were born comes back.
    usize Emit(usize count);

    bool Spawning() const; // still in its loop, not stopped
    bool Finished() const { return !Spawning() && particles_.count == 0; }
    f32 Time() const { return time_; }
    u32 Loop() const; // how many loops have ended
    const ParticleBuffer& Particles() const { return particles_; }
    usize Count() const { return particles_.count; }
    u64 TotalSpawned() const { return next_id_; }
    Bounds ComputeBounds() const; // in the simulation space, sizes included
    const Emitter& Asset() const { return emitter_; }

private:
    struct Birth {
        f32 age = 0.0f;  // already lived this frame
        f32 frac = 1.0f; // where along this frame's motion (0: the last pose, 1: this one)
    };
    void Advance(f32 dt);
    void Spawn(const std::vector<Birth>& births);
    void Initialize(usize i, const Birth& b);
    void Simulate(f32 dt, usize first_new);
    // A module point or direction into simulation space.
    Vec3 PointIn(const Vec3& p, bool world) const;
    Vec3 DirectionIn(const Vec3& d, bool world) const;

    Emitter emitter_;
    u64 seed_;
    Random rng_;
    ParticleBuffer particles_;
    EmitterPose pose_, last_pose_;
    bool placed_ = false;
    bool stopped_ = false;
    bool warmed_ = false;
    f32 time_ = 0.0f;
    std::vector<f32> carries_; // per spawn module
    Vec3 emitter_velocity_;
    u64 next_id_ = 0;
    std::vector<f32> step_; // this frame's time step per particle (newborns only live part of it)
};

// A whole particle system: its emitters, moved and ticked together.
class ParticleSystemInstance {
public:
    explicit ParticleSystemInstance(const ParticleSystemAsset& asset, u64 seed = 1);
    void SetPose(const EmitterPose& pose);
    void Teleport(const EmitterPose& pose);
    void Update(f32 dt);
    void Stop();
    void Restart();
    bool Finished() const;
    usize Count() const;
    usize EmitterCount() const { return emitters_.size(); }
    EmitterInstance& Emitter(usize i) { return *emitters_[i]; }
    const EmitterInstance& Emitter(usize i) const { return *emitters_[i]; }

private:
    std::vector<std::unique_ptr<EmitterInstance>> emitters_;
};

} // namespace aether::vfx
