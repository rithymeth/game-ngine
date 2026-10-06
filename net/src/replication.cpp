#include "aether/net/replication.h"

#include "aether/reflection/bytes.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cstring>

namespace aether::net {

namespace {

constexpr u8 kSnapshot = 1, kAck = 2;
constexpr u8 kNew = 1, kOwner = 2;
constexpr u16 kNone = 0xFFFF;

struct Writer {
    std::vector<u8>& out;
    void U8(u8 v) { out.push_back(v); }
    void U16(u16 v) { U8(static_cast<u8>(v)), U8(static_cast<u8>(v >> 8)); }
    void U32(u32 v) { U16(static_cast<u16>(v)), U16(static_cast<u16>(v >> 16)); }
    void F64(f64 v) {
        u64 bits;
        std::memcpy(&bits, &v, 8);
        U32(static_cast<u32>(bits)), U32(static_cast<u32>(bits >> 32));
    }
    void Bytes(const std::vector<u8>& b) { out.insert(out.end(), b.begin(), b.end()); }
};

struct Reader {
    std::span<const u8> in;
    usize at = 0;
    bool ok = true;
    bool Has(usize n) { return ok = ok && at + n <= in.size(); }
    u8 U8() { return Has(1) ? in[at++] : 0; }
    u16 U16() { u16 lo = U8(); return static_cast<u16>(lo | (U8() << 8)); }
    u32 U32() { u32 lo = U16(); return lo | (static_cast<u32>(U16()) << 16); }
    f64 F64() {
        u64 lo = U32();
        const u64 bits = lo | (static_cast<u64>(U32()) << 32);
        f64 v;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    std::vector<u8> Bytes(usize n) {
        if (!Has(n)) return {};
        std::vector<u8> b(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return b;
    }
};

struct FieldCost {
    u32 hash, field;
    usize bytes;
};

const ReplicatedComponent* FindComponent(const ReplicatedEntity& e, u32 hash) {
    for (const ReplicatedComponent& c : e.components)
        if (c.hash == hash) return &c;
    return nullptr;
}

// The entity's entry in a snapshot: what `cur` has that `base` (null: the
// client has not got it) does not. False when there is nothing to send.
bool WriteDelta(u32 net_id, const ReplicatedEntity& cur, const ReplicatedEntity* base, std::vector<u8>& out,
                std::vector<FieldCost>* costs = nullptr) {
    out.clear();
    if (costs) costs->clear();
    Writer w{out};
    w.U32(net_id);
    const bool owner_changed = base && base->owner != cur.owner;
    w.U8(static_cast<u8>((base ? 0 : kNew) | (owner_changed ? kOwner : 0)));
    if (!base || owner_changed) w.U32(cur.owner);
    if (!base) {
        const usize len = std::min<usize>(cur.archetype.size(), 255);
        w.U8(static_cast<u8>(len));
        out.insert(out.end(), cur.archetype.begin(), cur.archetype.begin() + static_cast<std::ptrdiff_t>(len));
    }
    const usize count_at = out.size();
    w.U8(0);
    u8 count = 0;
    for (const ReplicatedComponent& c : cur.components) {
        const ReplicatedComponent* old = base ? FindComponent(*base, c.hash) : nullptr;
        u32 mask = 0;
        for (usize f = 0; f < c.fields.size() && f < 32; ++f)
            if (!old || f >= old->fields.size() || old->fields[f] != c.fields[f]) mask |= 1u << f;
        if (mask == 0) continue;
        w.U32(c.hash), w.U32(mask);
        for (usize f = 0; f < c.fields.size() && f < 32; ++f)
            if (mask & (1u << f)) {
                w.U16(static_cast<u16>(c.fields[f].size())), w.Bytes(c.fields[f]);
                if (costs) costs->push_back({c.hash, static_cast<u32>(f), 2 + c.fields[f].size()});
            }
        ++count;
    }
    if (base)
        for (const ReplicatedComponent& c : base->components)
            if (!FindComponent(cur, c.hash)) w.U32(c.hash), w.U32(0), ++count; // removed
    out[count_at] = count;
    return !base || owner_changed || count > 0;
}

// Applies one entry to `state`; false when it does not fit (malformed, or
// a delta for an entity the baseline does not have).
bool ReadDelta(Reader& r, ReplicatedState& state) {
    const u32 net_id = r.U32();
    const u8 flags = r.U8();
    ReplicatedEntity* e;
    if (flags & kNew) {
        ReplicatedEntity fresh;
        fresh.owner = r.U32();
        const u8 len = r.U8();
        const std::vector<u8> name = r.Bytes(len);
        fresh.archetype.assign(name.begin(), name.end());
        e = &(state[net_id] = std::move(fresh));
    } else {
        auto it = state.find(net_id);
        if (it == state.end()) return false;
        e = &it->second;
        if (flags & kOwner) e->owner = r.U32();
    }
    const u8 count = r.U8();
    for (u8 i = 0; i < count && r.ok; ++i) {
        const u32 hash = r.U32(), mask = r.U32();
        auto it = std::find_if(e->components.begin(), e->components.end(),
                               [&](const ReplicatedComponent& c) { return c.hash == hash; });
        if (mask == 0) {
            if (it != e->components.end()) e->components.erase(it);
            continue;
        }
        if (it == e->components.end()) {
            ReplicatedComponent c;
            c.hash = hash;
            it = e->components.insert(std::upper_bound(e->components.begin(), e->components.end(), hash,
                                                       [](u32 h, const ReplicatedComponent& x) { return h < x.hash; }),
                                      std::move(c));
        }
        for (u32 f = 0; f < 32; ++f) {
            if (!(mask & (1u << f))) continue;
            if (it->fields.size() <= f) it->fields.resize(f + 1);
            const u16 len = r.U16();
            it->fields[f] = r.Bytes(len);
        }
    }
    return r.ok;
}

} // namespace

void SetNetArchetype(NetIdentity& identity, const std::string& archetype) {
    std::memset(identity.archetype, 0, sizeof(identity.archetype));
    std::strncpy(identity.archetype, archetype.c_str(), sizeof(identity.archetype) - 1);
}

u32 ComponentNameHash(const char* name) {
    u32 h = 2166136261u; // FNV-1a
    for (; *name; ++name) h = (h ^ static_cast<u8>(*name)) * 16777619u;
    return h;
}

const std::vector<ReplicatedComponentType>& ReplicatedComponentTypes() {
    // Rebuilt when more component types have registered since.
    static std::vector<ReplicatedComponentType> types;
    static ComponentId seen = 0;
    const ComponentId transform = GetComponentId<Transform>();
    const ComponentId identity = GetComponentId<NetIdentity>();
    const ComponentId count = RegisteredComponentCount();
    if (count != seen) {
        types.clear();
        for (ComponentId id = 0; id < count; ++id) {
            const ComponentInfo& info = GetComponentInfo(id);
            if (id == identity || !info.reflected) continue;
            ReplicatedComponentType t;
            t.id = id;
            t.hash = ComponentNameHash(info.name);
            for (const reflect::FieldInfo& f : info.reflected->fields)
                if ((id == transform || f.HasFlag(reflect::Field_Replicated)) && t.fields.size() < 32) t.fields.push_back(&f);
            if (!t.fields.empty()) types.push_back(std::move(t));
        }
        seen = count;
    }
    return types;
}

const char* ReplicatedComponentName(u32 hash) {
    const ReplicatedComponentType* t = FindReplicatedComponentType(hash);
    return t ? GetComponentInfo(t->id).name : "?";
}

const char* ReplicatedFieldName(u32 hash, u32 field) {
    const ReplicatedComponentType* t = FindReplicatedComponentType(hash);
    return t && field < t->fields.size() ? t->fields[field]->name : "?";
}

const ReplicatedComponentType* FindReplicatedComponentType(u32 hash) {
    for (const ReplicatedComponentType& t : ReplicatedComponentTypes())
        if (t.hash == hash) return &t;
    return nullptr;
}

// ---------------------------------------------------------------- server

ReplicationServer::ReplicationServer(World& world, NetHost& host, ReplicationConfig config)
    : world_(world), host_(host), config_(config) {
    config_.byte_budget = std::min(config_.byte_budget, Connection::kMaxFragment);
    config_.history = std::max<u32>(config_.history, 2);
}

void ReplicationServer::SetViewer(PeerId peer, const Vec3& position) {
    Peer& p = peers_[peer];
    p.has_viewer = true;
    p.viewer = position;
}

void ReplicationServer::ClearViewer(PeerId peer) {
    auto it = peers_.find(peer);
    if (it != peers_.end()) it->second.has_viewer = false;
}

bool ReplicationServer::HandleEvent(const NetEvent& event) {
    if (event.type == NetEventType::Disconnected) {
        peers_.erase(event.peer);
        return false;
    }
    if (event.type != NetEventType::Message || event.channel != config_.control_channel || event.data.size() != 3 ||
        event.data[0] != kAck)
        return false;
    auto it = peers_.find(event.peer);
    if (it == peers_.end()) return true;
    Peer& p = it->second;
    const u16 id = static_cast<u16>(event.data[1] | (event.data[2] << 8));
    const History& h = p.history[id % p.history.size()];
    if (h.id == id && (p.acked == kNone || SeqGreater(id, p.acked))) p.acked = p.stats.acked = id;
    return true;
}

Entity ReplicationServer::FindEntity(u32 net_id) const {
    auto it = entities_.find(net_id);
    return it == entities_.end() || !world_.IsAlive(it->second) ? Entity{} : it->second;
}

bool ReplicationServer::PeerHas(PeerId peer, u32 net_id) const {
    auto it = peers_.find(peer);
    if (it == peers_.end() || it->second.history.empty()) return false;
    const Peer& p = it->second;
    const u16 last = p.stats.last_snapshot;
    const History& h = p.history[last % p.history.size()];
    return h.id == last && h.state.count(net_id) > 0;
}

ReplicationServer::PeerStats ReplicationServer::Stats(PeerId peer) const {
    auto it = peers_.find(peer);
    return it == peers_.end() ? PeerStats{} : it->second.stats;
}

void ReplicationServer::Gather(ReplicatedState& current) {
    const ComponentId identity_id = GetComponentId<NetIdentity>();
    const ComponentId transform_id = GetComponentId<Transform>();
    const auto& types = ReplicatedComponentTypes();
    entities_.clear();
    meta_.clear();
    world_.ForEachArchetype([&](Archetype& a) {
        if (!a.Has(identity_id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const u32 n = a.ChunkEntityCount(c);
            Entity* ents = a.EntityArray(c);
            auto* ids = static_cast<NetIdentity*>(a.ComponentArray(c, identity_id));
            for (u32 row = 0; row < n; ++row) {
                NetIdentity& ni = ids[row];
                if (ni.net_id == 0) ni.net_id = next_net_id_++;
                entities_[ni.net_id] = ents[row];
                ReplicatedEntity& e = current[ni.net_id];
                e.owner = ni.owner;
                e.archetype.assign(ni.archetype, strnlen(ni.archetype, sizeof(ni.archetype)));
                Meta& m = meta_[ni.net_id];
                m.radius = ni.relevancy_radius;
                m.priority = ni.priority;
                for (const ReplicatedComponentType& t : types) {
                    if (!a.Has(t.id)) continue;
                    const u8* comp = static_cast<const u8*>(a.ComponentArray(c, t.id)) + row * GetComponentInfo(t.id).size;
                    ReplicatedComponent rc;
                    rc.hash = t.hash;
                    rc.fields.resize(t.fields.size());
                    for (usize f = 0; f < t.fields.size(); ++f)
                        reflect::AppendBinary(*t.fields[f]->type, t.fields[f]->Ptr(comp), rc.fields[f]);
                    e.components.push_back(std::move(rc));
                    if (t.id == transform_id) {
                        m.has_position = true;
                        m.position = reinterpret_cast<const Transform*>(comp)->position;
                    }
                }
                std::sort(e.components.begin(), e.components.end(),
                          [](const ReplicatedComponent& x, const ReplicatedComponent& y) { return x.hash < y.hash; });
            }
        }
    });
}

void ReplicationServer::Update(f64 now) {
    const std::vector<PeerId> connected = host_.Peers();
    // Forget peers the host no longer has (a viewer may be set while one is still connecting).
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (host_.Peer(it->first)) ++it;
        else it = peers_.erase(it);
    }
    if (now - last_send_ < config_.send_interval) return;
    last_send_ = now;
    ReplicatedState current;
    Gather(current);
    for (PeerId id : connected) {
        Peer& p = peers_[id];
        if (p.history.empty()) p.history.resize(config_.history);
        SendTo(id, p, current, now);
    }
}

void ReplicationServer::SendTo(PeerId id, Peer& peer, const ReplicatedState& current, f64 now) {
    const History* base = nullptr;
    if (peer.acked != kNone) {
        const History& h = peer.history[peer.acked % peer.history.size()];
        if (h.id == peer.acked) base = &h;
    }
    static const ReplicatedState kEmpty;
    const ReplicatedState& base_state = base ? base->state : kEmpty;

    // Where this client looks from.
    bool has_viewer = peer.has_viewer;
    Vec3 viewer = peer.viewer;
    if (!has_viewer)
        for (const auto& [net_id, e] : current)
            if (e.owner == id && meta_[net_id].has_position) {
                has_viewer = true, viewer = meta_[net_id].position;
                break;
            }
    auto relevant = [&](u32 net_id, const ReplicatedEntity& e) {
        const Meta& m = meta_[net_id];
        return !has_viewer || m.radius <= 0 || e.owner == id || !m.has_position ||
               (m.position - viewer).Length() <= m.radius;
    };

    const u16 snapshot = peer.next_snapshot++;
    if (peer.next_snapshot == kNone) peer.next_snapshot = 0;
    History& record = peer.history[snapshot % peer.history.size()];
    record.id = snapshot;
    record.state = base_state; // what the client will have: the baseline plus what is sent

    std::vector<u8> msg;
    Writer w{msg};
    w.U8(kSnapshot), w.U16(snapshot), w.U16(base ? base->id : kNone), w.F64(now);
    const usize despawn_count_at = msg.size();
    w.U16(0);
    // Despawns: in the baseline, gone or no longer relevant.
    u16 despawns = 0;
    std::vector<u32> gone;
    for (const auto& [net_id, e] : base_state) {
        auto it = current.find(net_id);
        if (it == current.end() || !relevant(net_id, it->second)) gone.push_back(net_id);
    }
    std::sort(gone.begin(), gone.end());
    const usize updates_reserve = 2 + 4; // the update count, and room for at least one id
    for (u32 net_id : gone) {
        if (msg.size() + 4 + updates_reserve > config_.byte_budget) break;
        w.U32(net_id);
        record.state.erase(net_id);
        peer.accumulators.erase(net_id);
        ++despawns;
    }
    msg[despawn_count_at] = static_cast<u8>(despawns), msg[despawn_count_at + 1] = static_cast<u8>(despawns >> 8);

    // Updates, by accumulated priority while they fit.
    struct Candidate {
        u32 net_id;
        f32 score;
        std::vector<u8> bytes;
        std::vector<FieldCost> costs;
        bool spawn;
    };
    std::vector<Candidate> candidates;
    std::vector<u8> entry;
    for (const auto& [net_id, e] : current) {
        if (!relevant(net_id, e)) continue;
        auto b = base_state.find(net_id);
        std::vector<FieldCost> costs;
        if (!WriteDelta(net_id, e, b == base_state.end() ? nullptr : &b->second, entry, &costs)) {
            peer.accumulators.erase(net_id);
            continue;
        }
        f32& acc = peer.accumulators[net_id];
        acc += std::max(meta_[net_id].priority, 0.001f);
        candidates.push_back({net_id, acc, entry, std::move(costs), b == base_state.end()});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score > b.score : a.net_id < b.net_id;
    });
    const usize update_count_at = msg.size();
    w.U16(0);
    u16 updates = 0;
    u32 deferred = 0;
    for (Candidate& c : candidates) {
        if (msg.size() + c.bytes.size() > config_.byte_budget) {
            ++deferred;
            continue;
        }
        w.Bytes(c.bytes);
        NetProfile::EntityCost& ec = profile_.entities[c.net_id];
        ec.archetype = current.at(c.net_id).archetype;
        ec.bytes += c.bytes.size();
        ++ec.updates;
        if (c.spawn) ++ec.spawns;
        for (const FieldCost& fc : c.costs) {
            NetProfile::ComponentCost& cc = ec.components[fc.hash];
            cc.bytes += fc.bytes;
            cc.fields[fc.field].bytes += fc.bytes;
            ++cc.fields[fc.field].sends;
        }
        record.state[c.net_id] = current.at(c.net_id);
        peer.accumulators.erase(c.net_id);
        ++updates;
    }
    msg[update_count_at] = static_cast<u8>(updates), msg[update_count_at + 1] = static_cast<u8>(updates >> 8);

