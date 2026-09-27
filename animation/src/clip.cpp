#include "aether/animation/clip.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace aether::anim {

namespace {

Vec3 Interpolate(const Vec3& a, const Vec3& b, f32 t) { return Lerp(a, b, t); }
Quaternion Interpolate(const Quaternion& a, const Quaternion& b, f32 t) { return Nlerp(a, b, t); }

// The key pair around `time`: (i, i + 1) and how far between them, clamped at the ends.
template <typename Track>
void Locate(const Track& track, f32 time, usize& i, usize& j, f32& t) {
    const auto& times = track.times;
    if (times.size() == 1 || time <= times.front()) {
        i = j = 0;
        t = 0.0f;
        return;
    }
    if (time >= times.back()) {
        i = j = times.size() - 1;
        t = 0.0f;
        return;
    }
    j = static_cast<usize>(std::upper_bound(times.begin(), times.end(), time) - times.begin());
    i = j - 1;
    const f32 span = times[j] - times[i];
    t = track.step || span <= 0.0f ? 0.0f : (time - times[i]) / span;
}

f32 ScaleError(const Vec3& a, const Vec3& b) {
    auto ratio = [](f32 x, f32 y) {
        if (x == y) return 0.0f;
        if (y == 0.0f || x == 0.0f) return 1.0f;
        return std::fabs(x / y - 1.0f);
    };
    return std::max({ratio(a.x, b.x), ratio(a.y, b.y), ratio(a.z, b.z)});
}

// Greedy reduction: from each kept key, extend as far as interpolation to
// the next candidate stays within `fits` for every key in between.
template <typename Track, typename Fits>
usize Reduce(Track& track, Fits fits) {
    const usize n = track.times.size();
    if (n <= 1) return 0;
    // A constant track needs one key.
    bool constant = true;
    for (usize k = 1; k < n && constant; ++k) constant = fits(track.values[0], track.values[k]);
    if (constant) {
        track.times.resize(1);
        track.values.resize(1);
        return n - 1;
    }
    if (track.step) {
        // Held keys: drop a key equal to the one before it.
        Track out;
        out.step = true;
        for (usize k = 0; k < n; ++k) {
            if (k == 0 || !fits(track.values[k], out.values.back())) {
                out.times.push_back(track.times[k]);
                out.values.push_back(track.values[k]);
            }
        }
        const usize removed = n - out.times.size();
        track = std::move(out);
        return removed;
    }
    Track out;
    out.step = false;
    out.times.push_back(track.times[0]);
    out.values.push_back(track.values[0]);
    usize anchor = 0;
    for (usize k = 1; k + 1 < n; ++k) {
        // Can key k be dropped, interpolating anchor -> k + 1?
        bool ok = true;
        const f32 t0 = track.times[anchor], t1 = track.times[k + 1];
        // At every dropped key, and half way between original keys
        // (rotations don't interpolate linearly, so the error peaks between keys).
        for (usize m = anchor + 1; m <= k + 1 && ok; ++m) {
            const f32 mid = 0.5f * (track.times[m - 1] + track.times[m]);
            const auto original_mid = Interpolate(track.values[m - 1], track.values[m], 0.5f);
            ok = fits(Interpolate(track.values[anchor], track.values[k + 1], (mid - t0) / (t1 - t0)), original_mid);
            if (ok && m <= k) ok = fits(Interpolate(track.values[anchor], track.values[k + 1], (track.times[m] - t0) / (t1 - t0)), track.values[m]);
        }
        if (!ok) {
            out.times.push_back(track.times[k]);
            out.values.push_back(track.values[k]);
            anchor = k;
        }
    }
    out.times.push_back(track.times[n - 1]);
    out.values.push_back(track.values[n - 1]);
    const usize removed = n - out.times.size();
    track = std::move(out);
    return removed;
}

} // namespace

usize AnimationClip::KeyCount() const {
    usize n = 0;
    for (const BoneTrack& t : tracks) n += t.translation.times.size() + t.rotation.times.size() + t.scale.times.size();
    return n;
}

