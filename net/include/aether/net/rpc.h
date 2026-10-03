#pragma once

#include "aether/net/replication.h"
#include "aether/reflection/any.h"

#include <string_view>
#include <unordered_map>

namespace aether::net {

using reflect::Any;

// Remote procedure calls (Phase 22 step 3). A reflected method on a component
// of a replicated entity (one with a NetIdentity), flagged
//
//   Fn_ServerRPC     - a client calls it; it runs on the server
//   Fn_ClientRPC     - the server calls it; it runs on the entity's owning client
//   Fn_MulticastRPC  - the server calls it; it runs on every client that has the entity
//
// (add Fn_RpcUnreliable to skip the reliable channel). Arguments are encoded
// by reflected type (the same codec as replication), so any reflected type
// works. Return values are dropped. The server never trusts a client: it
// checks the function is a Server RPC, that the caller owns the entity, that
// the arguments decode exactly, and applies a per-peer rate limit.
//
// RPC messages start with kRpcMessage; the game routes them with
// IsRpcMessage next to ReplicationClient::IsReplicationMessage.
//
//   [kRpcMessage][varint net_id][u32 component hash][u32 function hash][args...]

inline constexpr u8 kRpcMessage = 0xA1;

enum class RpcResult : u8 {
    Ok,
    Malformed,       // the bytes don't parse
    UnknownEntity,   // no such network id
    UnknownFunction, // no such component or RPC function
    NotAllowed,      // wrong direction, or the caller doesn't own the entity
    BadArguments,    // an argument type has no default value, or extra bytes follow
    RateLimited,
};

inline bool IsRpcMessage(const NetEvent& event) {
    return event.type == NetEventType::Message && !event.data.empty() && event.data[0] == kRpcMessage;
}

struct RpcStats {
    u64 sent = 0;
    u64 executed = 0;
    u64 rejected = 0;
};

struct RpcComponent {
    ComponentId id = kInvalidComponentId;
    u32 hash = 0;
    const reflect::TypeInfo* type = nullptr;
};

// Every registered component with at least one RPC function.
const std::vector<RpcComponent>& RpcComponents();

class RpcServer {
public:
    RpcServer(World& world, NetEndpoint& endpoint, ReplicationServer& replication)
        : world_(world), endpoint_(endpoint), replication_(replication) {}

    // Calls an RPC from the server. Client RPC: sent to the entity's owner.
    // Multicast: sent to every connected peer that knows the entity. Server
    // RPC: runs here, directly. False if the entity has no network id yet,
    // the function isn't an RPC, the arguments don't match its parameters, or
    // the message doesn't fit.
    bool Call(Entity entity, std::string_view component, std::string_view function, std::span<const Any> args = {});
    template <typename... Args>
    bool Call(Entity entity, std::string_view component, std::string_view function, Args&&... args) {
        std::vector<Any> list;
        (list.emplace_back(std::forward<Args>(args)), ...);
        return Call(entity, component, function, std::span<const Any>(list));
    }

    // Handles a Server RPC message received from `from`; `now` is the
    // caller's clock in seconds (for the rate limit).
    RpcResult Handle(NetAddress from, std::span<const u8> message, f64 now);

    usize max_calls_per_second = 100; // per peer; 0 = unlimited
    const RpcStats& Stats() const { return stats_; }

private:
    struct Window {
        f64 start = 0.0;
        usize count = 0;
    };
    World& world_;
    NetEndpoint& endpoint_;
    ReplicationServer& replication_;
    std::unordered_map<NetAddress, Window> windows_;
    RpcStats stats_;
};

class RpcClient {
public:
    RpcClient(World& world, NetEndpoint& endpoint, NetAddress server, ReplicationClient& replication)
        : world_(world), endpoint_(endpoint), server_(server), replication_(replication) {}

    // Calls a Server RPC on a replicated entity of this client's world.
    bool Call(Entity entity, std::string_view component, std::string_view function, std::span<const Any> args = {});
    template <typename... Args>
    bool Call(Entity entity, std::string_view component, std::string_view function, Args&&... args) {
        std::vector<Any> list;
        (list.emplace_back(std::forward<Args>(args)), ...);
        return Call(entity, component, function, std::span<const Any>(list));
    }

    // Handles a Client or Multicast RPC message from the server.
    RpcResult Handle(std::span<const u8> message);

    const RpcStats& Stats() const { return stats_; }

private:
    World& world_;
    NetEndpoint& endpoint_;
    NetAddress server_;
    ReplicationClient& replication_;
    RpcStats stats_;
};

} // namespace aether::net
