#include "aether/animation/blend_space.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace aether::anim {

using nlohmann::json;

namespace {

constexpr int kFormatVersion = 1;

struct P2 {
    f64 x = 0, y = 0;
};

f32 Span(const BlendAxis& a) { return a.max - a.min; }

P2 Normalized(const BlendSpace& s, f32 x, f32 y) {
    const f32 sx = Span(s.x) > 0 ? Span(s.x) : 1.0f, sy = Span(s.y) > 0 ? Span(s.y) : 1.0f;
    return {static_cast<f64>((x - s.x.min) / sx), static_cast<f64>((y - s.y.min) / sy)};
}

f64 Cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

bool Collinear(const std::vector<P2>& pts) {
    if (pts.size() < 3) return true;
    // Relative to the farthest pair, so the tolerance scales.
    usize a = 0, b = 1;
    f64 best = -1;
    for (usize i = 0; i < pts.size(); ++i) {
        for (usize j = i + 1; j < pts.size(); ++j) {
            const f64 d = (pts[i].x - pts[j].x) * (pts[i].x - pts[j].x) + (pts[i].y - pts[j].y) * (pts[i].y - pts[j].y);
            if (d > best) best = d, a = i, b = j;
        }
    }
    for (const P2& p : pts) {
        if (std::fabs(Cross(pts[a], pts[b], p)) > 1e-9 * std::max(best, 1e-12)) return false;
    }
    return true;
}

// Weights on a segment a-b at its closest point to p.
f64 SegmentClosest(const P2& a, const P2& b, const P2& p, f64& t) {
    const f64 dx = b.x - a.x, dy = b.y - a.y, len = dx * dx + dy * dy;
    t = len > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len, 0.0, 1.0) : 0.0;
    const f64 cx = a.x + dx * t - p.x, cy = a.y + dy * t - p.y;
    return cx * cx + cy * cy;
}

std::vector<BlendWeight> Finish(std::map<u32, f32> w) {
    std::vector<BlendWeight> out;
    f32 total = 0.0f;
    for (const auto& [i, v] : w) {
        if (v > 1e-6f) total += v;
    }
    for (const auto& [i, v] : w) {
        if (v > 1e-6f) out.push_back({i, v / total});
    }
    std::stable_sort(out.begin(), out.end(), [](const BlendWeight& a, const BlendWeight& b) { return a.weight > b.weight; });
    return out;
}

std::vector<BlendWeight> Weights1D(const std::vector<std::pair<f64, u32>>& line, f64 x) {
    std::map<u32, f32> w;
    if (line.size() == 1 || x <= line.front().first) {
        w[line.front().second] = 1.0f;
    } else if (x >= line.back().first) {
        w[line.back().second] = 1.0f;
    } else {
        for (usize i = 0; i + 1 < line.size(); ++i) {
            if (x >= line[i].first && x <= line[i + 1].first) {
                const f64 span = line[i + 1].first - line[i].first;
                const f32 t = span > 0 ? static_cast<f32>((x - line[i].first) / span) : 0.0f;
                w[line[i].second] += 1.0f - t;
                w[line[i + 1].second] += t;
                break;
            }
        }
    }
    return Finish(w);
}

} // namespace

std::vector<BlendDiagnostic> ValidateBlendSpace(const BlendSpace& s) {
    std::vector<BlendDiagnostic> out;
    auto add = [&](const char* code, bool error, i32 sample, const std::string& message) { out.push_back({code, error, sample, message}); };
    if (s.dimensions != 1 && s.dimensions != 2) add("BS004", true, -1, "A blend space has 1 or 2 dimensions.");
    if (!(s.x.min < s.x.max)) add("BS004", true, -1, "The '" + s.x.name + "' axis needs min below max.");
    if (s.dimensions == 2 && !(s.y.min < s.y.max)) add("BS004", true, -1, "The '" + s.y.name + "' axis needs min below max.");
    if (s.samples.empty()) add("BS001", true, -1, "The blend space has no samples.");
    std::vector<P2> pts;
    for (usize i = 0; i < s.samples.size(); ++i) {
        const BlendSample& a = s.samples[i];
        const i32 id = static_cast<i32>(i);
        if (a.clip.empty() || !(a.rate > 0.0f)) add("BS006", true, id, "Each sample needs a clip and a rate above 0.");
        const bool outside = a.x < s.x.min || a.x > s.x.max || (s.dimensions == 2 && (a.y < s.y.min || a.y > s.y.max));
        if (outside) add("BS005", false, id, "The sample for '" + a.clip + "' is outside the axis range; it's still used.");
        for (usize j = 0; j < i; ++j) {
            const bool same_x = std::fabs(a.x - s.samples[j].x) < 1e-6f;
            const bool same_y = s.dimensions == 1 || std::fabs(a.y - s.samples[j].y) < 1e-6f;
            if (same_x && same_y) add("BS002", true, id, "Two samples are in the same place.");
        }
        pts.push_back(Normalized(s, a.x, a.y));
    }
    if (s.dimensions == 2 && s.samples.size() >= 2 && Collinear(pts)) {
        add("BS003", true, -1, "The samples are all on one line; use a 1D blend space or move one off the line.");
    }
    return out;
}