AnimationClip BuildClip(const assets::AnimationData& animation, const Skeleton& skeleton, const std::vector<u32>& bone_nodes) {
    AnimationClip clip;
    clip.name = animation.name;
    clip.duration = animation.duration;
    clip.tracks.resize(skeleton.bones.size());
    std::map<u32, usize> bone_of_node;
    for (usize b = 0; b < bone_nodes.size() && b < skeleton.bones.size(); ++b) bone_of_node[bone_nodes[b]] = b;
    for (const assets::AnimationChannelData& ch : animation.channels) {
        auto it = bone_of_node.find(ch.node);
        if (it == bone_of_node.end()) continue;
        BoneTrack& track = clip.tracks[it->second];
        const bool step = ch.interpolation == assets::AnimationInterpolation::Step;
        const usize width = ch.path == assets::AnimationPath::Rotation ? 4 : 3;
        const usize keys = std::min(ch.times.size(), ch.values.size() / width);
        if (ch.path == assets::AnimationPath::Rotation) {
            track.rotation = {};
            track.rotation.step = step;
            for (usize k = 0; k < keys; ++k) {
                track.rotation.times.push_back(ch.times[k]);
                const f32* v = &ch.values[k * 4];
                track.rotation.values.push_back(Quaternion(v[0], v[1], v[2], v[3]).Normalized());
            }
        } else {
            Vec3Track& t = ch.path == assets::AnimationPath::Translation ? track.translation : track.scale;
            t = {};
            t.step = step;
            for (usize k = 0; k < keys; ++k) {
                t.times.push_back(ch.times[k]);
                t.values.push_back(Vec3(ch.values[k * 3], ch.values[k * 3 + 1], ch.values[k * 3 + 2]));
            }
        }
        for (usize k = 0; k < keys; ++k) clip.duration = std::max(clip.duration, ch.times[k]);
    }
    return clip;
}

f32 ClipTime(const AnimationClip& clip, f32 time, bool loop) {
    if (clip.duration <= 0.0f) return 0.0f;
    if (!loop) return std::clamp(time, 0.0f, clip.duration);
    const f32 t = std::fmod(time, clip.duration);
    return t < 0.0f ? t + clip.duration : t;
}

Vec3 SampleTrack(const Vec3Track& track, f32 time, const Vec3& fallback) {
    if (track.Empty()) return fallback;
    usize i, j;
    f32 t;
    Locate(track, time, i, j, t);
    return Lerp(track.values[i], track.values[j], t);
}

Quaternion SampleTrack(const RotationTrack& track, f32 time, const Quaternion& fallback) {
    if (track.Empty()) return fallback;
    usize i, j;
    f32 t;
    Locate(track, time, i, j, t);
    return Nlerp(track.values[i], track.values[j], t);
}

void SampleClip(const AnimationClip& clip, const Skeleton& skeleton, f32 time, bool loop, Pose& out) {
    const f32 t = ClipTime(clip, time, loop);
    out.local.resize(skeleton.bones.size());
    for (usize b = 0; b < skeleton.bones.size(); ++b) {
        const BoneTransform& rest = skeleton.bones[b].rest;
        if (b >= clip.tracks.size()) {
            out.local[b] = rest;
            continue;
        }
        const BoneTrack& track = clip.tracks[b];
        out.local[b] = {SampleTrack(track.translation, t, rest.translation), SampleTrack(track.rotation, t, rest.rotation),
                        SampleTrack(track.scale, t, rest.scale)};
    }
}

usize ReduceKeys(AnimationClip& clip, const ReductionSettings& s) {
    usize removed = 0;
    for (BoneTrack& t : clip.tracks) {
        removed += Reduce(t.translation, [&](const Vec3& a, const Vec3& b) { return (a - b).Length() <= s.translation; });
        removed += Reduce(t.rotation, [&](const Quaternion& a, const Quaternion& b) { return AngleBetween(a, b) <= s.rotation; });
        removed += Reduce(t.scale, [&](const Vec3& a, const Vec3& b) { return ScaleError(a, b) <= s.scale; });
    }
    return removed;
}

void CollectNotifies(const AnimationClip& clip, f32 from, f32 to, bool loop, std::vector<NotifyPoint>& out) {
    auto crossed = [&](f32 a, f32 b, bool include_start) {
        // Points in (a, b], or [a, b] for the start of a wrapped span.
        for (const AnimNotify& n : clip.notifies) {
            auto hit = [&](f32 t) { return (include_start ? t >= a : t > a) && t <= b; };
            if (hit(n.time)) out.push_back({&n, n.duration > 0.0f, false});
            if (n.duration > 0.0f && hit(std::min(n.time + n.duration, clip.duration))) out.push_back({&n, true, true});
        }
    };
    if (clip.notifies.empty() || from == to) return;
    if (to > from || !loop) {
        if (to > from) crossed(from, to, false);
        return;
    }
    crossed(from, clip.duration, false); // wrapped: the end, then the start
    crossed(0.0f, to, true);
}

} // namespace aether::anim
