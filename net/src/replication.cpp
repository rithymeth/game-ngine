#include "aether/net/replication.h"

#include "aether/core/log.h"

#include <algorithm>
#include <cstring>

namespace aether::net {

namespace {

enum RecordKind : u8 { kSpawn = 1, kUpdate = 2, kRemove = 3, kDespawn = 4 };
constexpr u8 kFullComponent = 1; // a component record that is a complete state (spawn or add)
constexpr usize kMaxReplicatedFields = 64;

i64 ReadSigned(const void* p, u32 size) {
    switch (size) {
    case 1: return *static_cast<const i8*>(p);
    case 2: { i16 v; std::memcpy(&v, p, 2); return v; }
    case 4: { i32 v; std::memcpy(&v, p, 4); return v; }
    default: { i64 v = 0; std::memcpy(&v, p, std::min<u32>(size, 8)); return v; }
    }
}

u64 ReadUnsigned(const void* p, u32 size) {
    u64 v = 0;
    std::memcpy(&v, p, std::min<u32>(size, 8)); // little-endian
    return v;
}

void WriteInt(void* p, u32 size, u64 v) { std::memcpy(p, &v, std::min<u32>(size, 8)); }

u64 ZigZag(i64 v) { return (static_cast<u64>(v) << 1) ^ static_cast<u64>(v >> 63); }
i64 UnZigZag(u64 v) { return static_cast<i64>(v >> 1) ^ -static_cast<i64>(v & 1); }

const reflect::TypeInfo& IntegerTypeOf(const reflect::TypeInfo& type) {
    return type.kind == reflect::TypeKind::Enum && type.underlying != nullptr ? *type.underlying : type;
}

} // namespace

void EncodeValue(const reflect::TypeInfo& type, const void* value, ByteWriter& out) {
    using reflect::TypeKind;
    switch (type.kind) {
    case TypeKind::Bool: out.U8(*static_cast<const bool*>(value) ? 1 : 0); break;
    case TypeKind::Int: out.Varint(ZigZag(ReadSigned(value, type.size))); break;
    case TypeKind::UInt: out.Varint(ReadUnsigned(value, type.size)); break;
    case TypeKind::Float:
        if (type.size == 4) out.F32(*static_cast<const f32*>(value));
        else out.F64(*static_cast<const f64*>(value));
        break;
    case TypeKind::String: out.String(*static_cast<const std::string*>(value)); break;
    case TypeKind::FixedString: {
        const char* s = static_cast<const char*>(value);
        out.String(std::string(s, ::strnlen(s, type.size)));
        break;
    }
    case TypeKind::Enum: {
        const reflect::TypeInfo& base = IntegerTypeOf(type);
        if (base.kind == TypeKind::Int) out.Varint(ZigZag(ReadSigned(value, type.size)));
        else out.Varint(ReadUnsigned(value, type.size));
        break;
    }
    case TypeKind::Struct:
        for (const reflect::FieldInfo& f : type.fields) {
            if (!f.HasFlag(reflect::Field_Transient)) EncodeValue(*f.type, f.Ptr(value), out);
        }
        break;
    case TypeKind::Array: {
        const usize n = type.array_size(value);
        out.Varint(n);
        for (usize i = 0; i < n; ++i) EncodeValue(*type.element, type.ArrayElement(value, i), out);
        break;
    }
    }
}

bool DecodeValue(const reflect::TypeInfo& type, void* value, ByteReader& in) {
    using reflect::TypeKind;
    switch (type.kind) {
    case TypeKind::Bool: *static_cast<bool*>(value) = in.U8() != 0; break;
    case TypeKind::Int: WriteInt(value, type.size, static_cast<u64>(UnZigZag(in.Varint()))); break;
    case TypeKind::UInt: WriteInt(value, type.size, in.Varint()); break;
    case TypeKind::Float:
        if (type.size == 4) *static_cast<f32*>(value) = in.F32();
        else *static_cast<f64*>(value) = in.F64();
        break;
    case TypeKind::String: *static_cast<std::string*>(value) = in.String(); break;
    case TypeKind::FixedString: {
        const std::string s = in.String();
        if (type.size == 0) break;
        std::memset(value, 0, type.size);
        std::memcpy(value, s.data(), std::min<usize>(s.size(), type.size - 1));
        break;
    }
    case TypeKind::Enum: {
        const reflect::TypeInfo& base = IntegerTypeOf(type);
        const u64 raw = base.kind == TypeKind::Int ? static_cast<u64>(UnZigZag(in.Varint())) : in.Varint();
        WriteInt(value, type.size, raw);
        break;
    }
    case TypeKind::Struct:
        for (const reflect::FieldInfo& f : type.fields) {
            if (f.HasFlag(reflect::Field_Transient)) continue;
            if (!DecodeValue(*f.type, f.Ptr(value), in)) return false;
        }
        break;
    case TypeKind::Array: {
        const u64 n = in.Varint();
        if (!in.Ok() || n > in.Remaining()) return false; // every element takes at least a byte
        type.array_resize(value, static_cast<usize>(n));
        for (usize i = 0; i < static_cast<usize>(n); ++i) {
            if (!DecodeValue(*type.element, type.array_element(value, i), in)) return false;
        }
        break;
    }
    }
    return in.Ok();
}

u32 ComponentHash(const char* name) {
    u32 h = 2166136261u; // FNV-1a
    for (const char* c = name; *c != '\0'; ++c) h = (h ^ static_cast<u8>(*c)) * 16777619u;
    return h;
}

namespace {

struct Registry {
    std::vector<ReplicatedComponent> components;
    std::unordered_map<u32, usize> by_hash;
    ComponentId scanned = 0;
};

Registry& Refresh() {
    static Registry registry;
    for (; registry.scanned < RegisteredComponentCount(); ++registry.scanned) {
        const ComponentInfo& info = GetComponentInfo(registry.scanned);
        if (info.reflected == nullptr) continue;
        ReplicatedComponent rc;
        for (const reflect::FieldInfo& f : info.reflected->fields) {
            if (!f.HasFlag(reflect::Field_Replicated)) continue;
            if (rc.fields.size() == kMaxReplicatedFields) {
                AETHER_LOG_WARN("Net", "%s has more than %zu replicated fields; the rest are not sent", info.name, kMaxReplicatedFields);
                break;
            }
            rc.fields.push_back(&f);
        }
        if (rc.fields.empty()) continue;
        rc.id = registry.scanned;
        rc.type = info.reflected;
        rc.hash = ComponentHash(info.name);
        registry.by_hash.emplace(rc.hash, registry.components.size());
        registry.components.push_back(std::move(rc));
    }
    return registry;
}

} // namespace

const std::vector<ReplicatedComponent>& ReplicatedComponents() { return Refresh().components; }

const ReplicatedComponent* FindReplicated(u32 hash) {
    Registry& r = Refresh();
    auto it = r.by_hash.find(hash);
    return it == r.by_hash.end() ? nullptr : &r.components[it->second];
}

// --- server -----------------------------------------------------------------

struct ReplicationServer::Record {
    RecordKind kind = kUpdate;
    u32 net_id = 0;
    f32 priority = 0.0f;
    std::vector<u8> bytes;
    const Snapshot* snapshot = nullptr;
    std::vector<u32> hashes; // components an Update or Remove covers
};

ReplicationServer::ReplicationServer(World& world, NetEndpoint& endpoint) : world_(world), endpoint_(endpoint) {
    (void)GetComponentId<NetIdentity>();
}

Entity ReplicationServer::EntityOf(u32 net_id) const {
    auto it = by_id_.find(net_id);
    return it == by_id_.end() ? kNullEntity : it->second;
}

bool ReplicationServer::Knows(NetAddress peer, u32 net_id) const {
    auto it = peers_.find(peer);
    return it != peers_.end() && it->second.known.count(net_id) != 0;
}

void ReplicationServer::TakeSnapshots(std::vector<Snapshot>& out) {
    const ComponentId identity_id = GetComponentId<NetIdentity>();
    const std::vector<ReplicatedComponent>& replicated = ReplicatedComponents();
    by_id_.clear();

    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Has(identity_id)) return;
        std::vector<const ReplicatedComponent*> present;
        for (const ReplicatedComponent& rc : replicated) {
            if (archetype.Has(rc.id)) present.push_back(&rc);
        }
        for (usize chunk = 0; chunk < archetype.ChunkCount(); ++chunk) {
            const Entity* entities = archetype.EntityArray(chunk);
            NetIdentity* identities = static_cast<NetIdentity*>(archetype.ComponentArray(chunk, identity_id));
            for (u32 row = 0; row < archetype.ChunkEntityCount(chunk); ++row) {
                if (identities[row].net_id == 0) identities[row].net_id = next_net_id_++;
                Snapshot snap;
                snap.net_id = identities[row].net_id;
                snap.entity = entities[row];
                for (const ReplicatedComponent* rc : present) {
                    const u8* base = static_cast<const u8*>(archetype.ComponentArray(chunk, rc->id));
                    const void* object = base + static_cast<usize>(row) * GetComponentInfo(rc->id).size;
                    Component comp;
                    comp.hash = rc->hash;
                    comp.info = rc;
                    for (const reflect::FieldInfo* f : rc->fields) {
                        ByteWriter w;
                        EncodeValue(*f->type, f->Ptr(object), w);
                        comp.fields.push_back(w.Take());
                    }
                    snap.components.push_back(std::move(comp));
                }
                by_id_[snap.net_id] = snap.entity;
                out.push_back(std::move(snap));
            }
        }
    });
    std::sort(out.begin(), out.end(), [](const Snapshot& a, const Snapshot& b) { return a.net_id < b.net_id; });
}

