#include "aether/physics_net/protocol.h"

#include <cmath>
#include <cstring>

namespace aether::physics_net {
namespace {

void Put8(std::vector<u8>& out, u8 v) { out.push_back(v); }
void Put16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}
void Put32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void Put64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void PutF32(std::vector<u8>& out, f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    Put32(out, bits);
}
void PutGuid(std::vector<u8>& out, const EntityGuid& g) {
    Put64(out, g.hi);
    Put64(out, g.lo);
}
void PutVec3(std::vector<u8>& out, const Vec3& v) {
    PutF32(out, v.x);
    PutF32(out, v.y);
    PutF32(out, v.z);
}

struct Reader {
    std::span<const u8> data;
    usize at = 0;
    bool ok = true;
    bool Has(usize n) const { return ok && data.size() - at >= n; }
    u8 Get8() {
        if (!Has(1)) return ok = false, 0;
        return data[at++];
    }
    u16 Get16() {
        if (!Has(2)) return ok = false, 0;
        const u16 v = static_cast<u16>(data[at] | (data[at + 1] << 8));
        at += 2;
        return v;
    }
    u32 Get32() {
        if (!Has(4)) return ok = false, 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data[at + i]) << (8 * i);
        at += 4;
        return v;
    }
    u64 Get64() {
        if (!Has(8)) return ok = false, 0;
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<u64>(data[at + i]) << (8 * i);
        at += 8;
        return v;
    }
    f32 GetF32() {
        const u32 bits = Get32();
        f32 v;
        std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v)) ok = false;
        return ok ? v : 0.0f;
    }
    EntityGuid GetGuid() {
        EntityGuid g;
        g.hi = Get64();
        g.lo = Get64();
        return g;
    }
    Vec3 GetVec3() {
        Vec3 v;
        v.x = GetF32();
        v.y = GetF32();
        v.z = GetF32();
        return v;
    }
    bool Finished() const { return ok && at == data.size(); }
};

bool Begin(Reader& r, MessageType expected) { return r.Get8() == static_cast<u8>(expected) && r.ok; }

} // namespace

const char* GrabStatusName(GrabStatus status) {
    switch (status) {
    case GrabStatus::Granted: return "Granted";
    case GrabStatus::UnknownPlayer: return "UnknownPlayer";
    case GrabStatus::UnknownObject: return "UnknownObject";
    case GrabStatus::NotGrabbable: return "NotGrabbable";
    case GrabStatus::TooFar: return "TooFar";
    case GrabStatus::TooManyHolders: return "TooManyHolders";
    case GrabStatus::AlreadyHolding: return "AlreadyHolding";
    case GrabStatus::RateLimited: return "RateLimited";
    case GrabStatus::Released: return "Released";
    }
    return "?";
}

std::optional<MessageType> PeekMessageType(std::span<const u8> data) {
    if (data.empty()) return std::nullopt;
    switch (data[0]) {
    case static_cast<u8>(MessageType::SnapshotChunk): return MessageType::SnapshotChunk;
    case static_cast<u8>(MessageType::GrabRequest): return MessageType::GrabRequest;
    case static_cast<u8>(MessageType::GrabTarget): return MessageType::GrabTarget;
    case static_cast<u8>(MessageType::GrabRelease): return MessageType::GrabRelease;
    case static_cast<u8>(MessageType::GrabResult): return MessageType::GrabResult;
    default: return std::nullopt;
    }
}

