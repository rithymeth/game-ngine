#include "aether/net/rpc.h"

#include <algorithm>

namespace aether::net {

namespace {

constexpr u32 kRpcMask = reflect::Fn_ServerRPC | reflect::Fn_ClientRPC | reflect::Fn_MulticastRPC;

struct Registry {
    std::vector<RpcComponent> components;
    ComponentId scanned = 0;
};

Registry& Refresh() {
    static Registry registry;
    for (; registry.scanned < RegisteredComponentCount(); ++registry.scanned) {
        const ComponentInfo& info = GetComponentInfo(registry.scanned);
        if (info.reflected == nullptr) continue;
        const bool any = std::any_of(info.reflected->functions.begin(), info.reflected->functions.end(),
                                     [](const reflect::FunctionInfo& f) { return (f.flags & kRpcMask) != 0; });
        if (!any) continue;
        registry.components.push_back({registry.scanned, ComponentHash(info.name), info.reflected});
    }
    return registry;
}

const RpcComponent* FindByName(std::string_view name) {
    for (const RpcComponent& c : Refresh().components) {
        if (name == c.type->name) return &c;
    }
    return nullptr;
}

const RpcComponent* FindByHash(u32 hash) {
    for (const RpcComponent& c : Refresh().components) {
        if (c.hash == hash) return &c;
    }
    return nullptr;
}

const reflect::FunctionInfo* FindFunctionByHash(const reflect::TypeInfo& type, u32 hash, u32 flags) {
    for (const reflect::FunctionInfo& f : type.functions) {
        if ((f.flags & flags) != 0 && ComponentHash(f.name) == hash) return &f;
    }
    return nullptr;
}

// Resolves "Component"/"function" for a call: the function must carry one of `flags`.
bool Resolve(std::string_view component, std::string_view function, u32 flags, const RpcComponent*& comp,
             const reflect::FunctionInfo*& fn) {
    comp = FindByName(component);
    if (comp == nullptr) return false;
    fn = comp->type->FindFunction(function);
    return fn != nullptr && (fn->flags & flags) != 0;
}

bool Build(u32 net_id, const RpcComponent& comp, const reflect::FunctionInfo& fn, std::span<const Any> args, usize max_size,
           std::vector<u8>& out) {
    if (args.size() != fn.params.size()) return false;
    ByteWriter w;
    w.U8(kRpcMessage);
    w.Varint(net_id);
    w.U32(comp.hash);
    w.U32(ComponentHash(fn.name));
    for (usize i = 0; i < args.size(); ++i) {
        if (args[i].Type() != fn.params[i].type) return false;
        EncodeValue(*fn.params[i].type, args[i].Data(), w);
    }
    if (w.Size() > max_size) return false;
    out = w.Take();
    return true;
}

// Parses the header, finds the function (which must carry one of `flags`), decodes
// the arguments and runs it on `entity`'s component.
struct Header {
    u32 net_id = 0;
    const RpcComponent* comp = nullptr;
    const reflect::FunctionInfo* fn = nullptr;
};

RpcResult ParseHeader(ByteReader& in, u32 flags, Header& h) {
    if (in.U8() != kRpcMessage) return RpcResult::Malformed;
    h.net_id = static_cast<u32>(in.Varint());
    const u32 comp_hash = in.U32();
    const u32 fn_hash = in.U32();
    if (!in.Ok()) return RpcResult::Malformed;
    h.comp = FindByHash(comp_hash);
    if (h.comp == nullptr) return RpcResult::UnknownFunction;
    h.fn = FindFunctionByHash(*h.comp->type, fn_hash, kRpcMask);
    if (h.fn == nullptr) return RpcResult::UnknownFunction;
    if ((h.fn->flags & flags) == 0) return RpcResult::NotAllowed; // right function, wrong direction
    return RpcResult::Ok;
}

RpcResult Execute(World& world, Entity entity, const Header& h, ByteReader& in) {
    std::vector<Any> args;
    args.reserve(h.fn->params.size());
    for (const reflect::ParamInfo& p : h.fn->params) {
        Any value = Any::DefaultOf(*p.type);
        if (!value.HasValue()) return RpcResult::BadArguments;
        if (!DecodeValue(*p.type, value.Data(), in)) return RpcResult::Malformed;
        args.push_back(std::move(value));
    }
    if (in.Remaining() != 0) return RpcResult::BadArguments;
    if (!world.IsAlive(entity)) return RpcResult::UnknownEntity;
    void* object = world.GetComponentRaw(entity, h.comp->id);
    if (object == nullptr) return RpcResult::UnknownFunction; // the entity lacks that component
    return h.fn->Invoke(object, args) ? RpcResult::Ok : RpcResult::BadArguments;
}

} // namespace

const std::vector<RpcComponent>& RpcComponents() { return Refresh().components; }

// --- server -----------------------------------------------------------------

bool RpcServer::Call(Entity entity, std::string_view component, std::string_view function, std::span<const Any> args) {
    const NetIdentity* identity = world_.IsAlive(entity) ? world_.GetComponent<NetIdentity>(entity) : nullptr;
    if (identity == nullptr || identity->net_id == 0) return false;
    const RpcComponent* comp = nullptr;
    const reflect::FunctionInfo* fn = nullptr;
    if (!Resolve(component, function, kRpcMask, comp, fn)) return false;

    if (fn->flags & reflect::Fn_ServerRPC) { // already on the server: just run it
        if (args.size() != fn->params.size()) return false;
        std::vector<Any> copy(args.begin(), args.end());
        void* object = world_.GetComponentRaw(entity, comp->id);
        if (object == nullptr || !fn->Invoke(object, copy)) return false;
        ++stats_.executed;
        return true;
    }

    std::vector<u8> message;
    if (!Build(identity->net_id, *comp, *fn, args, endpoint_.Config().max_message_size, message)) return false;
    const Channel channel = (fn->flags & reflect::Fn_RpcUnreliable) ? Channel::Unreliable : Channel::ReliableOrdered;
    bool sent = false;
    if (fn->flags & reflect::Fn_ClientRPC) {
        if (identity->owner == 0 || !endpoint_.IsConnected(identity->owner)) return false;
        sent = endpoint_.Send(identity->owner, channel, message);
    } else {
        for (NetAddress peer : endpoint_.ConnectedPeers()) {
            if (replication_.Knows(peer, identity->net_id)) sent = endpoint_.Send(peer, channel, message) || sent;
        }
    }
    if (sent) ++stats_.sent;
    return sent;
}

RpcResult RpcServer::Handle(NetAddress from, std::span<const u8> message, f64 now) {
    auto reject = [&](RpcResult r) {
        ++stats_.rejected;
        return r;
    };
    if (max_calls_per_second != 0) {
        Window& w = windows_[from];
        if (now - w.start >= 1.0) w = {now, 0};
        if (++w.count > max_calls_per_second) return reject(RpcResult::RateLimited);
    }

    ByteReader in(message);
    Header h;
    if (RpcResult r = ParseHeader(in, reflect::Fn_ServerRPC, h); r != RpcResult::Ok) return reject(r);
    const Entity entity = replication_.EntityOf(h.net_id);
    if (entity.IsNull() || !world_.IsAlive(entity)) return reject(RpcResult::UnknownEntity);
    const NetIdentity* identity = world_.GetComponent<NetIdentity>(entity);
    if (identity == nullptr || identity->owner != from) return reject(RpcResult::NotAllowed); // only the owner may call
    const RpcResult r = Execute(world_, entity, h, in);
    if (r != RpcResult::Ok) return reject(r);
    ++stats_.executed;
    return r;
}

// --- client -----------------------------------------------------------------

bool RpcClient::Call(Entity entity, std::string_view component, std::string_view function, std::span<const Any> args) {
    const NetIdentity* identity = world_.IsAlive(entity) ? world_.GetComponent<NetIdentity>(entity) : nullptr;
    if (identity == nullptr || identity->net_id == 0) return false;
    const RpcComponent* comp = nullptr;
    const reflect::FunctionInfo* fn = nullptr;
    if (!Resolve(component, function, reflect::Fn_ServerRPC, comp, fn)) return false;
    std::vector<u8> message;
    if (!Build(identity->net_id, *comp, *fn, args, endpoint_.Config().max_message_size, message)) return false;
    const Channel channel = (fn->flags & reflect::Fn_RpcUnreliable) ? Channel::Unreliable : Channel::ReliableOrdered;
    if (!endpoint_.Send(server_, channel, message)) return false;
    ++stats_.sent;
    return true;
}

RpcResult RpcClient::Handle(std::span<const u8> message) {
    auto reject = [&](RpcResult r) {
        ++stats_.rejected;
        return r;
    };
    ByteReader in(message);
    Header h;
    if (RpcResult r = ParseHeader(in, reflect::Fn_ClientRPC | reflect::Fn_MulticastRPC, h); r != RpcResult::Ok) return reject(r);
    const Entity entity = replication_.EntityOf(h.net_id);
    if (entity.IsNull()) return reject(RpcResult::UnknownEntity);
    const RpcResult r = Execute(world_, entity, h, in);
    if (r != RpcResult::Ok) return reject(r);
    ++stats_.executed;
    return r;
}

} // namespace aether::net
