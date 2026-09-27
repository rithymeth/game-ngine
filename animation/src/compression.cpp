#include "aether/animation/compression.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::anim {

namespace {

constexpr u32 kMagic = 0x4D4E4141; // "AANM"
constexpr u16 kVersion = 1;
constexpr f32 kRotationRange = 0.70710678f; // the three smallest components of a unit quaternion are within ±1/√2

enum : u8 {
    kHasTranslation = 1 << 0,
    kTranslationStep = 1 << 1,
    kHasRotation = 1 << 2,
    kRotationStep = 1 << 3,
    kHasScale = 1 << 4,
    kScaleStep = 1 << 5,
};

class Writer {
public:
    template <typename T>
    void Put(const T& v) {
        const usize at = bytes.size();
        bytes.resize(at + sizeof(T));
        std::memcpy(bytes.data() + at, &v, sizeof(T));
    }
    std::vector<u8> bytes;
};

class Reader {
public:
    explicit Reader(const std::vector<u8>& b) : bytes(b) {}
    template <typename T>
    bool Get(T& v) {
        if (bytes.size() - pos < sizeof(T)) return false;
        std::memcpy(&v, bytes.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }
    bool Skip(usize n) const { return bytes.size() - pos >= n; }
    const std::vector<u8>& bytes;
    usize pos = 0;
};

u16 Quantize(f32 v, f32 lo, f32 extent) {
    if (extent <= 0.0f || !std::isfinite(v)) return 0;
    const f32 u = std::clamp((v - lo) / extent, 0.0f, 1.0f);
    return static_cast<u16>(std::lround(u * 65535.0f));
}
f32 Dequantize(u16 q, f32 lo, f32 extent) { return lo + extent * (static_cast<f32>(q) / 65535.0f); }

void PutTimes(Writer& w, const std::vector<f32>& times) {
    w.Put(static_cast<u32>(times.size()));
    if (times.empty()) return;
    const f32 lo = times.front(), extent = times.back() - times.front();
    w.Put(lo);
    w.Put(extent);
    for (f32 t : times) w.Put(Quantize(t, lo, extent));
}

bool GetTimes(Reader& r, std::vector<f32>& times) {
    u32 n = 0;
    if (!r.Get(n)) return false;
    if (n == 0) return true;
    f32 lo = 0, extent = 0;
    if (!r.Get(lo) || !r.Get(extent) || !r.Skip(static_cast<usize>(n) * 2)) return false;
    times.resize(n);
    for (u32 k = 0; k < n; ++k) {
        u16 q = 0;
        r.Get(q);
        times[k] = Dequantize(q, lo, extent);
    }
    return true;
}

void PutVec3Track(Writer& w, const Vec3Track& track) {
    PutTimes(w, track.times);
    if (track.times.empty()) return;
    Vec3 lo = track.values[0], hi = track.values[0];
    for (const Vec3& v : track.values) {
        lo = Vec3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z));
        hi = Vec3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
    }
    const Vec3 extent = hi - lo;
    w.Put(lo);
    w.Put(extent);
    for (const Vec3& v : track.values) {
        w.Put(Quantize(v.x, lo.x, extent.x));
        w.Put(Quantize(v.y, lo.y, extent.y));
        w.Put(Quantize(v.z, lo.z, extent.z));
    }
}

bool GetVec3Track(Reader& r, Vec3Track& track) {
    if (!GetTimes(r, track.times)) return false;
    if (track.times.empty()) return true;
    Vec3 lo, extent;
    if (!r.Get(lo) || !r.Get(extent) || !r.Skip(track.times.size() * 6)) return false;
    track.values.resize(track.times.size());
    for (Vec3& v : track.values) {
        u16 q[3] = {0, 0, 0};
        r.Get(q[0]);
        r.Get(q[1]);
        r.Get(q[2]);
        v = Vec3(Dequantize(q[0], lo.x, extent.x), Dequantize(q[1], lo.y, extent.y), Dequantize(q[2], lo.z, extent.z));
    }
    return true;
}

void PutRotationTrack(Writer& w, const RotationTrack& track) {
    PutTimes(w, track.times);
    for (const Quaternion& raw : track.values) {
        const Quaternion q = raw.Normalized();
        f32 c[4] = {q.x, q.y, q.z, q.w};
        u8 largest = 0;
        for (u8 i = 1; i < 4; ++i) {
            if (std::fabs(c[i]) > std::fabs(c[largest])) largest = i;
        }
        const f32 sign = c[largest] < 0.0f ? -1.0f : 1.0f; // q and -q are the same rotation
        w.Put(largest);
        for (u8 i = 0; i < 4; ++i) {
            if (i != largest) w.Put(Quantize(c[i] * sign, -kRotationRange, 2.0f * kRotationRange));
        }
    }
}

