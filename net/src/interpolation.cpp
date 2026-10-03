#include "aether/net/interpolation.h"

#include "aether/net/bytes.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>

namespace aether::net {

namespace {

i16 QuantizeUnit(f32 v) { return static_cast<i16>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); }
f32 DequantizeUnit(i16 q) { return static_cast<f32>(q) / 32767.0f; }

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Quaternion Nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = dot < 0.0f ? -1.0f : 1.0f; // the short way round
    return Quaternion(a.x + (b.x * s - a.x) * t, a.y + (b.y * s - a.y) * t, a.z + (b.z * s - a.z) * t, a.w + (b.w * s - a.w) * t)
        .Normalized();
}

constexpr usize kMaxEntityBytes = 5 + 12 + 6; // id varint (up to 5), position, rotation

} // namespace

std::vector<EntitySnapshot> CaptureSnapshots(const World& world, const std::function<bool(const NetIdentity&)>& filter) {
    std::vector<EntitySnapshot> out;
    world.ForEach<NetIdentity, Transform>([&](NetIdentity& id, Transform& t) {
        if (id.net_id == 0 || (filter && !filter(id))) return;
        out.push_back({id.net_id, t.position, t.rotation});
    });
    std::sort(out.begin(), out.end(), [](const EntitySnapshot& a, const EntitySnapshot& b) { return a.net_id < b.net_id; });
    return out;
}

std::vector<std::vector<u8>> BuildSnapshotMessages(f64 server_time, std::span<const EntitySnapshot> snapshots, usize max_message_size) {
    std::vector<std::vector<u8>> messages;
    constexpr usize kHeader = 1 + 8 + 2; // kind, time, count (a varint of up to two bytes here)
    const usize per_message = max_message_size > kHeader + kMaxEntityBytes ? (max_message_size - kHeader) / kMaxEntityBytes : 0;
    if (per_message == 0) return messages;
    for (usize first = 0; first < snapshots.size(); first += per_message) {
        const usize count = std::min(per_message, snapshots.size() - first);
        ByteWriter w;
        w.U8(kSnapshotMessage);
        w.F64(server_time);
        w.Varint(count);
        for (usize i = first; i < first + count; ++i) {
            const EntitySnapshot& s = snapshots[i];
            w.Varint(s.net_id);
            w.F32(s.position.x), w.F32(s.position.y), w.F32(s.position.z);
            Quaternion q = s.rotation.Normalized();
            if (q.w < 0.0f) q = Quaternion(-q.x, -q.y, -q.z, -q.w); // w >= 0 so it can be rebuilt
            w.U16(static_cast<u16>(QuantizeUnit(q.x)));
            w.U16(static_cast<u16>(QuantizeUnit(q.y)));
            w.U16(static_cast<u16>(QuantizeUnit(q.z)));
        }
        messages.push_back(w.Take());
    }
    return messages;
}

bool ParseSnapshotMessage(std::span<const u8> message, f64& server_time, std::vector<EntitySnapshot>& out) {
    out.clear();
    ByteReader r(message);
    if (r.U8() != kSnapshotMessage) return false;
    const f64 time = r.F64();
    const u64 count = r.Varint();
    if (!r.Ok() || !std::isfinite(time) || count > r.Remaining() / 19) return false; // an entity is at least 19 bytes
    std::vector<EntitySnapshot> parsed;
    parsed.reserve(static_cast<usize>(count));
    for (u64 i = 0; i < count; ++i) {
        EntitySnapshot s;
        s.net_id = static_cast<u32>(r.Varint());
        s.position.x = r.F32(), s.position.y = r.F32(), s.position.z = r.F32();
        const f32 x = DequantizeUnit(static_cast<i16>(r.U16()));
        const f32 y = DequantizeUnit(static_cast<i16>(r.U16()));
        const f32 z = DequantizeUnit(static_cast<i16>(r.U16()));
        if (!r.Ok() || !Finite(s.position)) return false;
        const f32 w2 = 1.0f - x * x - y * y - z * z;
        s.rotation = Quaternion(x, y, z, w2 > 0.0f ? std::sqrt(w2) : 0.0f).Normalized();
        parsed.push_back(s);
    }
    if (r.Remaining() != 0) return false;
    server_time = time;
    out = std::move(parsed);
    return true;
}

