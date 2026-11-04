#pragma once

#include "aether/net/host.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics_net/grab_policy.h"

#include <functional>
#include <map>
#include <unordered_map>

namespace aether::physics_net {

struct PhysicsNetHostConfig {
    GrabPolicy policy;
    f64 snapshot_interval = 1.0 / 20.0; // seconds between constraint snapshots sent to every peer
    usize snapshot_chunk_bytes = 1000;  // the most one chunk encodes to (an unreliable message holds 1024)
};

struct PhysicsNetHostStats {
    u32 snapshots_sent = 0, chunks_sent = 0;
    u32 requests = 0, granted = 0, denied = 0;
    u32 denied_by[9] = {}; // by GrabStatus
    u32 targets_applied = 0, targets_stale = 0, targets_clamped = 0, targets_ignored = 0; // ignored: no grab by that peer on that object
    u32 releases = 0;       // by the client
    u32 timeouts = 0;       // released: no target updates for policy.target_timeout
    u32 lost_grabs = 0;     // released: the grab ended by itself (broke, or its object went away)
    u32 malformed = 0;
};

// The host's side of physics over the network (Priority 2): it sends every peer the constraint snapshot a few times a second (chunked, unreliable:
// the latest state wins and a lost chunk only delays its records), and it validates and runs the clients' grab requests. Clients never create
// grabs: a request names an object by GUID and a world point, the host checks GrabPolicy against its own world (the pawn registered for the
// peer with SetPlayer is the holder, so the holder's position is the host's, not the client's), creates the grab on its PhysicsScene, and
// answers with a GrabResult. Target updates then move the grab, clamped to the holder's reach. A grab dies with its peer, its pawn, a target
// timeout, or when the physics breaks it or removes its object; the client is told with GrabStatus::Released.
//
// One per host; call HandleEvent for every NetEvent the NetHost produced and Tick once a frame, after the physics step. The GuidIndex must name
// the world's entities (see GuidIndex::Rebuild) and outlive this.
class PhysicsNetHost {
public:
    PhysicsNetHost(PhysicsScene& scene, net::NetHost& host, const GuidIndex& guids, PhysicsNetHostConfig config = {});

    // The entity (a pawn with a position) that is the holder for the peer's grabs. Replaces an earlier one (and releases its grabs).
    void SetPlayer(net::PeerId peer, Entity pawn);
    // Releases the peer's grabs and forgets it. Also done when its Disconnected event is handled.
    void RemovePlayer(net::PeerId peer);
    // Which entities may be grabbed (default: all dynamic bodies).
    void SetGrabbableFilter(std::function<bool(Entity)> filter) { filter_ = std::move(filter); }

    // Handles a message of ours (true) or a disconnect (false: still the caller's to see too).
    bool HandleEvent(const net::NetEvent& event);
    // Once a frame: grab upkeep (timeouts, grabs that ended) and the snapshot when it is due.
    void Tick(f64 now);

    usize GrabsOf(net::PeerId peer) const;
    const PhysicsNetHostStats& Stats() const { return stats_; }
    const PhysicsNetHostConfig& Config() const { return config_; }

private:
    struct HeldGrab {
        GrabId id = kInvalidGrab;
        Entity object;
        f64 deadline = 0.0; // the grab is released when this passes without a target (see GrabPolicy::first_target_grace / target_timeout)
        u32 last_sequence = 0;
        bool has_sequence = false;
    };
    struct Player {
        Entity pawn;
        TokenBucket bucket;
        std::map<EntityGuid, HeldGrab> grabs;
    };
    void OnRequest(net::PeerId peer, const GrabRequest& request);
    void OnTarget(net::PeerId peer, const GrabTarget& target);
    void OnRelease(net::PeerId peer, const GrabRelease& release);
    void Reply(net::PeerId peer, u16 request_id, GrabStatus status, const EntityGuid& object);
    void ReleaseAll(Player& player);
    Vec3 PositionOf(Entity entity) const;

    PhysicsScene& scene_;
    net::NetHost& host_;
    const GuidIndex& guids_;
    PhysicsNetHostConfig config_;
    std::function<bool(Entity)> filter_;
    std::unordered_map<net::PeerId, Player> players_;
    PhysicsNetHostStats stats_;
    f64 now_ = 0.0, next_snapshot_ = 0.0;
    u32 snapshot_sequence_ = 0;
};

} // namespace aether::physics_net
