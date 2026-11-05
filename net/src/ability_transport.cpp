#include "aether/net/ability_transport.h"

#include <algorithm>

namespace aether::net {

namespace {

constexpr u8 kRequest = 0x41; // 'A'
constexpr u8 kReply = 0x61;   // 'a'

void PutU32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 24));
}

// Bounds-checked reader: ok goes false the first time a field would run off the end, so a
// truncated or hostile packet is reported rather than read past. The type byte is already known.
struct In {
    const std::vector<u8>& data;
    usize at = 1;
    bool ok = true;
    explicit In(const std::vector<u8>& d) : data(d) {}

    u8 U8() {
        if (!ok || at + 1 > data.size()) {
            ok = false;
            return 0;
        }
        return data[at++];
    }
    u32 U32() {
        if (!ok || at + 4 > data.size()) {
            ok = false;
            return 0;
        }
        const u32 v = static_cast<u32>(data[at]) | (static_cast<u32>(data[at + 1]) << 8) |
                      (static_cast<u32>(data[at + 2]) << 16) | (static_cast<u32>(data[at + 3]) << 24);
        at += 4;
        return v;
    }
    std::string Str(usize max) {
        const u8 n = U8();
        if (!ok || n > max || at + n > data.size()) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data.data() + at), n);
        at += n;
        return s;
    }
};

void PutStr(std::vector<u8>& out, const std::string& s) {
    out.push_back(static_cast<u8>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

} // namespace

// ---------------------------------------------------------------- server

AbilityServer::AbilityServer(World& world, NetHost& host, ReplicationServer& replication, AbilityOps ops,
                             AbilityChannelConfig config)
    : world_(world), host_(host), replication_(replication), ops_(std::move(ops)), config_(config) {}

bool AbilityServer::HandleEvent(const NetEvent& event) {
    if (event.type != NetEventType::Message || event.channel != config_.request_channel || event.data.empty() ||
        event.data[0] != kRequest)
        return false;
    In in{event.data};
    const u32 sequence = in.U32();
    const u32 net_id = in.U32();
    const std::string ability = in.Str(config_.max_ability_name);
    if (!in.ok) {
        ++stats_.malformed;
        return true;
    }
    const Entity entity = replication_.FindEntity(net_id);
    if (entity.IsNull() || !world_.IsAlive(entity)) {
        ++stats_.unknown_entity;
        return true;
    }
    const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
    if (ni == nullptr || ni->owner != event.peer) {
        // Only the owner may ask for its own entity's abilities; anything else is a spoof.
        ++stats_.not_owner;
        return true;
    }
    const u32 handle = ops_.activate ? ops_.activate(net_id, ability) : 0;
    const bool accepted = handle != 0;
    if (accepted) {
        if (ops_.commit) ops_.commit(handle);
        ++stats_.accepted;
    } else {
        ++stats_.refused;
    }
    std::vector<u8> reply{kReply};
    PutU32(reply, sequence);
    reply.push_back(accepted ? 1 : 0);
    host_.Send(event.peer, config_.reply_channel, reply);
    return true;
}

// ---------------------------------------------------------------- client

AbilityClient::AbilityClient(World& world, NetHost& host, ReplicationClient& replication, PeerId server,
                             AbilityOps ops, AbilityChannelConfig config)
    : world_(world), host_(host), replication_(replication), server_(server), ops_(std::move(ops)), config_(config) {}

bool AbilityClient::Request(Entity entity, const std::string& ability) {
    if (!world_.IsAlive(entity) || ability.size() > config_.max_ability_name) return false;
    const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
    if (ni == nullptr || !ni->locally_owned) return false;
    const u32 handle = ops_.activate ? ops_.activate(ni->net_id, ability) : 0;
    if (handle == 0) {
        // Refused locally: there is nothing for the server to confirm, so nothing is sent.
        ++stats_.local_failures;
        return false;
    }
    ++stats_.predicted;
    Entry p;
    p.sequence = next_++;
    p.handle = handle;
    pending_.push_back(p);

    std::vector<u8> msg{kRequest};
    PutU32(msg, p.sequence);
    PutU32(msg, ni->net_id);
    PutStr(msg, ability);
    host_.Send(server_, config_.request_channel, msg);
    return true;
}

bool AbilityClient::HandleEvent(const NetEvent& event) {
    if (event.type != NetEventType::Message || event.peer != server_ || event.channel != config_.reply_channel ||
        event.data.empty() || event.data[0] != kReply)
        return false;
    In in{event.data};
    const u32 sequence = in.U32();
    const bool accepted = in.U8() != 0;
    if (!in.ok) {
        ++stats_.malformed;
        return true;
    }
    const auto at = std::find_if(pending_.begin(), pending_.end(),
                                 [sequence](const Entry& p) { return p.sequence == sequence; });
    if (at == pending_.end()) {
        // Unknown or already answered: a reply that arrives twice must not commit a cost twice.
        ++stats_.stale_replies;
        return true;
    }
    const u32 handle = at->handle;
    pending_.erase(at);
    if (accepted) {
        ++stats_.accepted;
        if (ops_.commit) ops_.commit(handle);
    } else {
        ++stats_.refused;
        if (ops_.cancel) ops_.cancel(handle);
    }
    return true;
}

} // namespace aether::net