void Triangulate(BlendSpace& s) {
    s.triangles.clear();
    if (s.dimensions != 2 || s.samples.size() < 3) return;
    std::vector<P2> pts;
    for (const BlendSample& a : s.samples) pts.push_back(Normalized(s, a.x, a.y));
    if (Collinear(pts)) return;
    // Bowyer-Watson with a big enclosing triangle.
    f64 lo_x = 1e30, lo_y = 1e30, hi_x = -1e30, hi_y = -1e30;
    for (const P2& p : pts) {
        lo_x = std::min(lo_x, p.x), lo_y = std::min(lo_y, p.y), hi_x = std::max(hi_x, p.x), hi_y = std::max(hi_y, p.y);
    }
    const f64 size = std::max(hi_x - lo_x, hi_y - lo_y) * 20.0 + 1.0, cx = (lo_x + hi_x) / 2, cy = (lo_y + hi_y) / 2;
    const u32 n = static_cast<u32>(pts.size());
    pts.push_back({cx - size, cy - size});
    pts.push_back({cx + size, cy - size});
    pts.push_back({cx, cy + size});
    struct Tri {
        u32 a, b, c;
    };
    std::vector<Tri> tris{{n, n + 1, n + 2}};
    auto in_circle = [&](const Tri& t, const P2& p) {
        P2 a = pts[t.a], b = pts[t.b], c = pts[t.c];
        if (Cross(a, b, c) < 0) std::swap(b, c);
        const f64 ax = a.x - p.x, ay = a.y - p.y, bx = b.x - p.x, by = b.y - p.y, qx = c.x - p.x, qy = c.y - p.y;
        return (ax * ax + ay * ay) * (bx * qy - qx * by) - (bx * bx + by * by) * (ax * qy - qx * ay) +
                   (qx * qx + qy * qy) * (ax * by - bx * ay) > 1e-12;
    };
    for (u32 i = 0; i < n; ++i) {
        std::vector<Tri> bad, keep;
        for (const Tri& t : tris) (in_circle(t, pts[i]) ? bad : keep).push_back(t);
        // The hole's boundary: edges of bad triangles that only one of them has.
        std::map<std::pair<u32, u32>, int> edges;
        for (const Tri& t : bad) {
            for (auto [u, v] : {std::pair{t.a, t.b}, std::pair{t.b, t.c}, std::pair{t.c, t.a}}) ++edges[{std::min(u, v), std::max(u, v)}];
        }
        for (const auto& [e, count] : edges) {
            if (count == 1) keep.push_back({e.first, e.second, i});
        }
        tris = std::move(keep);
    }
    for (const Tri& t : tris) {
        if (t.a >= n || t.b >= n || t.c >= n) continue;
        if (std::fabs(Cross(pts[t.a], pts[t.b], pts[t.c])) < 1e-12) continue; // a sliver from collinear points
        s.triangles.push_back({t.a, t.b, t.c});
    }
}