bool GetRotationTrack(Reader& r, RotationTrack& track) {
    if (!GetTimes(r, track.times)) return false;
    if (!r.Skip(track.times.size() * 7)) return false;
    track.values.resize(track.times.size());
    for (Quaternion& q : track.values) {
        u8 largest = 0;
        r.Get(largest);
        if (largest > 3) return false;
        f32 c[4] = {0, 0, 0, 0};
        f32 sum = 0.0f;
        for (u8 i = 0; i < 4; ++i) {
            if (i == largest) continue;
            u16 v = 0;
            r.Get(v);
            c[i] = Dequantize(v, -kRotationRange, 2.0f * kRotationRange);
            sum += c[i] * c[i];
        }
        c[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
        q = Quaternion(c[0], c[1], c[2], c[3]).Normalized();
    }
    return true;
}

} // namespace

std::vector<u8> CompressClip(const AnimationClip& clip) {
    Writer w;
    w.Put(kMagic);
    w.Put(kVersion);
    const u16 name_size = static_cast<u16>(std::min<usize>(clip.name.size(), 0xFFFF));
    w.Put(name_size);
    for (u16 i = 0; i < name_size; ++i) w.Put(static_cast<u8>(clip.name[i]));
    w.Put(clip.duration);
    w.Put(static_cast<u32>(clip.tracks.size()));
    for (const BoneTrack& t : clip.tracks) {
        u8 flags = 0;
        if (!t.translation.Empty()) flags |= kHasTranslation | (t.translation.step ? kTranslationStep : 0);
        if (!t.rotation.Empty()) flags |= kHasRotation | (t.rotation.step ? kRotationStep : 0);
        if (!t.scale.Empty()) flags |= kHasScale | (t.scale.step ? kScaleStep : 0);
        w.Put(flags);
        if (flags & kHasTranslation) PutVec3Track(w, t.translation);
        if (flags & kHasRotation) PutRotationTrack(w, t.rotation);
        if (flags & kHasScale) PutVec3Track(w, t.scale);
    }
    return std::move(w.bytes);
}

bool DecompressClip(const std::vector<u8>& bytes, AnimationClip& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    Reader r(bytes);
    u32 magic = 0;
    u16 version = 0, name_size = 0;
    if (!r.Get(magic) || magic != kMagic) return fail("not a compressed animation clip");
    if (!r.Get(version) || version > kVersion) return fail("saved by a newer version of the engine");
    if (!r.Get(name_size) || !r.Skip(name_size)) return fail("the clip is cut short");
    AnimationClip clip;
    clip.name.assign(reinterpret_cast<const char*>(bytes.data() + r.pos), name_size);
    r.pos += name_size;
    u32 tracks = 0;
    if (!r.Get(clip.duration) || !r.Get(tracks)) return fail("the clip is cut short");
    if (tracks > bytes.size()) return fail("the clip is corrupt"); // each track takes at least a byte
    clip.tracks.resize(tracks);
    for (BoneTrack& t : clip.tracks) {
        u8 flags = 0;
        if (!r.Get(flags)) return fail("the clip is cut short");
        if ((flags & kHasTranslation) && !GetVec3Track(r, t.translation)) return fail("the clip is cut short");
        if ((flags & kHasRotation) && !GetRotationTrack(r, t.rotation)) return fail("the clip is cut short or corrupt");
        if ((flags & kHasScale) && !GetVec3Track(r, t.scale)) return fail("the clip is cut short");
        t.translation.step = (flags & kTranslationStep) != 0;
        t.rotation.step = (flags & kRotationStep) != 0;
        t.scale.step = (flags & kScaleStep) != 0;
    }
    if (r.pos != bytes.size()) return fail("the clip has trailing data");
    out = std::move(clip);
    return true;
}

usize RawClipSize(const AnimationClip& clip) {
    usize bytes = 0;
    for (const BoneTrack& t : clip.tracks) {
        bytes += t.translation.times.size() * (4 + 12) + t.rotation.times.size() * (4 + 16) + t.scale.times.size() * (4 + 12);
    }
    return bytes;
}

} // namespace aether::anim