std::vector<u8> Encode(const GrabRequest& m) {
    std::vector<u8> out;
    Put8(out, static_cast<u8>(MessageType::GrabRequest));
    Put16(out, m.request_id);
    PutGuid(out, m.object);
    PutVec3(out, m.point);
    return out;
}
std::vector<u8> Encode(const GrabTarget& m) {
    std::vector<u8> out;
    Put8(out, static_cast<u8>(MessageType::GrabTarget));
    Put32(out, m.sequence);
    PutGuid(out, m.object);
    PutVec3(out, m.target);
    return out;
}
std::vector<u8> Encode(const GrabRelease& m) {
    std::vector<u8> out;
    Put8(out, static_cast<u8>(MessageType::GrabRelease));
    PutGuid(out, m.object);
    return out;
}
std::vector<u8> Encode(const GrabResult& m) {
    std::vector<u8> out;
    Put8(out, static_cast<u8>(MessageType::GrabResult));
    Put16(out, m.request_id);
    Put8(out, static_cast<u8>(m.status));
    PutGuid(out, m.object);
    return out;
}
std::vector<u8> Encode(const SnapshotChunk& m) {
    std::vector<u8> out;
    Put8(out, static_cast<u8>(MessageType::SnapshotChunk));
    Put32(out, m.sequence);
    Put16(out, m.index);
    Put16(out, m.count);
    const std::vector<u8> body = SerializeConstraintSnapshot(m.snapshot);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

bool Decode(std::span<const u8> data, GrabRequest& out) {
    Reader r{data};
    if (!Begin(r, MessageType::GrabRequest)) return false;
    GrabRequest m;
    m.request_id = r.Get16();
    m.object = r.GetGuid();
    m.point = r.GetVec3();
    if (!r.Finished()) return false;
    out = m;
    return true;
}
bool Decode(std::span<const u8> data, GrabTarget& out) {
    Reader r{data};
    if (!Begin(r, MessageType::GrabTarget)) return false;
    GrabTarget m;
    m.sequence = r.Get32();
    m.object = r.GetGuid();
    m.target = r.GetVec3();
    if (!r.Finished()) return false;
    out = m;
    return true;
}
bool Decode(std::span<const u8> data, GrabRelease& out) {
    Reader r{data};
    if (!Begin(r, MessageType::GrabRelease)) return false;
    GrabRelease m;
    m.object = r.GetGuid();
    if (!r.Finished()) return false;
    out = m;
    return true;
}
bool Decode(std::span<const u8> data, GrabResult& out) {
    Reader r{data};
    if (!Begin(r, MessageType::GrabResult)) return false;
    GrabResult m;
    m.request_id = r.Get16();
    const u8 status = r.Get8();
    m.object = r.GetGuid();
    if (!r.Finished() || status > static_cast<u8>(GrabStatus::Released)) return false;
    m.status = static_cast<GrabStatus>(status);
    out = m;
    return true;
}
bool Decode(std::span<const u8> data, SnapshotChunk& out) {
    Reader r{data};
    if (!Begin(r, MessageType::SnapshotChunk)) return false;
    SnapshotChunk m;
    m.sequence = r.Get32();
    m.index = r.Get16();
    m.count = r.Get16();
    if (!r.ok || m.count == 0 || m.index >= m.count) return false;
    if (!DeserializeConstraintSnapshot(data.subspan(r.at), m.snapshot)) return false;
    out = std::move(m);
    return true;
}

std::vector<std::vector<u8>> EncodeSnapshotChunks(u32 sequence, const ConstraintSnapshot& snapshot, usize max_bytes) {
    // Sizes from the snapshot format (physics/constraints.h): "AECS" + u16 + two u32 counts, then 25 bytes a joint and 68 a body; ours adds 9.
    constexpr usize kFixed = 9 + 4 + 2 + 4 + 4, kJoint = 25, kBody = 68;
    std::vector<ConstraintSnapshot> parts(1);
    usize used = kFixed;
    const auto room = [&](usize bytes) {
        if (used + bytes > max_bytes && (!parts.back().joints.empty() || !parts.back().bodies.empty())) {
            parts.emplace_back();
            used = kFixed;
        }
        used += bytes;
    };
    for (const JointStateRecord& j : snapshot.joints) {
        room(kJoint);
        parts.back().joints.push_back(j);
    }
    for (const BodyStateRecord& b : snapshot.bodies) {
        room(kBody);
        parts.back().bodies.push_back(b);
    }
    std::vector<std::vector<u8>> out;
    for (usize i = 0; i < parts.size(); ++i) {
        SnapshotChunk chunk;
        chunk.sequence = sequence;
        chunk.index = static_cast<u16>(i);
        chunk.count = static_cast<u16>(parts.size());
        chunk.snapshot = std::move(parts[i]);
        out.push_back(Encode(chunk));
    }
    return out;
}

} // namespace aether::physics_net