namespace {

// [hash][flags][mask][the encoded values of the fields in the mask]
void WriteComponent(ByteWriter& w, u32 hash, u8 flags, u64 mask, const std::vector<std::vector<u8>>& fields) {
    w.U32(hash);
    w.U8(flags);
    w.Varint(mask);
    for (usize i = 0; i < fields.size(); ++i) {
        if (mask & (1ull << i)) w.Bytes(fields[i]);
    }
}

u64 AllFields(usize count) { return count >= 64 ? ~0ull : (1ull << count) - 1; }

} // namespace

void ReplicationServer::ReplicateTo(NetAddress peer, PeerState& state, const std::vector<Snapshot>& snapshots) {
    const usize max_message = endpoint_.Config().max_message_size;
    std::vector<Record> records;

    std::unordered_map<u32, const Snapshot*> visible;
    for (const Snapshot& s : snapshots) {
        if (!relevancy || relevancy(peer, s.entity)) visible[s.net_id] = &s;
    }

    std::vector<u32> gone;
    for (const auto& [id, baseline] : state.known) {
        (void)baseline;
        if (visible.count(id) == 0) gone.push_back(id);
    }
    std::sort(gone.begin(), gone.end());
    for (u32 id : gone) {
        Record r;
        r.kind = kDespawn;
        r.net_id = id;
        r.priority = 2.0e9f;
        ByteWriter w;
        w.U8(kDespawn);
        w.Varint(id);
        r.bytes = w.Take();
        records.push_back(std::move(r));
    }

    for (const Snapshot& snap : snapshots) {
        if (visible.count(snap.net_id) == 0) continue;
        auto known = state.known.find(snap.net_id);
        if (known == state.known.end()) {
            Record r;
            r.kind = kSpawn;
            r.net_id = snap.net_id;
            r.priority = 1.0e9f;
            r.snapshot = &snap;
            ByteWriter w;
            w.U8(kSpawn);
            w.Varint(snap.net_id);
            w.U8(static_cast<u8>(snap.components.size()));
            for (const Component& c : snap.components) WriteComponent(w, c.hash, kFullComponent, AllFields(c.fields.size()), c.fields);
            r.bytes = w.Take();
            records.push_back(std::move(r));
            continue;
        }

        const Baseline& base = known->second;
        struct Change {
            const Component* comp;
            u8 flags;
            u64 mask;
        };
        std::vector<Change> changes;
        for (const Component& c : snap.components) {
            auto old = std::find_if(base.components.begin(), base.components.end(), [&](const Component& b) { return b.hash == c.hash; });
            if (old == base.components.end()) {
                changes.push_back({&c, kFullComponent, AllFields(c.fields.size())});
                continue;
            }
            if (spawn_only_.count(c.hash) != 0) continue; // kept current by something else
            u64 mask = 0;
            for (usize i = 0; i < c.fields.size(); ++i) {
                if (old->fields[i] != c.fields[i]) mask |= 1ull << i;
            }
            if (mask != 0) changes.push_back({&c, 0, mask});
        }
        const f32 waited = state.priority.count(snap.net_id) ? state.priority[snap.net_id] : 0.0f;
        auto make_update = [&](usize first, usize last) {
            Record r;
            r.kind = kUpdate;
            r.net_id = snap.net_id;
            r.priority = waited;
            r.snapshot = &snap;
            ByteWriter w;
            w.U8(kUpdate);
            w.Varint(snap.net_id);
            w.U8(static_cast<u8>(last - first));
            for (usize i = first; i < last; ++i) {
                WriteComponent(w, changes[i].comp->hash, changes[i].flags, changes[i].mask, changes[i].comp->fields);
                r.hashes.push_back(changes[i].comp->hash);
            }
            r.bytes = w.Take();
            return r;
        };
        if (!changes.empty()) {
            Record all = make_update(0, changes.size());
            if (all.bytes.size() + 1 <= max_message || changes.size() == 1) {
                records.push_back(std::move(all));
            } else {
                // Too big together: one record per component.
                for (usize i = 0; i < changes.size(); ++i) records.push_back(make_update(i, i + 1));
            }
        }
        for (const Component& b : base.components) {
            const bool still = std::any_of(snap.components.begin(), snap.components.end(), [&](const Component& c) { return c.hash == b.hash; });
            if (still) continue;
            Record r;
            r.kind = kRemove;
            r.net_id = snap.net_id;
            r.priority = waited;
            r.snapshot = &snap;
            r.hashes.push_back(b.hash);
            ByteWriter w;
            w.U8(kRemove);
            w.Varint(snap.net_id);
            w.U32(b.hash);
            r.bytes = w.Take();
            records.push_back(std::move(r));
        }
    }

    std::stable_sort(records.begin(), records.end(), [](const Record& a, const Record& b) { return a.priority > b.priority; });

    std::vector<bool> sent(records.size(), false);
    std::vector<usize> batch_members;
    std::vector<u8> batch = {kReplicationMessage};
    usize sent_bytes = 0;
    bool blocked = false;

    auto commit = [&](const Record& r) {
        switch (r.kind) {
        case kSpawn:
            state.known[r.net_id].components = r.snapshot->components;
            ++stats_.spawns;
            break;
        case kUpdate: {
            Baseline& base = state.known[r.net_id];
            for (u32 hash : r.hashes) {
                const Component* now = nullptr;
                for (const Component& c : r.snapshot->components) {
                    if (c.hash == hash) now = &c;
                }
                if (now == nullptr) continue;
                auto old = std::find_if(base.components.begin(), base.components.end(), [&](const Component& b) { return b.hash == hash; });
                if (old != base.components.end()) *old = *now;
                else base.components.push_back(*now);
            }
            ++stats_.updates;
            break;
        }
        case kRemove: {
            Baseline& base = state.known[r.net_id];
            base.components.erase(std::remove_if(base.components.begin(), base.components.end(),
                                                 [&](const Component& b) { return b.hash == r.hashes[0]; }),
                                  base.components.end());
            ++stats_.removals;
            break;
        }
        case kDespawn:
            state.known.erase(r.net_id);
            ++stats_.despawns;
            break;
        }
        state.priority.erase(r.net_id);
        ++stats_.records_sent;
    };

    auto flush = [&]() {
        if (batch_members.empty()) return;
        if (!endpoint_.Send(peer, Channel::ReliableOrdered, batch)) {
            blocked = true; // send queue full: everything after waits too
        } else {
            stats_.bytes_sent += batch.size();
            for (usize i : batch_members) {
                sent[i] = true;
                commit(records[i]);
            }
        }
        batch.assign(1, kReplicationMessage);
        batch_members.clear();
    };

    for (usize i = 0; i < records.size() && !blocked; ++i) {
        const Record& r = records[i];
        if (r.bytes.size() + 1 > max_message) {
            ++stats_.oversized;
            AETHER_LOG_WARN("Net", "Replication record for entity %u is %zu bytes, over the message limit; not sent", r.net_id, r.bytes.size());
            continue;
        }
        const bool essential = r.kind == kSpawn || r.kind == kDespawn;
        if (bytes_per_peer != 0 && !essential && sent_bytes >= bytes_per_peer) continue;
        if (batch.size() + r.bytes.size() > max_message) {
            flush();
            if (blocked) break;
        }
        batch.insert(batch.end(), r.bytes.begin(), r.bytes.end());
        batch_members.push_back(i);
        sent_bytes += r.bytes.size();
    }
    if (!blocked) flush();

    for (usize i = 0; i < records.size(); ++i) {
        if (sent[i]) continue;
        ++stats_.deferred;
        if (records[i].kind == kUpdate || records[i].kind == kRemove) state.priority[records[i].net_id] = records[i].priority + 1.0f;
    }
}

