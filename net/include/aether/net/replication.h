#pragma once

#include "aether/ecs/world.h"
#include "aether/math/math.h"
#include "aether/net/host.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace aether::net {

// Marks an entity as networked. On the server, give an entity one (net_id
// left 0) and the ReplicationServer assigns its id and sends it to the
// clients it is relevant to; on a client, the ReplicationClient creates
// entities with one filled in. Plain data.
struct NetIdentity {
    u32 net_id = 0;            // assigned by the server; 0 until then
    PeerId owner = kNoPeer;    // the server's id for the owning client (kNoPeer: the server owns it)
    char archetype[32] = {};   // what the client spawns ("player", "rocket"); see ReplicationClient::OnSpawn
    f32 relevancy_radius = 0;  // sent to viewers within this distance; 0: to everyone
    f32 priority = 1;          // share of the bandwidth budget when there is not room for everything
    bool locally_owned = false; // client side: this client owns it
};

void SetNetArchetype(NetIdentity& identity, const std::string& archetype);

struct ReplicationConfig {
    u8 snapshot_channel = 2;  // an UnreliableSequenced channel: late snapshots are useless
    u8 control_channel = 1;   // an Unreliable channel for acks
    usize byte_budget = 1000; // per snapshot per client (at most Connection::kMaxFragment)
    f64 send_interval = 1.0 / 20.0;
    u32 history = 64;         // snapshots remembered per client (deltas are against one of them)
};

// The replicated state of one entity: per component (by name hash) the
// replicated fields' bytes. Components replicate their Field_Replicated
// fields; Transform always replicates whole.
struct ReplicatedComponent {
    u32 hash = 0;
    std::vector<std::vector<u8>> fields; // by replicated-field index; empty: not sent yet
};
struct ReplicatedEntity {
    PeerId owner = kNoPeer;
    std::string archetype;
    std::vector<ReplicatedComponent> components; // sorted by hash
};
using ReplicatedState = std::unordered_map<u32, ReplicatedEntity>; // by net id

// The components that replicate and their replicated fields.
struct ReplicatedComponentType {
    ComponentId id = kInvalidComponentId;
    u32 hash = 0;
    std::vector<const reflect::FieldInfo*> fields; // at most 32
};
const std::vector<ReplicatedComponentType>& ReplicatedComponentTypes();
const ReplicatedComponentType* FindReplicatedComponentType(u32 hash);
u32 ComponentNameHash(const char* name);

// Sends the world to clients as delta snapshots (Phase 22 step 2,
// docs/design/PHASE_SPECS.md §22.2). Every send_interval each connected
// client gets one snapshot holding, for the entities relevant to it, the
// replicated fields that differ from the last snapshot it acknowledged -
// so an idle world costs a few bytes, and a lost snapshot costs nothing
// but latency, because the next one is still against an acknowledged
// baseline. Spawns carry everything; despawns are explicit. When the
// changes do not fit the byte budget, entities are sent by accumulated
// priority and the rest wait.
class ReplicationServer {
public:
    ReplicationServer(World& world, NetHost& host, ReplicationConfig config = {});

    // Where a client looks from, for relevancy; without one, the Transform
    // of an entity it owns; without that, everything is relevant.
    void SetViewer(PeerId peer, const Vec3& position);
    void ClearViewer(PeerId peer);

    // Takes the acks out of the host's events; true when `event` was one
    // (or a disconnect it needed to see; those are left for the game too: false).
    bool HandleEvent(const NetEvent& event);
    void Update(f64 now);

    Entity FindEntity(u32 net_id) const;

    struct PeerStats {
        u16 last_snapshot = 0;
        u16 acked = 0xFFFF;
        usize last_bytes = 0;
        u32 entities_sent = 0, entities_deferred = 0, despawns_sent = 0;
        u64 total_bytes = 0;
    };
    PeerStats Stats(PeerId peer) const;

private:
    struct History {
        u16 id = 0xFFFF;
        ReplicatedState state;
    };
    struct Peer {
        u16 next_snapshot = 0;
        u16 acked = 0xFFFF;
        bool has_viewer = false;
        Vec3 viewer;
        std::vector<History> history;
        std::unordered_map<u32, f32> accumulators;
        PeerStats stats;
    };
    struct Meta {
        f32 radius = 0, priority = 1;
        bool has_position = false;
        Vec3 position;
    };
    void Gather(ReplicatedState& current);
    void SendTo(PeerId id, Peer& peer, const ReplicatedState& current, f64 now);

    World& world_;
    NetHost& host_;
    ReplicationConfig config_;
    u32 next_net_id_ = 1;
    f64 last_send_ = -1e300;
    std::unordered_map<u32, Entity> entities_;
    std::unordered_map<u32, Meta> meta_;
    std::unordered_map<PeerId, Peer> peers_;
};

// Rebuilds the server's world from its snapshots (Phase 22 step 2): spawns
// and despawns entities, writes the replicated fields that changed, and
// acknowledges each snapshot so the next is a delta against it.
class ReplicationClient {
public:
    ReplicationClient(World& world, NetHost& host, PeerId server, ReplicationConfig config = {});

    // Runs when an entity of `archetype` is spawned, before its fields are
    // written: add the components that are not replicated (a mesh, a
    // collider). "" matches every archetype without its own hook.
    using SpawnFn = std::function<void(World&, Entity, const NetIdentity&)>;
    void OnSpawn(const std::string& archetype, SpawnFn fn);
    using DespawnFn = std::function<void(World&, Entity)>;
    void OnDespawn(DespawnFn fn) { on_despawn_ = std::move(fn); }

    // Takes the snapshots out of the host's events; true when `event` was one.
    bool HandleEvent(const NetEvent& event);

    Entity FindEntity(u32 net_id) const;
    usize EntityCount() const { return entities_.size(); }
    u16 LastSnapshot() const { return last_; }
    f64 ServerTime() const { return server_time_; }

    struct Stats {
        u32 snapshots = 0;
        u32 undecodable = 0; // their baseline was forgotten
        u32 spawned = 0, despawned = 0;
        u64 fields_written = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct History {
        u16 id = 0xFFFF;
        ReplicatedState state;
    };
    bool Receive(std::span<const u8> data);
    void Apply(const ReplicatedState& state);

    World& world_;
    NetHost& host_;
    PeerId server_;
    ReplicationConfig config_;
    std::vector<History> history_;
    ReplicatedState applied_;
    std::unordered_map<u32, Entity> entities_;
    std::unordered_map<std::string, SpawnFn> on_spawn_;
    DespawnFn on_despawn_;
    u16 last_ = 0xFFFF;
    bool any_ = false;
    f64 server_time_ = 0;
    Stats stats_;
};

} // namespace aether::net