void ClockSync::OnServerTime(f64 server_time, f64 local_now) {
    const f64 sample = server_time - local_now;
    if (!synced_ || std::fabs(sample - offset_) > 1.0) {
        offset_ = sample; // first sample, or the server's clock jumped
        synced_ = true;
    } else if (sample > offset_) {
        offset_ = sample; // a less delayed packet: closer to the truth
    } else {
        offset_ += (sample - offset_) * 0.02; // follow a drifting clock slowly
    }
}

void InterpolationBuffer::Push(f64 time, const Vec3& position, const Quaternion& rotation) {
    const Entry e{time, position, rotation};
    if (snapshots_.empty() || time > snapshots_.back().time) {
        snapshots_.push_back(e);
    } else {
        auto it = std::lower_bound(snapshots_.begin(), snapshots_.end(), time, [](const Entry& a, f64 t) { return a.time < t; });
        if (it != snapshots_.end() && it->time == time) *it = e;
        else if (it == snapshots_.begin()) return; // older than everything kept
        else snapshots_.insert(it, e);
    }
    while (snapshots_.size() > config_.capacity) snapshots_.pop_front();
}

bool InterpolationBuffer::Sample(f64 render_time, Vec3& position, Quaternion& rotation, bool* extrapolating) const {
    if (extrapolating != nullptr) *extrapolating = false;
    if (snapshots_.empty()) return false;
    const Entry& first = snapshots_.front();
    const Entry& last = snapshots_.back();
    if (render_time <= first.time) {
        position = first.position;
        rotation = first.rotation;
        return true;
    }
    if (render_time >= last.time) {
        position = last.position;
        rotation = last.rotation;
        if (render_time > last.time) {
            if (extrapolating != nullptr) *extrapolating = true;
            if (snapshots_.size() >= 2) {
                const Entry& prev = snapshots_[snapshots_.size() - 2];
                const f64 span = last.time - prev.time;
                if (span > 0.0) {
                    const f64 ahead = std::min(render_time - last.time, config_.max_extrapolation);
                    position = last.position + (last.position - prev.position) * static_cast<f32>(ahead / span);
                }
            }
        }
        return true;
    }
    auto next = std::upper_bound(snapshots_.begin(), snapshots_.end(), render_time, [](f64 t, const Entry& e) { return t < e.time; });
    const Entry& b = *next;
    const Entry& a = *(next - 1);
    const f32 t = static_cast<f32>((render_time - a.time) / (b.time - a.time));
    position = a.position + (b.position - a.position) * t;
    rotation = Nlerp(a.rotation, b.rotation, t);
    return true;
}

bool SnapshotInterpolator::OnSnapshotMessage(std::span<const u8> message, f64 local_now) {
    f64 time = 0.0;
    std::vector<EntitySnapshot> snapshots;
    if (!ParseSnapshotMessage(message, time, snapshots)) {
        ++stats_.malformed;
        return false;
    }
    clock_.OnServerTime(time, local_now);
    for (const EntitySnapshot& s : snapshots) {
        auto it = buffers_.try_emplace(s.net_id, config_).first;
        it->second.Push(time, s.position, s.rotation);
    }
    ++stats_.snapshots;
    return true;
}

void SnapshotInterpolator::Apply(World& world, const ReplicationClient& replication, f64 local_now, NetAddress local_owner) {
    if (!clock_.Synced()) return;
    const f64 render_time = clock_.ServerNow(local_now) - config_.delay;
    for (const auto& [net_id, buffer] : buffers_) {
        const Entity entity = replication.EntityOf(net_id);
        if (entity.IsNull() || !world.IsAlive(entity)) continue; // not spawned (yet): the snapshots wait
        if (local_owner != 0) {
            const NetIdentity* identity = world.GetComponent<NetIdentity>(entity);
            if (identity != nullptr && identity->owner == local_owner) continue;
        }
        Transform* t = world.GetComponent<Transform>(entity);
        if (t == nullptr) continue;
        bool extrapolating = false;
        Vec3 position;
        Quaternion rotation;
        if (!buffer.Sample(render_time, position, rotation, &extrapolating)) continue;
        t->position = position;
        t->rotation = rotation;
        if (extrapolating) ++stats_.extrapolated_frames;
    }
}

} // namespace aether::net
