#include "mesh_lod.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

namespace aether::player {
namespace {

struct Edge {
    f32 cost;
    u32 a, b; // the vertex that goes, the one it joins
    bool operator>(const Edge& other) const { return cost > other.cost; }
};

u64 Quantise(const Vec3& p, f32 inv) {
    const auto q = [&](f32 v) { return static_cast<u64>(static_cast<i64>(std::llround(v * inv)) + (1 << 20)) & 0x1FFFFF; };
    return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}

} // namespace

std::vector<u32> SimplifyIndices(const std::vector<Vec3>& positions, const std::vector<u32>& indices, f32 target_ratio, f32 max_ratio) {
    if (positions.size() < 8 || indices.size() < 3 * 12) return {};
    Vec3 lo = positions[0], hi = positions[0];
    for (const Vec3& p : positions) {
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const f32 longest = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    if (!(longest > 1e-6f)) return {};
    const Vec3 centre = (lo + hi) * 0.5f;

    // Weld: vertices at one place (a flat-shaded mesh has one per face) are one vertex while collapsing, so a collapse cannot open a crack between faces.
    const f32 inv = 1.0f / (longest * 1e-5f);
    std::unordered_map<u64, u32> welded;
    std::vector<u32> canon(positions.size());
    std::vector<u32> first_of;  // the first original vertex at each welded place
    std::vector<Vec3> place;
    for (usize i = 0; i < positions.size(); ++i) {
        const auto [it, added] = welded.try_emplace(Quantise(positions[i] - lo, inv), static_cast<u32>(place.size()));
        if (added) { place.push_back(positions[i]); first_of.push_back(static_cast<u32>(i)); }
        canon[i] = it->second;
    }

    // Triangles over welded vertices, each with its three original corners kept for the output.
    struct Tri { u32 c[3]; u32 o[3]; bool alive; };
    std::vector<Tri> tris;
    tris.reserve(indices.size() / 3);
    for (usize t = 0; t + 2 < indices.size(); t += 3) {
        if (indices[t] >= positions.size() || indices[t + 1] >= positions.size() || indices[t + 2] >= positions.size()) continue;
        Tri tri{{canon[indices[t]], canon[indices[t + 1]], canon[indices[t + 2]]}, {indices[t], indices[t + 1], indices[t + 2]}, true};
        if (tri.c[0] == tri.c[1] || tri.c[1] == tri.c[2] || tri.c[0] == tri.c[2]) continue;
        tris.push_back(tri);
    }
    const usize start = tris.size();
    if (start < 12) return {};
    const usize target = std::max<usize>(static_cast<usize>(static_cast<f32>(indices.size() / 3) * std::clamp(target_ratio, 0.02f, 0.9f)), 4);

    std::vector<std::vector<u32>> incident(place.size());
    for (u32 t = 0; t < tris.size(); ++t)
        for (const u32 c : tris[t].c) incident[c].push_back(t);
    std::vector<u32> parent(place.size());
    for (u32 i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto find = [&](u32 v) { while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; } return v; };
    // The more extreme vertex survives a collapse: a tip stays a tip (the other way round the tree gets shorter and thinner).
    // "More extreme" is by the share of the box's half-width along each axis, the largest of the three, so the tip of a tall thin tree outranks the wide ring below it.
    const Vec3 half = (hi - lo) * 0.5f;
    const auto reach = [&](u32 v) {
        const Vec3 d = place[v] - centre;
        return std::max({std::fabs(d.x) / std::max(half.x, 1e-6f), std::fabs(d.y) / std::max(half.y, 1e-6f), std::fabs(d.z) / std::max(half.z, 1e-6f)});
    };
    const auto edge = [&](u32 x, u32 y) {
        const u32 keep = reach(x) >= reach(y) ? x : y;
        const u32 go = keep == x ? y : x;
        // Short edges go first, but those at the box's extremes last: a thin tip's ring edges are short and would otherwise go before the tip's own, taking the tip with them.
        const f32 r = std::max(reach(x), reach(y)), r2 = r * r;
        return Edge{(place[x] - place[y]).Length() * (1.0f + 80.0f * r2 * r2 * r2 * r2), go, keep};
    };

    std::priority_queue<Edge, std::vector<Edge>, std::greater<Edge>> heap;
    for (const Tri& tri : tris)
        for (int k = 0; k < 3; ++k) {
            const u32 x = tri.c[k], y = tri.c[(k + 1) % 3];
            if (x < y) heap.push(edge(x, y)); // each edge is pushed once from its lower-numbered end (twice for an edge two triangles share: harmless)
        }

    // A vertex on the box's surface (a tip, the foot of a trunk) must keep a triangle: a collapse that would take its last one is refused, or the tree loses its top
    // and floats off the ground with its trunk gone.
    std::vector<u32> live(place.size(), 0);
    for (const Tri& tri : tris)
        for (const u32 c : tri.c) ++live[c];
    const auto on_surface = [&](u32 v) { return reach(v) >= 0.995f; };

    usize alive = tris.size();
    while (alive > target && !heap.empty()) {
        const Edge e = heap.top();
        heap.pop();
        if (find(e.a) != e.a || find(e.b) != e.b || e.a == e.b) continue; // one end was already collapsed away: a stale entry
        bool refused = false;
        for (const u32 t : incident[e.a]) {
            const Tri& tri = tris[t];
            if (!tri.alive || (tri.c[0] != e.b && tri.c[1] != e.b && tri.c[2] != e.b)) continue; // it dies only if it holds both ends
            for (const u32 w : tri.c)
                if (w != e.a && w != e.b && live[w] <= 1 && on_surface(w)) refused = true;
        }
        if (refused) continue;
        parent[e.a] = e.b;
        std::vector<u32>& to = incident[e.b];
        for (const u32 t : incident[e.a]) {
            Tri& tri = tris[t];
            if (!tri.alive) continue;
            for (u32& c : tri.c) if (find(c) == e.b) c = e.b;
            if (tri.c[0] == tri.c[1] || tri.c[1] == tri.c[2] || tri.c[0] == tri.c[2]) {
                tri.alive = false;
                --alive;
                // Its three corners (the two ends and the third) each lose a triangle; the end that stays is counted below with the others it takes over.
                for (const u32 c : tri.c) if (c != e.b) --live[c];
                --live[e.b];
                continue;
            }
            ++live[e.b];
            to.push_back(t);
            for (int k = 0; k < 3; ++k) if (tri.c[k] == e.b) {
                heap.push(edge(e.b, tri.c[(k + 1) % 3]));
                heap.push(edge(e.b, tri.c[(k + 2) % 3]));
            }
        }
        incident[e.a].clear();
    }

    // Corners that kept their place keep their own vertex (and its normal); one that was collapsed onto another place takes the first vertex there.
    std::vector<u32> out;
    out.reserve(alive * 3);
    for (const Tri& tri : tris) {
        if (!tri.alive) continue;
        u32 corner[3];
        for (int k = 0; k < 3; ++k) {
            const u32 original_place = canon[tri.o[k]];
            corner[k] = find(original_place) == tri.c[k] && original_place == tri.c[k] ? tri.o[k] : first_of[tri.c[k]];
        }
        out.push_back(corner[0]);
        out.push_back(corner[1]);
        out.push_back(corner[2]);
    }
    if (out.empty() || static_cast<f32>(out.size()) > static_cast<f32>(indices.size()) * max_ratio) return {};
    return out;
}

} // namespace aether::player
