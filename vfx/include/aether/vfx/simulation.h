#pragma once

#include "aether/math/quaternion.h"
#include "aether/vfx/collision.h"
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

// Something that happened to a particle this frame (step 3), in world space.
struct ParticleEvent {
    ParticleEventKind kind = ParticleEventKind::Death;
    Vec3 position, velocity;
    Vec3 normal;  // Collision: the surface's
    LinearColor color;
    f32 size = 0.0f;
    u64 id = 0;
    bool expired = false; // Death: of old age (not killed by a volume, attractor or hit)
};
constexpr u8 EventBit(ParticleEventKind k) { return static_cast<u8>(1u << static_cast<u8>(k)); }

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
    // Births at a world point (a sub-emitter's event), with extra velocity,
    // and the colour and size taken from the event if given.
    usize EmitAt(usize count, const Vec3& world_position, const Vec3& add_velocity = {}, const LinearColor* color = nullptr, const f32* size = nullptr);

    // --- Step 3 -----------------------------------------------------------------------
    // What SceneCollision modules hit (not owned; null: they do nothing).
    void SetCollider(const ParticleCollider* collider) { collider_ = collider; }
    // Which events to keep (EventBit masks); this frame's, until the next Update (or ClearEvents).
    void RecordEvents(u8 mask) { event_mask_ = mask; }
    u8 EventMask() const { return event_mask_; }
    const std::vector<ParticleEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }
    // A system keeps events across its emitters' updates and clears them itself.
    void SetAutoClearEvents(bool on) { auto_clear_events_ = on; }
    // Sets a module field of this instance's copy of the emitter (parameters).
    bool SetField(const std::string& field, const ParameterValue& value, std::string* error = nullptr);

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
    void Record(ParticleEventKind kind, usize i, const Vec3& normal = {}, bool expired = false);

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
    std::vector<Vec3> previous_; // positions before this frame's move (scene collisions)
    const ParticleCollider* collider_ = nullptr;
    u8 event_mask_ = 0;
    bool auto_clear_events_ = true;
    std::vector<ParticleEvent> events_;
};

// A whole particle system: its emitters, moved and ticked together; its
// sub-emitters fed from their events; its parameters.
class ParticleSystemInstance {
public:
    explicit ParticleSystemInstance(const ParticleSystemAsset& asset, u64 seed = 1);
    // Sets a declared parameter (of its type) and every field bound to it.
    bool SetParameter(const std::string& name, const ParameterValue& value, std::string* error = nullptr);
    const ParameterValue* GetParameter(const std::string& name) const;
    void SetCollider(const ParticleCollider* collider);
    // Keep these events on every emitter too (for gameplay), besides what sub-emitters need.
    void RecordEvents(u8 mask);
    // Particles born through sub-emitters so far.
    u64 SubEmitterSpawns() const { return sub_spawns_; }
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
    struct ResolvedSub {
        SubEmitter sub;
        usize target = 0;
    };
    std::vector<std::unique_ptr<EmitterInstance>> emitters_;
    std::vector<std::vector<ResolvedSub>> subs_; // per emitter
    std::vector<u8> needed_masks_;               // what each emitter's sub-emitters listen for
    std::vector<ParticleParameter> parameters_;
    std::vector<vfx::Emitter> assets_;           // for bindings
    Random rng_;
    u64 sub_spawns_ = 0;
};

} // namespace aether::vfx
