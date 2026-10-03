#include "aether/net/rpc.h"

#include "aether/core/log.h"
#include "aether/reflection/bytes.h"

#include <vector>

namespace aether::net {

namespace {

constexpr u8 kRpc = 3;

void Put32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

struct In {
    std::span<const u8> data;
    usize at = 0;
    bool ok = true;
    bool Has(usize n) { return ok = ok && at + n <= data.size(); }
    u8 U8() { return Has(1) ? data[at++] : 0; }
    u16 U16() { u16 lo = U8(); return static_cast<u16>(lo | (U8() << 8)); }
    u32 U32() { u32 lo = U16(); return lo | (static_cast<u32>(U16()) << 16); }
};

ComponentId FindComponentByHash(u32 hash) {
    for (ComponentId id = 0; id < RegisteredComponentCount(); ++id) {
        const ComponentInfo& info = GetComponentInfo(id);
        if (info.reflected && ComponentNameHash(info.name) == hash) return id;
    }
    return kInvalidComponentId;
}

const reflect::FunctionInfo* FindFunctionByHash(const reflect::TypeInfo& type, u32 hash) {
    for (const reflect::FunctionInfo& f : type.functions)
        if (ComponentNameHash(f.name) == hash) return &f;
    return nullptr;
}

} // namespace

RpcRouter::RpcRouter(World& world, NetHost& host, ReplicationServer& server, RpcConfig config)
    : world_(world), host_(host), replication_server_(&server), config_(config) {
    SetRemoteCallRouter(world_, this);
}

RpcRouter::RpcRouter(World& world, NetHost& host, ReplicationClient& client, PeerId server, RpcConfig config)
    : world_(world), host_(host), replication_client_(&client), server_(server), config_(config) {
    SetRemoteCallRouter(world_, this);
}

RpcRouter::~RpcRouter() {
    if (GetRemoteCallRouter(world_) == this) SetRemoteCallRouter(world_, nullptr);
}

RemoteCallResult RpcRouter::Route(Entity entity, const reflect::TypeInfo& component, const reflect::FunctionInfo& function,
                                  std::span<reflect::Any> args) {
    const NetIdentity* ni = world_.IsAlive(entity) ? world_.GetComponent<NetIdentity>(entity) : nullptr;
    if (IsServer()) {
        if (function.HasFlag(reflect::Fn_Server)) return RemoteCallResult::RunLocally;
        if (!ni || ni->net_id == 0) return RemoteCallResult::RunLocally; // not networked (yet): nobody else has it
        if (function.HasFlag(reflect::Fn_Client)) {
            if (ni->owner == kNoPeer) return RemoteCallResult::RunLocally;
            return Send(ni->owner, ni->net_id, component, function, args) ? RemoteCallResult::Sent
                                                                           : RemoteCallResult::Refused;
        }
        // Multicast: to everyone who has it, and here.
        for (PeerId peer : host_.Peers())
            if (replication_server_->PeerHas(peer, ni->net_id)) Send(peer, ni->net_id, component, function, args);
        return RemoteCallResult::RunLocally;
    }
    if (!function.HasFlag(reflect::Fn_Server)) return RemoteCallResult::RunLocally;
    if (!ni || !ni->locally_owned) {
        ++stats_.refused;
        AETHER_LOG_WARN("Net", "Refused the server call %s.%s: this client doesn't own the entity", component.name,
                        function.name);
        return RemoteCallResult::Refused;
    }
    return Send(server_, ni->net_id, component, function, args) ? RemoteCallResult::Sent : RemoteCallResult::Refused;
}

bool RpcRouter::Send(PeerId peer, u32 net_id, const reflect::TypeInfo& component, const reflect::FunctionInfo& function,
                     std::span<reflect::Any> args) {
    if (args.size() != function.params.size() || args.size() > 255) return false;
    std::vector<u8> msg;
    msg.push_back(kRpc);
    Put32(msg, net_id);
    Put32(msg, ComponentNameHash(component.name));
    Put32(msg, ComponentNameHash(function.name));
    msg.push_back(static_cast<u8>(args.size()));
    std::vector<u8> bytes;
    for (usize i = 0; i < args.size(); ++i) {
        if (args[i].Type() != function.params[i].type) return false;
        bytes.clear();
        reflect::AppendBinary(*args[i].Type(), args[i].Data(), bytes);
        if (bytes.size() > 0xFFFF) return false;
        msg.push_back(static_cast<u8>(bytes.size())), msg.push_back(static_cast<u8>(bytes.size() >> 8));
        msg.insert(msg.end(), bytes.begin(), bytes.end());
    }
    const u8 channel = function.HasFlag(reflect::Fn_Unreliable) ? config_.unreliable_channel : config_.reliable_channel;
    if (!host_.Send(peer, channel, msg)) return false;
    ++stats_.sent;
    return true;
}

bool RpcRouter::HandleEvent(const NetEvent& event) {
    if (event.type != NetEventType::Message || event.data.empty() || event.data[0] != kRpc ||
        (event.channel != config_.reliable_channel && event.channel != config_.unreliable_channel))
        return false;
    if (!IsServer() && event.peer != server_) return false;
    Receive(event.peer, event.data);
    return true;
}

void RpcRouter::Receive(PeerId from, std::span<const u8> data) {
    In in{data};
    in.U8();
    const u32 net_id = in.U32(), component_hash = in.U32(), function_hash = in.U32();
    const u8 argc = in.U8();
    if (!in.ok) {
        ++stats_.malformed;
        return;
    }
    const Entity entity = IsServer() ? replication_server_->FindEntity(net_id) : replication_client_->FindEntity(net_id);
    const ComponentId component = FindComponentByHash(component_hash);
    if (entity.IsNull() || !world_.IsAlive(entity) || component == kInvalidComponentId ||
        !world_.HasComponentRaw(entity, component)) {
        ++stats_.unknown;
        return;
    }
    const reflect::TypeInfo& type = *GetComponentInfo(component).reflected;
    const reflect::FunctionInfo* function = FindFunctionByHash(type, function_hash);
    if (!function || function->HasFlag(reflect::Fn_Static)) {
        ++stats_.unknown;
        return;
    }
    // What may arrive: on the server, server calls from the owner; on a client, client and multicast calls.
    if (IsServer()) {
        const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
        if (!function->HasFlag(reflect::Fn_Server) || !ni || ni->owner != from) {
            ++stats_.rejected;
            AETHER_LOG_WARN("Net", "Rejected %s.%s from peer %u: it doesn't own the entity, or it isn't a server call",
                            type.name, function->name, from);
            return;
        }
    } else if (!function->HasFlag(reflect::Fn_Client) && !function->HasFlag(reflect::Fn_Multicast)) {
        ++stats_.rejected;
        return;
    }
    if (argc != function->params.size()) {
        ++stats_.malformed;
        return;
    }
    std::vector<reflect::Any> args;
    args.reserve(argc);
    for (const reflect::ParamInfo& param : function->params) {
        const u16 len = in.U16();
        if (!in.Has(len)) break;
        args.push_back(reflect::Any::DefaultOf(*param.type));
        if (!args.back().HasValue() || !reflect::ReadBinary(*param.type, args.back().Data(), data.data() + in.at, len)) {
            in.ok = false;
            break;
        }
        in.at += len;
    }
    if (!in.ok || args.size() != function->params.size()) {
        ++stats_.malformed;
        return;
    }
    caller_ = from;
    const bool ran = function->Invoke(world_.GetComponentRaw(entity, component), args);
    caller_ = kNoPeer;
    if (ran) ++stats_.received;
    else ++stats_.malformed;
}

} // namespace aether::net