std::vector<BlendWeight> ComputeWeights(const BlendSpace& s, f32 x, f32 y) {
    if (s.samples.empty()) return {};
    const f32 cx = Span(s.x) > 0 ? std::clamp(x, s.x.min, s.x.max) : x;
    const f32 cy = Span(s.y) > 0 ? std::clamp(y, s.y.min, s.y.max) : y;
    if (s.dimensions != 2) {
        std::vector<std::pair<f64, u32>> line;
        for (u32 i = 0; i < s.samples.size(); ++i) line.push_back({s.samples[i].x, i});
        std::stable_sort(line.begin(), line.end());
        return Weights1D(line, cx);
    }
    std::vector<P2> pts;
    for (const BlendSample& a : s.samples) pts.push_back(Normalized(s, a.x, a.y));
    const P2 p = Normalized(s, cx, cy);
    if (s.triangles.empty()) {
        // Too few samples, or on a line: blend along the line through the two farthest.
        if (pts.size() == 1) return {{0, 1.0f}};
        usize a = 0, b = 1;
        f64 best = -1;
        for (usize i = 0; i < pts.size(); ++i) {
            for (usize j = i + 1; j < pts.size(); ++j) {
                const f64 d = (pts[i].x - pts[j].x) * (pts[i].x - pts[j].x) + (pts[i].y - pts[j].y) * (pts[i].y - pts[j].y);
                if (d > best) best = d, a = i, b = j;
            }
        }
        const f64 dx = pts[b].x - pts[a].x, dy = pts[b].y - pts[a].y;
        std::vector<std::pair<f64, u32>> line;
        for (u32 i = 0; i < pts.size(); ++i) line.push_back({(pts[i].x - pts[a].x) * dx + (pts[i].y - pts[a].y) * dy, i});
        std::stable_sort(line.begin(), line.end());
        return Weights1D(line, (p.x - pts[a].x) * dx + (p.y - pts[a].y) * dy);
    }
    for (const auto& t : s.triangles) {
        const P2 &a = pts[t[0]], &b = pts[t[1]], &c = pts[t[2]];
        const f64 area = Cross(a, b, c);
        const f64 wa = Cross(p, b, c) / area, wb = Cross(a, p, c) / area, wc = Cross(a, b, p) / area;
        if (wa >= -1e-9 && wb >= -1e-9 && wc >= -1e-9) {
            std::map<u32, f32> w;
            w[t[0]] += static_cast<f32>(std::max(wa, 0.0));
            w[t[1]] += static_cast<f32>(std::max(wb, 0.0));
            w[t[2]] += static_cast<f32>(std::max(wc, 0.0));
            return Finish(w);
        }
    }
    // Outside the hull: the nearest point on any edge (the hull's, since p is outside).
    f64 best = 1e300, best_t = 0;
    u32 ea = 0, eb = 0;
    for (const auto& t : s.triangles) {
        for (auto [u, v] : {std::pair{t[0], t[1]}, std::pair{t[1], t[2]}, std::pair{t[2], t[0]}}) {
            f64 tt;
            const f64 d = SegmentClosest(pts[u], pts[v], p, tt);
            if (d < best) best = d, best_t = tt, ea = u, eb = v;
        }
    }
    std::map<u32, f32> w;
    w[ea] += static_cast<f32>(1.0 - best_t);
    w[eb] += static_cast<f32>(best_t);
    return Finish(w);
}

void BlendWeighted(const std::vector<const Pose*>& poses, const std::vector<f32>& weights, Pose& out) {
    if (poses.empty()) return;
    f32 total = 0.0f;
    for (f32 w : weights) total += std::max(w, 0.0f);
    const usize n = poses[0]->local.size();
    Pose result;
    result.local.assign(n, BoneTransform{Vec3(0, 0, 0), Quaternion(0, 0, 0, 0), Vec3(0, 0, 0)});
    for (usize p = 0; p < poses.size() && p < weights.size(); ++p) {
        const f32 w = total > 0 ? std::max(weights[p], 0.0f) / total : (p == 0 ? 1.0f : 0.0f);
        if (w == 0.0f) continue;
        for (usize i = 0; i < n && i < poses[p]->local.size(); ++i) {
            const BoneTransform& t = poses[p]->local[i];
            BoneTransform& r = result.local[i];
            r.translation = r.translation + t.translation * w;
            r.scale = r.scale + t.scale * w;
            const f32 sign = Dot(poses[0]->local[i].rotation, t.rotation) < 0.0f ? -w : w;
            r.rotation = Quaternion(r.rotation.x + t.rotation.x * sign, r.rotation.y + t.rotation.y * sign, r.rotation.z + t.rotation.z * sign,
                                    r.rotation.w + t.rotation.w * sign);
        }
    }
    for (BoneTransform& r : result.local) r.rotation = r.rotation.Normalized();
    out = std::move(result);
}

// --- Player --------------------------------------------------------------------------------

BlendSpacePlayer::BlendSpacePlayer(const BlendSpace& space, std::vector<const AnimationClip*> clips, const Skeleton& skeleton)
    : space_(space), clips_(std::move(clips)), skeleton_(skeleton) {
    weights_ = ComputeWeights(space_, x_, y_);
}

void BlendSpacePlayer::SetParameters(f32 x, f32 y) {
    x_ = x;
    y_ = y;
    weights_ = ComputeWeights(space_, x_, y_);
}

f32 BlendSpacePlayer::CycleDuration() const {
    f32 d = 0.0f;
    for (const BlendWeight& w : weights_) {
        const AnimationClip* c = w.sample < clips_.size() ? clips_[w.sample] : nullptr;
        const f32 rate = space_.samples[w.sample].rate > 0 ? space_.samples[w.sample].rate : 1.0f;
        if (c != nullptr) d += w.weight * c->duration / rate;
    }
    return d;
}