usize ReplicationServer::Replicate() {
    std::vector<Snapshot> snapshots;
    TakeSnapshots(snapshots);

    const std::vector<NetAddress> connected = endpoint_.ConnectedPeers();
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (std::find(connected.begin(), connected.end(), it->first) == connected.end()) it = peers_.erase(it);
        else ++it;
    }
    const u64 before = stats_.records_sent;
    for (NetAddress peer : connected) ReplicateTo(peer, peers_[peer], snapshots);
    return static_cast<usize>(stats_.records_sent - before);
}

// --- client -----------------------------------------------------------------

Entity ReplicationClient::EntityOf(u32 net_id) const {
    auto it = by_id_.find(net_id);
    return it == by_id_.end() ? kNullEntity : it->second;
}

void ReplicationClient::Clear() {
    for (const auto& [id, entity] : by_id_) {
        (void)id;
        if (world_.IsAlive(entity)) world_.DestroyEntity(entity);
    }
    by_id_.clear();
}

bool ReplicationClient::ApplyComponent(Entity entity, ByteReader& in, bool is_update) {
    (void)is_update;
    const u32 hash = in.U32();
    in.U8(); // flags: a component the entity lacks is added either way
    const u64 mask = in.Varint();
    if (!in.Ok()) return false;
    const ReplicatedComponent* rc = FindReplicated(hash);
    if (rc == nullptr || (rc->fields.size() < 64 && (mask >> rc->fields.size()) != 0)) return false;
    if (!world_.HasComponentRaw(entity, rc->id)) world_.AddComponentRaw(entity, rc->id);
    void* object = world_.GetComponentRaw(entity, rc->id);
    for (usize i = 0; i < rc->fields.size(); ++i) {
        if ((mask & (1ull << i)) == 0) continue;
        if (!DecodeValue(*rc->fields[i]->type, rc->fields[i]->Ptr(object), in)) return false;
    }
    return true;
}

