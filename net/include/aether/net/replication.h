#pragma once

#include "aether/ecs/world.h"
#include "aether/net/bytes.h"
#include "aether/net/components.h"
#include "aether/net/endpoint.h"

#include <functional>
#include <unordered_map>
#include <vector>

namespace aether::net {

// State replication (Phase 22 step 2), built on reflection: every field
// flagged Field_Replicated, of every component on an entity that has a
// NetIdentity, is sent server -> client. Only what changed since the last
// update the peer received goes on the wire (a per-field delta against a
// per-peer baseline), over the reliable ordered channel, so a peer can never
// miss a change or see one out of order.
//
// Wire (reliable messages whose first byte is kReplicationMessage; other
// messages belong to the game): a batch of records - Spawn (all replicated
// components), Update (changed fields of changed components, adding a
// component the client lacks), RemoveComponent, Despawn.
//
// Components are named on the wire by a hash of their reflected name, so the
// client must have those component types registered (touching
// GetComponentId<T>() once, as RegisterReplicatedComponent does). Fields are
// encoded by reflected type (see EncodeValue); a component may have at most
// 64 replicated fields, and one entity's Spawn must fit in one message.

inline constexpr u8 kReplicationMessage = 0xA0;

// --- value codec -----------------------------------------------------------

// Encodes a reflected value compactly: integers as varints, floats raw,
// strings and arrays length-prefixed, structs as their non-transient fields.
void EncodeValue(const reflect::TypeInfo& type, const void* value, ByteWriter& out);
// False if the bytes are malformed; `value` may then be partly written.
bool DecodeValue(const reflect::TypeInfo& type, void* value, ByteReader& in);

// --- replicated components --------------------------------------------------

struct ReplicatedComponent {
    ComponentId id = kInvalidComponentId;
    u32 hash = 0;
    const reflect::TypeInfo* type = nullptr;
    std::vector<const reflect::FieldInfo*> fields; // the Field_Replicated ones, declaration order
};

// Every registered component with at least one replicated field.
const std::vector<ReplicatedComponent>& ReplicatedComponents();
const ReplicatedComponent* FindReplicated(u32 hash);
u32 ComponentHash(const char* name);

template <typename T>
void RegisterReplicatedComponent() {
    (void)GetComponentId<T>();
}

// --- server -----------------------------------------------------------------

struct ReplicationStats {
    u64 records_sent = 0;
    u64 spawns = 0;
    u64 updates = 0;
    u64 removals = 0;
    u64 despawns = 0;
    u64 bytes_sent = 0;
    u64 deferred = 0;  // records held back by the byte budget or a full send queue
    u64 oversized = 0; // records too big for one message (never sent)
};

class ReplicationServer {
public:
    ReplicationServer(World& world, NetEndpoint& endpoint);

    // Decides whether a peer should see an entity (default: everyone sees everything).
    // An entity that stops being relevant is despawned on that client, and
    // spawned again when it becomes relevant.
    std::function<bool(NetAddress peer, Entity entity)> relevancy;

    // Most bytes of records sent to one peer per Replicate call (0 = unlimited).
    // Records beyond it wait for the next call; the longer one waits, the
    // higher its priority, and new spawns always go first.
    usize bytes_per_peer = 0;

    // One replication pass: assigns network ids, and sends each connected peer
    // what changed. Call it at the game's network tick rate, after
    // NetEndpoint::Update. Returns the number of records sent.
    usize Replicate();

    Entity EntityOf(u32 net_id) const;
    bool Knows(NetAddress peer, u32 net_id) const;
    const ReplicationStats& Stats() const { return stats_; }

private:
    using FieldBytes = std::vector<std::vector<u8>>; // one encoding per replicated field
    struct Component {
        u32 hash = 0;
        const ReplicatedComponent* info = nullptr;
        FieldBytes fields;
    };
    struct Snapshot {
        u32 net_id = 0;
        Entity entity;
        std::vector<Component> components;
    };
    struct Baseline {
        std::vector<Component> components;
    };
    struct PeerState {
        std::unordered_map<u32, Baseline> known;
        std::unordered_map<u32, f32> priority;
    };
    struct Record;

    void TakeSnapshots(std::vector<Snapshot>& out);
    void ReplicateTo(NetAddress peer, PeerState& state, const std::vector<Snapshot>& snapshots);

    World& world_;
    NetEndpoint& endpoint_;
    u32 next_net_id_ = 1;
    std::unordered_map<u32, Entity> by_id_;
    std::unordered_map<NetAddress, PeerState> peers_;
    ReplicationStats stats_;
};

// --- client -----------------------------------------------------------------

struct ClientReplicationStats {
    u64 spawns = 0;
    u64 updates = 0;
    u64 removals = 0;
    u64 despawns = 0;
    u64 malformed = 0;
};

class ReplicationClient {
public:
    explicit ReplicationClient(World& world) : world_(world) {}

    static bool IsReplicationMessage(const NetEvent& event) {
        return event.type == NetEventType::Message && event.channel == Channel::ReliableOrdered && !event.data.empty() &&
               event.data[0] == kReplicationMessage;
    }

    // Applies one replication message to the world. False if it is malformed
    // (the records before the bad one were applied).
    bool Apply(std::span<const u8> message);

    Entity EntityOf(u32 net_id) const;
    usize EntityCount() const { return by_id_.size(); }
    // Destroys every replicated entity (on disconnect).
    void Clear();
    const ClientReplicationStats& Stats() const { return stats_; }

private:
    bool ApplyComponent(Entity entity, ByteReader& in, bool is_update);

    World& world_;
    std::unordered_map<u32, Entity> by_id_;
    ClientReplicationStats stats_;
};

} // namespace aether::net
