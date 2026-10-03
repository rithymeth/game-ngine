#pragma once

#include "aether/ecs/remote_call.h"
#include "aether/net/replication.h"

namespace aether::net {

struct RpcConfig {
    u8 reliable_channel = 0;   // a ReliableOrdered channel
    u8 unreliable_channel = 1; // for Fn_Unreliable calls
};

struct RpcStats {
    u32 sent = 0;
    u32 received = 0;    // and run
    u32 refused = 0;     // calls from here not allowed (a server call on an entity this client doesn't own)
    u32 rejected = 0;    // incoming calls not allowed (from a client that doesn't own the entity, or the wrong kind)
    u32 unknown = 0;     // incoming calls for an entity, component or function this side doesn't have
    u32 malformed = 0;
};

// Routes remote calls over a NetHost (Phase 22 step 3,
// docs/design/PHASE_SPECS.md §22.3). One is made on the server and one on
// each client; it installs itself as its world's RemoteCallRouter, so calls
// from C++ (CallFunction), Blueprints and Luau all come here:
// - Fn_Server, called on a client: sent to the server when this client
//   owns the entity, refused otherwise. The server runs it only when the
//   caller owns the entity. Called on the server: runs.
// - Fn_Client, called on the server: sent to the owning client (runs on
//   the server when no client owns it). Called on a client: runs there.
// - Fn_Multicast, called on the server: runs there and on every client
//   that has the entity. Called on a client: runs only there.
// Entities are named by their NetIdentity net id, components and functions
// by name hashes, and arguments go in reflection's binary form. Remote
// calls send nothing back; their return values are not used.
class RpcRouter final : public RemoteCallRouter {
public:
    RpcRouter(World& world, NetHost& host, ReplicationServer& server, RpcConfig config = {});
    RpcRouter(World& world, NetHost& host, ReplicationClient& client, PeerId server, RpcConfig config = {});
    ~RpcRouter() override;
    RpcRouter(const RpcRouter&) = delete;
    RpcRouter& operator=(const RpcRouter&) = delete;

    RemoteCallResult Route(Entity entity, const reflect::TypeInfo& component, const reflect::FunctionInfo& function,
                           std::span<reflect::Any> args) override;

    // Runs the calls in the host's events; true when `event` was one.
    bool HandleEvent(const NetEvent& event);

    bool IsServer() const { return replication_server_ != nullptr; }
    // While an incoming call runs: who sent it (kNoPeer otherwise).
    PeerId Caller() const { return caller_; }
    const RpcStats& Stats() const { return stats_; }

private:
    bool Send(PeerId peer, u32 net_id, const reflect::TypeInfo& component, const reflect::FunctionInfo& function,
              std::span<reflect::Any> args);
    void Receive(PeerId from, std::span<const u8> data);

    World& world_;
    NetHost& host_;
    ReplicationServer* replication_server_ = nullptr;
    ReplicationClient* replication_client_ = nullptr;
    PeerId server_ = kNoPeer;
    RpcConfig config_;
    PeerId caller_ = kNoPeer;
    RpcStats stats_;
};

} // namespace aether::net
