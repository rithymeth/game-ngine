#pragma once

#include "aether/ecs/world.h"
#include "aether/math/quaternion.h"
#include "aether/net/replication.h"

#include <deque>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace aether::net {

// Snapshot interpolation (Phase 22 step 5): how a client shows entities it
// doesn't control. The server sends timestamped position/rotation snapshots
// (unreliable, at a modest rate); the client keeps a short buffer per entity
// and renders it slightly in the past, blending between the two snapshots
// around that moment, so motion is smooth despite jitter and lost packets.
// If the buffer runs dry the entity is extrapolated briefly, then held.
//
//   [kSnapshotMessage][f64 server time][varint count]
//     count x ([varint net id][3 x f32 position][3 x i16 rotation x, y, z])
//
// The rotation's w is rebuilt from the unit-length constraint. A moving
// entity's Transform should not also be replicated: mark it
// ReplicationServer::ReplicateOnlyOnSpawn<Transform>() so the reliable stream
// sets it once and the snapshots take over.

inline constexpr u8 kSnapshotMessage = 0xA4;

struct EntitySnapshot {
    u32 net_id = 0;
    Vec3 position{0, 0, 0};
    Quaternion rotation;
};

// --- server -----------------------------------------------------------------

// The current Transform of every entity with a NetIdentity (assigned id) that
// `filter` accepts (default: all).
std::vector<EntitySnapshot> CaptureSnapshots(const World& world, const std::function<bool(const NetIdentity&)>& filter = {});

// Packs snapshots into as many messages as the size limit needs.
std::vector<std::vector<u8>> BuildSnapshotMessages(f64 server_time, std::span<const EntitySnapshot> snapshots, usize max_message_size);

// False if the message is malformed (nothing is returned then).
bool ParseSnapshotMessage(std::span<const u8> message, f64& server_time, std::vector<EntitySnapshot>& out);

// --- client -----------------------------------------------------------------

// Estimates the server's clock from snapshot timestamps. Each timestamp
// minus its arrival time is the true offset less that packet's delay, so the
// largest recent value is the best (least delayed) estimate; it follows a
// drifting clock slowly and jumps if the server's clock changes drastically.
class ClockSync {
public:
    void OnServerTime(f64 server_time, f64 local_now);
    bool Synced() const { return synced_; }
    // The server time of the newest data that could have arrived by `local_now`.
    f64 ServerNow(f64 local_now) const { return local_now + offset_; }

private:
    bool synced_ = false;
    f64 offset_ = 0.0;
};

struct InterpolationConfig {
    f64 delay = 0.1;              // how far in the past entities are shown, seconds
    f64 max_extrapolation = 0.25; // past the newest snapshot, how long to keep moving at the last velocity
    usize capacity = 32;          // snapshots kept per entity
};

class InterpolationBuffer {
public:
    explicit InterpolationBuffer(const InterpolationConfig& config = {}) : config_(config) {}

    // Out-of-order snapshots are slotted in; a repeat of a time replaces it; older than the
    // whole buffer is dropped.
    void Push(f64 time, const Vec3& position, const Quaternion& rotation);

    // The state at `render_time`. False if empty. Before the oldest snapshot it
    // is the oldest; after the newest it extrapolates (`extrapolating` set) up to the limit, then holds.
    bool Sample(f64 render_time, Vec3& position, Quaternion& rotation, bool* extrapolating = nullptr) const;

    usize Size() const { return snapshots_.size(); }
    f64 NewestTime() const { return snapshots_.empty() ? 0.0 : snapshots_.back().time; }

private:
    struct Entry {
        f64 time;
        Vec3 position;
        Quaternion rotation;
    };
    InterpolationConfig config_;
    std::deque<Entry> snapshots_; // ascending time
};

struct InterpolatorStats {
    u64 snapshots = 0;
    u64 malformed = 0;
    u64 extrapolated_frames = 0; // entity-frames shown by extrapolation: a sign the delay is too short or loss too high
};

class SnapshotInterpolator {
public:
    explicit SnapshotInterpolator(const InterpolationConfig& config = {}) : config_(config) {}

    // A kSnapshotMessage that arrived at `local_now` (the same clock as Apply's).
    bool OnSnapshotMessage(std::span<const u8> message, f64 local_now);

    // Writes the interpolated Transform of every buffered entity that exists
    // in `replication`'s world. Entities owned by `local_owner` (the ones this
    // client predicts) are left alone; 0 skips none.
    void Apply(World& world, const ReplicationClient& replication, f64 local_now, NetAddress local_owner = 0);

    void Remove(u32 net_id) { buffers_.erase(net_id); }
    void Clear() { buffers_.clear(); }
    usize EntityCount() const { return buffers_.size(); }
    const ClockSync& Clock() const { return clock_; }
    const InterpolatorStats& Stats() const { return stats_; }

private:
    InterpolationConfig config_;
    ClockSync clock_;
    std::unordered_map<u32, InterpolationBuffer> buffers_;
    InterpolatorStats stats_;
};

} // namespace aether::net
