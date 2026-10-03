#include "aether/net/interpolation.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace aether::net {

Quaternion NlerpShortest(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = dot < 0.0f ? -1.0f : 1.0f;
    Quaternion q;
    q.x = a.x + (b.x * s - a.x) * t;
    q.y = a.y + (b.y * s - a.y) * t;
    q.z = a.z + (b.z * s - a.z) * t;
    q.w = a.w + (b.w * s - a.w) * t;
    const f32 len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-8f) q.x /= len, q.y /= len, q.z /= len, q.w /= len;
    return q;
}

void InterpolationBuffer::Push(f64 time, const Transform& transform) {
    if (!samples_.empty() && time <= samples_.back().time) return;
    samples_.push_back({time, transform});
    while (samples_.size() > kCapacity) samples_.pop_front();
}

bool InterpolationBuffer::Sample(f64 time, Transform& out) const {
    if (samples_.empty()) return false;
    if (time <= samples_.front().time) {
        out = samples_.front().transform;
        return true;
    }
    if (time >= samples_.back().time) {
        out = samples_.back().transform;
        return true;
    }
    auto after = std::upper_bound(samples_.begin(), samples_.end(), time,
                                  [](f64 t, const Sample_& s) { return t < s.time; });
    const Sample_& b = *after;
    const Sample_& a = *(after - 1);
    const f32 t = static_cast<f32>((time - a.time) / (b.time - a.time));
    out.position = a.transform.position + (b.transform.position - a.transform.position) * t;
    out.rotation = NlerpShortest(a.transform.rotation, b.transform.rotation, t);
    return true;
}

SnapshotInterpolation::SnapshotInterpolation(World& world, ReplicationClient& replication, f64 delay)
    : world_(world), replication_(replication), delay_(delay) {}

const InterpolationBuffer* SnapshotInterpolation::Buffer(u32 net_id) const {
    auto it = buffers_.find(net_id);
    return it == buffers_.end() ? nullptr : &it->second;
}

void SnapshotInterpolation::Update(f64 now) {
    const ComponentId transform_id = GetComponentId<Transform>();
    if (replication_.GetStats().snapshots != seen_snapshots_) {
        seen_snapshots_ = replication_.GetStats().snapshots;
        const f64 server_time = replication_.ServerTime();
        // Server time as seen now (it is at least this; the network delay is in the offset).
        const f64 target = server_time - now;
        if (!has_offset_ || std::fabs(target - offset_) > 0.25) offset_ = target, has_offset_ = true;
        else offset_ += (target - offset_) * 0.1;
        for (const auto& [net_id, entity] : replication_.Entities()) {
            if (!world_.IsAlive(entity)) continue;
            const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
            if (!ni || ni->locally_owned || !world_.HasComponent<Transform>(entity)) continue;
            Transform t = *world_.GetComponent<Transform>(entity);
            if (replication_.ReadReplicated(net_id, transform_id, &t)) buffers_[net_id].Push(server_time, t);
        }
        for (auto it = buffers_.begin(); it != buffers_.end();)
            it = replication_.Entities().count(it->first) ? std::next(it) : buffers_.erase(it);
    }
    if (!has_offset_) return;
    render_time_ = std::max(render_time_, now + offset_ - delay_);
    for (auto& [net_id, buffer] : buffers_) {
        const Entity entity = replication_.FindEntity(net_id);
        if (entity.IsNull() || !world_.IsAlive(entity)) continue;
        const NetIdentity* ni = world_.GetComponent<NetIdentity>(entity);
        Transform* t = world_.GetComponent<Transform>(entity);
        if (!t || !ni || ni->locally_owned) continue;
        if (render_time_ > buffer.Newest()) ++starved_;
        buffer.Sample(render_time_, *t);
    }
}

} // namespace aether::net