bool ReplicationClient::Apply(std::span<const u8> message) {
    ByteReader in(message);
    auto fail = [&]() {
        ++stats_.malformed;
        return false;
    };
    if (in.U8() != kReplicationMessage) return fail();
    const ComponentId identity_id = GetComponentId<NetIdentity>();

    while (in.Remaining() > 0) {
        const u8 kind = in.U8();
        const u32 net_id = static_cast<u32>(in.Varint());
        if (!in.Ok()) return fail();
        switch (kind) {
        case kSpawn:
        case kUpdate: {
            const u8 count = in.U8();
            if (!in.Ok()) return fail();
            Entity entity = EntityOf(net_id);
            if (kind == kSpawn && entity.IsNull()) {
                ComponentMask mask;
                mask.set(identity_id);
                entity = world_.CreateEntityRaw(mask);
                by_id_[net_id] = entity;
                ++stats_.spawns;
            } else if (entity.IsNull() || !world_.IsAlive(entity)) {
                return fail(); // an update for something never spawned
            }
            for (u8 i = 0; i < count; ++i) {
                if (!ApplyComponent(entity, in, kind == kUpdate)) return fail();
            }
            if (NetIdentity* id = world_.GetComponent<NetIdentity>(entity)) id->net_id = net_id;
            if (kind == kUpdate) ++stats_.updates;
            break;
        }
        case kRemove: {
            const u32 hash = in.U32();
            if (!in.Ok()) return fail();
            Entity entity = EntityOf(net_id);
            const ReplicatedComponent* rc = FindReplicated(hash);
            if (!entity.IsNull() && rc != nullptr && rc->id != identity_id) {
                world_.RemoveComponentRaw(entity, rc->id);
                ++stats_.removals;
            }
            break;
        }
        case kDespawn: {
            Entity entity = EntityOf(net_id);
            if (!entity.IsNull() && world_.IsAlive(entity)) world_.DestroyEntity(entity);
            by_id_.erase(net_id);
            ++stats_.despawns;
            break;
        }
        default:
            return fail();
        }
    }
    return true;
}

} // namespace aether::net
