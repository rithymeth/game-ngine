#pragma once

#include "aether/net/replication.h"
#include "aether/scene/components.h"

#include <deque>

namespace aether::net {

// Transforms over server time, for snapshot interpolation.
class InterpolationBuffer {
public:
    static constexpr usize kCapacity = 32;

    // Samples must come in increasing time; older or equal ones are ignored.
    void Push(f64 time, const Transform& transform);
    // The transform at `time`: interpolated between the samples around it
    // (position linearly, rotation by normalized lerp along the shorter
    // arc), or the oldest/newest one outside them. False when empty.
    bool Sample(f64 time, Transform& out) const;
    usize Size() const { return samples_.size(); }
    f64 Newest() const { return samples_.empty() ? 0.0 : samples_.back().time; }
    f64 Oldest() const { return samples_.empty() ? 0.0 : samples_.front().time; }

private:
    struct Sample_ {
        f64 time;
        Transform transform;
    };
    std::deque<Sample_> samples_;
};

Quaternion NlerpShortest(const Quaternion& a, const Quaternion& b, f32 t);

// Smooths what other machines own (Phase 22 step 4, docs/design/PHASE_SPECS.md
// §22.4): every snapshot adds each replicated entity's Transform to its
// buffer at the snapshot's server time, and every frame the entities are
// shown as they were `delay` seconds ago in server time - between two
// snapshots, never jumping to the newest. The client's estimate of server
// time follows the snapshots' arrival, smoothed, and never runs backwards.
// Entities this client owns are left to prediction.
class SnapshotInterpolation {
public:
    SnapshotInterpolation(World& world, ReplicationClient& replication, f64 delay = 0.1);

    // Call each frame after the replication client has taken its events.
    void Update(f64 now);

    f64 RenderTime() const { return render_time_; }
    f64 Delay() const { return delay_; }
    void SetDelay(f64 delay) { delay_ = delay; }
    const InterpolationBuffer* Buffer(u32 net_id) const;
    // Frames where the render time had run past an entity's newest sample.
    u32 Starved() const { return starved_; }

private:
    World& world_;
    ReplicationClient& replication_;
    f64 delay_;
    u32 seen_snapshots_ = 0;
    bool has_offset_ = false;
    f64 offset_ = 0.0, render_time_ = -1e300;
    u32 starved_ = 0;
    std::unordered_map<u32, InterpolationBuffer> buffers_;
};

} // namespace aether::net