void BlendSpacePlayer::Update(f32 dt, Pose& out, RootMotionDelta* motion, const RootMotionSettings* settings) {
    const f32 cycle = CycleDuration();
    const f32 before = phase_;
    if (cycle > 0.0f) {
        phase_ += dt / cycle;
        phase_ -= std::floor(phase_);
    }
    std::vector<const Pose*> poses;
    std::vector<f32> weights;
    scratch_.resize(weights_.size());
    Vec3 moved(0, 0, 0);
    Quaternion turned(0, 0, 0, 0);
    for (usize k = 0; k < weights_.size(); ++k) {
        const BlendWeight& w = weights_[k];
        const AnimationClip* c = w.sample < clips_.size() ? clips_[w.sample] : nullptr;
        if (c == nullptr) continue;
        SampleClip(*c, skeleton_, phase_ * c->duration, true, scratch_[k]);
        if (motion != nullptr) {
            const RootMotionSettings s = settings != nullptr ? *settings : RootMotionSettings{};
            const RootMotionDelta d = ExtractRootMotion(*c, skeleton_, before * c->duration, phase_ * c->duration, true, s);
            moved = moved + d.translation * w.weight;
            const f32 sign = d.rotation.w < 0.0f ? -w.weight : w.weight;
            turned = Quaternion(turned.x + d.rotation.x * sign, turned.y + d.rotation.y * sign, turned.z + d.rotation.z * sign,
                                turned.w + d.rotation.w * sign);
            StripRootMotion(scratch_[k], *c, skeleton_, s);
        }
        poses.push_back(&scratch_[k]);
        weights.push_back(w.weight);
    }
    if (poses.empty()) {
        out = RestPose(skeleton_);
    } else {
        BlendWeighted(poses, weights, out);
    }
    if (motion != nullptr) {
        motion->translation = moved;
        motion->rotation = turned.LengthSq() > 0 ? turned.Normalized() : Quaternion::Identity();
    }
}

// --- Files ---------------------------------------------------------------------------------

json BlendSpaceToJson(const BlendSpace& s) {
    json samples = json::array();
    for (const BlendSample& a : s.samples) {
        json j = {{"clip", a.clip}, {"x", a.x}};
        if (s.dimensions == 2) j["y"] = a.y;
        if (a.rate != 1.0f) j["rate"] = a.rate;
        samples.push_back(std::move(j));
    }
    json out = {{"$type", "BlendSpace"}, {"$version", kFormatVersion}, {"dimensions", s.dimensions},
                {"x", {{"name", s.x.name}, {"min", s.x.min}, {"max", s.x.max}}}, {"samples", samples}};
    if (s.dimensions == 2) out["y"] = {{"name", s.y.name}, {"min", s.y.min}, {"max", s.y.max}};
    return out;
}

bool BlendSpaceFromJson(const json& j, BlendSpace& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "BlendSpace") return fail("not a blend space file");
    if (j.value("$version", 0) > kFormatVersion) return fail("saved by a newer version of the engine");
    BlendSpace s;
    const json dims = j.value("dimensions", json(1));
    if (!dims.is_number_integer() || (dims.get<i64>() != 1 && dims.get<i64>() != 2)) return fail("a blend space has 1 or 2 dimensions");
    s.dimensions = static_cast<u32>(dims.get<i64>());
    auto axis = [&](const char* key, BlendAxis& a) {
        const json v = j.value(key, json::object());
        if (!v.is_object() || !v.value("min", json(0)).is_number() || !v.value("max", json(1)).is_number()) return false;
        a.name = v.value("name", a.name);
        a.min = v.value("min", 0.0f);
        a.max = v.value("max", 1.0f);
        return true;
    };
    if (!axis("x", s.x) || (s.dimensions == 2 && !axis("y", s.y))) return fail("a bad axis");
    for (const json& a : j.value("samples", json::array())) {
        if (!a.is_object() || !a.value("clip", json()).is_string() || !a.value("x", json()).is_number()) return fail("a sample needs a clip and an x");
        BlendSample sample;
        sample.clip = a["clip"].get<std::string>();
        sample.x = a["x"].get<f32>();
        sample.y = a.value("y", 0.0f);
        sample.rate = a.value("rate", 1.0f);
        s.samples.push_back(std::move(sample));
    }
    Triangulate(s);
    out = std::move(s);
    return true;
}

} // namespace aether::anim