    host_.Send(id, config_.snapshot_channel, msg);
    profile_.snapshot_bytes += msg.size();
    ++profile_.snapshots;
    profile_.despawns += despawns;
    peer.stats.last_snapshot = snapshot;
    peer.stats.last_bytes = msg.size();
    peer.stats.total_bytes += msg.size();
    peer.stats.entities_sent = updates;
    peer.stats.entities_deferred = deferred;
    peer.stats.despawns_sent = despawns;
}

// ---------------------------------------------------------------- client

ReplicationClient::ReplicationClient(World& world, NetHost& host, PeerId server, ReplicationConfig config)
    : world_(world), host_(host), server_(server), config_(config) {
    history_.resize(std::max<u32>(config_.history, 2));
}

bool ReplicationClient::ReadReplicated(u32 net_id, ComponentId component, void* out) const {
    auto it = applied_.find(net_id);
    if (it == applied_.end()) return false;
    const ComponentInfo& info = GetComponentInfo(component);
    if (!info.reflected) return false;
    const ReplicatedComponentType* t = FindReplicatedComponentType(ComponentNameHash(info.name));
    const ReplicatedComponent* c = t ? FindComponent(it->second, t->hash) : nullptr;
    if (!c) return false;
    for (usize f = 0; f < c->fields.size() && f < t->fields.size(); ++f)
        if (!c->fields[f].empty())
            reflect::ReadBinary(*t->fields[f]->type, t->fields[f]->Ptr(out), c->fields[f].data(), c->fields[f].size());
    return true;
}

void ReplicationClient::OnSpawn(const std::string& archetype, SpawnFn fn) { on_spawn_[archetype] = std::move(fn); }

Entity ReplicationClient::FindEntity(u32 net_id) const {
    auto it = entities_.find(net_id);
    return it == entities_.end() ? Entity{} : it->second;
}

bool ReplicationClient::HandleEvent(const NetEvent& event) {
    if (event.type != NetEventType::Message || event.peer != server_ || event.channel != config_.snapshot_channel ||
        event.data.empty() || event.data[0] != kSnapshot)
        return false;
    Receive(event.data);
    return true;
}

bool ReplicationClient::Receive(std::span<const u8> data) {
    Reader r{data};
    r.U8();
    const u16 id = r.U16(), baseline = r.U16();
    const f64 time = r.F64();
    if (!r.ok) return false;
    if (any_ && !SeqGreater(id, last_)) return false; // stale
    ReplicatedState state;
    if (baseline != kNone) {
        const History& h = history_[baseline % history_.size()];
        if (h.id != baseline) {
            ++stats_.undecodable;
            return false;
        }
        state = h.state;
    }
    const u16 despawns = r.U16();
    for (u16 i = 0; i < despawns && r.ok; ++i) state.erase(r.U32());
    const u16 updates = r.U16();
    for (u16 i = 0; i < updates && r.ok; ++i)
        if (!ReadDelta(r, state)) {
            ++stats_.undecodable;
            return false;
        }
    if (!r.ok) {
        ++stats_.undecodable;
        return false;
    }
    Apply(state);
    History& slot = history_[id % history_.size()];
    slot.id = id;
    slot.state = std::move(state);
    last_ = id, any_ = true, server_time_ = time;
    ++stats_.snapshots;
    const u8 ack[3] = {kAck, static_cast<u8>(id), static_cast<u8>(id >> 8)};
    host_.Send(server_, config_.control_channel, ack);
    return true;
}

void ReplicationClient::Apply(const ReplicatedState& state) {
    // Gone since the last snapshot.
    for (auto it = applied_.begin(); it != applied_.end();) {
        if (state.count(it->first)) {
            ++it;
            continue;
        }
        auto e = entities_.find(it->first);
        if (e != entities_.end()) {
            if (on_despawn_) on_despawn_(world_, e->second);
            if (world_.IsAlive(e->second)) world_.DestroyEntity(e->second);
            entities_.erase(e);
            ++stats_.despawned;
        }
        it = applied_.erase(it);
    }
    std::optional<PeerInfo> server = host_.Peer(server_);
    const u32 my_id = server ? server->remote_id : 0;
    for (const auto& [net_id, e] : state) {
        auto prev_it = applied_.find(net_id);
        const ReplicatedEntity* prev = prev_it == applied_.end() ? nullptr : &prev_it->second;
        Entity entity = FindEntity(net_id);
        if (entity.IsNull() || !world_.IsAlive(entity)) {
            NetIdentity ni;
            ni.net_id = net_id;
            ni.owner = e.owner;
            SetNetArchetype(ni, e.archetype);
            ni.locally_owned = e.owner != kNoPeer && e.owner == my_id;
            entity = world_.CreateEntity(ni);
            entities_[net_id] = entity;
            prev = nullptr;
            ++stats_.spawned;
            auto hook = on_spawn_.find(e.archetype);
            if (hook == on_spawn_.end()) hook = on_spawn_.find("");
            if (hook != on_spawn_.end() && hook->second) hook->second(world_, entity, ni);
            if (!world_.IsAlive(entity)) continue;
        } else if (prev && prev->owner != e.owner) {
            if (auto* ni = world_.GetComponent<NetIdentity>(entity)) {
                ni->owner = e.owner;
                ni->locally_owned = e.owner != kNoPeer && e.owner == my_id;
            }
        }
        if (prev)
            for (const ReplicatedComponent& c : prev->components)
                if (!FindComponent(e, c.hash))
                    if (const ReplicatedComponentType* t = FindReplicatedComponentType(c.hash))
                        world_.RemoveComponentRaw(entity, t->id);
        for (const ReplicatedComponent& c : e.components) {
            const ReplicatedComponentType* t = FindReplicatedComponentType(c.hash);
            if (!t) continue; // a component this build does not know
            if (!world_.HasComponentRaw(entity, t->id)) world_.AddComponentRaw(entity, t->id);
            const ReplicatedComponent* old = prev ? FindComponent(*prev, c.hash) : nullptr;
            if (prev && e.owner != kNoPeer && e.owner == my_id &&
                std::find(predicted_.begin(), predicted_.end(), t->id) != predicted_.end())
                continue; // ours to predict
            void* comp = world_.GetComponentRaw(entity, t->id);
            for (usize f = 0; f < c.fields.size() && f < t->fields.size(); ++f) {
                if (c.fields[f].empty() || (old && f < old->fields.size() && old->fields[f] == c.fields[f])) continue;
                reflect::ReadBinary(*t->fields[f]->type, t->fields[f]->Ptr(comp), c.fields[f].data(), c.fields[f].size());
                ++stats_.fields_written;
            }
        }
    }
    applied_ = state;
}

} // namespace aether::net
