#include "aether/cook/cluster_hierarchy.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace aether::cook {

namespace {

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

struct Sphere {
    Vec3 center{0, 0, 0};
    f32 radius = 0.0f;
};

// The smallest sphere around two spheres.
Sphere Enclose(const Sphere& a, const Sphere& b) {
    const Vec3 d = b.center - a.center;
    const f32 dist = d.Length();
    if (dist + b.radius <= a.radius) return a;
    if (dist + a.radius <= b.radius) return b;
    const f32 radius = 0.5f * (dist + a.radius + b.radius);
    return {a.center + d * ((radius - a.radius) / dist), radius};
}

u32 Spread(u32 v) { // the bits of a 10-bit number, two zeros between each
    v &= 0x3FF;
    v = (v | (v << 16)) & 0x030000FF;
    v = (v | (v << 8)) & 0x0300F00F;
    v = (v | (v << 4)) & 0x030C30C3;
    v = (v | (v << 2)) & 0x09249249;
    return v;
}

// Neighbouring clusters share vertices. Seeds go in Morton order of their centres; each group grows by the unassigned neighbour that shares the most vertices.
std::vector<std::vector<u32>> Partition(const std::vector<u32>& current, const ClusterHierarchy& h, u32 group_size) {
    std::vector<std::vector<u32>> groups;
    if (current.size() <= 1) {
        if (!current.empty()) groups.push_back(current);
        return groups;
    }
    std::unordered_map<u32, std::vector<u32>> by_vertex; // vertex -> positions in `current`
    std::vector<std::vector<u32>> vertices(current.size());
    for (u32 i = 0; i < current.size(); ++i) {
        const Cluster& c = h.clusters[current[i]];
        std::unordered_set<u32> unique(h.indices.begin() + c.index_offset, h.indices.begin() + c.index_offset + c.triangle_count * 3);
        vertices[i].assign(unique.begin(), unique.end());
        std::sort(vertices[i].begin(), vertices[i].end());
        for (const u32 v : vertices[i]) by_vertex[v].push_back(i);
    }
    Vec3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const u32 id : current) {
        const Vec3& p = h.clusters[id].bounds.center;
        lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
        hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
    }
    const auto quantise = [](f32 v, f32 a, f32 b) { return b > a ? static_cast<u32>(std::clamp((v - a) / (b - a), 0.0f, 1.0f) * 1023.0f) : 0u; };
    std::vector<u64> order(current.size());
    for (u32 i = 0; i < current.size(); ++i) {
        const Vec3& p = h.clusters[current[i]].bounds.center;
        const u32 m = Spread(quantise(p.x, lo.x, hi.x)) | (Spread(quantise(p.y, lo.y, hi.y)) << 1) | (Spread(quantise(p.z, lo.z, hi.z)) << 2);
        order[i] = (static_cast<u64>(m) << 32) | i;
    }
    std::sort(order.begin(), order.end());
    std::vector<char> taken(current.size(), 0);
    for (const u64 seed_key : order) {
        const u32 seed = static_cast<u32>(seed_key & 0xFFFFFFFFu);
        if (taken[seed]) continue;
        std::vector<u32> members = {seed};
        taken[seed] = 1;
        std::unordered_map<u32, u32> shared; // candidate -> vertices shared with the group
        const auto add_neighbours = [&](u32 member) {
            for (const u32 v : vertices[member]) {
                for (const u32 other : by_vertex[v]) {
                    if (!taken[other]) ++shared[other];
                }
            }
        };
        add_neighbours(seed);
        while (members.size() < group_size && !shared.empty()) {
            u32 best = 0, best_count = 0;
            bool found = false;
            for (const auto& [other, count] : shared) {
                if (taken[other]) continue;
                if (!found || count > best_count || (count == best_count && other < best)) {
                    best = other;
                    best_count = count;
                    found = true;
                }
            }
            if (!found) break;
            taken[best] = 1;
            members.push_back(best);
            shared.erase(best);
            add_neighbours(best);
        }
        std::vector<u32> ids;
        ids.reserve(members.size());
        for (const u32 m : members) ids.push_back(current[m]);
        groups.push_back(std::move(ids));
    }
    return groups;
}

} // namespace

usize ClusterHierarchy::leaf_triangles() const {
    usize n = 0;
    for (u32 i = 0; i < leaf_count; ++i) n += clusters[i].triangle_count;
    return n;
}

usize ClusterHierarchy::root_count() const {
    usize n = 0;
    for (const Cluster& c : clusters) n += c.group < 0 ? 1 : 0;
    return n;
}

bool BuildClusterHierarchy(std::span<const Vec3> positions, std::span<const u32> indices, const ClusterParams& params, ClusterHierarchy& out, std::string* error) {
    out = ClusterHierarchy();
    if (params.group_size < 2 || params.group_size > 8) return Fail(error, "group_size must be 2 to 8");
    if (!(params.reduction >= 0.1f && params.reduction <= 0.9f)) return Fail(error, "reduction must be 0.1 to 0.9");
    if (!(params.min_progress > params.reduction && params.min_progress <= 1.0f)) return Fail(error, "min_progress must be above reduction, up to 1");
    if (params.max_levels < 1 || params.max_levels > 64) return Fail(error, "max_levels must be 1 to 64");

    MeshletMesh leaves;
    if (!BuildMeshlets(positions, indices, params.meshlets, leaves, error)) return false;
    if (leaves.meshlets.empty()) return true;
    out.mesh_extent = meshopt_simplifyScale(&positions[0].x, positions.size(), sizeof(Vec3));

    const auto add_cluster = [&](const std::vector<u32>& tri_indices, usize first, usize count, const MeshletBounds& bounds, u32 level) {
        Cluster c;
        c.index_offset = static_cast<u32>(out.indices.size());
        c.triangle_count = static_cast<u32>(count);
        c.level = level;
        c.bounds = bounds;
        c.center = bounds.center;
        c.radius = bounds.radius;
        c.parent_error = FLT_MAX;
        out.indices.insert(out.indices.end(), tri_indices.begin() + first * 3, tri_indices.begin() + (first + count) * 3);
        out.clusters.push_back(c);
        return static_cast<u32>(out.clusters.size() - 1);
    };
    // Level 0: the leaves, in meshlet order.
    std::vector<u32> current;
    for (usize m = 0; m < leaves.meshlets.size(); ++m) {
        const Meshlet& ml = leaves.meshlets[m];
        std::vector<u32> tris;
        tris.reserve(ml.triangle_count * 3);
        for (u32 t = 0; t < ml.triangle_count * 3; ++t) tris.push_back(leaves.vertices[ml.vertex_offset + leaves.triangles[ml.triangle_offset + t]]);
        current.push_back(add_cluster(tris, 0, ml.triangle_count, leaves.bounds[m], 0));
    }
    out.leaf_count = static_cast<u32>(current.size());
    out.level_count = 1;

    for (u32 level = 1; level <= params.max_levels && current.size() > 1; ++level) {
        const std::vector<std::vector<u32>> groups = Partition(current, out, params.group_size);
        std::vector<u32> next;
        bool progressed = false;
        for (const std::vector<u32>& members : groups) {
            // Merge the group's triangles.
            std::vector<u32> merged;
            for (const u32 id : members) {
                const Cluster& c = out.clusters[id];
                merged.insert(merged.end(), out.indices.begin() + c.index_offset, out.indices.begin() + c.index_offset + c.triangle_count * 3);
            }
            const usize target = std::max<usize>(3, (static_cast<usize>(static_cast<f64>(merged.size()) * params.reduction) / 3) * 3);
            std::vector<u32> simplified(merged.size());
            f32 simplify_error = 0.0f;
            const usize produced = meshopt_simplify(simplified.data(), merged.data(), merged.size(), &positions[0].x, positions.size(), sizeof(Vec3), target, FLT_MAX,
                                                    meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute, &simplify_error);
            simplified.resize(produced);
            if (produced == 0 || static_cast<f64>(produced) > static_cast<f64>(merged.size()) * params.min_progress) {
                next.insert(next.end(), members.begin(), members.end()); // could not shrink: its clusters wait for another grouping
                continue;
            }
            progressed = true;
            // The group's sphere holds every input's, and its error is above theirs: the two things that make the cut consistent.
            Sphere sphere{out.clusters[members[0]].center, out.clusters[members[0]].radius};
            f32 error_below = 0.0f;
            for (const u32 id : members) {
                sphere = Enclose(sphere, {out.clusters[id].center, out.clusters[id].radius});
                error_below = std::max(error_below, out.clusters[id].error);
            }
            ClusterGroup group;
            group.center = sphere.center;
            group.radius = sphere.radius;
            group.error = error_below + std::max(simplify_error, 0.0f);
            group.inputs = members;
            const i32 group_id = static_cast<i32>(out.groups.size());
            for (const u32 id : members) {
                Cluster& c = out.clusters[id];
                c.group = group_id;
                c.parent_center = group.center;
                c.parent_radius = group.radius;
                c.parent_error = group.error;
            }
            // The simplified triangles become the next level's clusters.
            MeshletMesh pieces;
            if (!BuildMeshlets(positions, simplified, params.meshlets, pieces, error)) return false;
            for (usize m = 0; m < pieces.meshlets.size(); ++m) {
                const Meshlet& ml = pieces.meshlets[m];
                std::vector<u32> tris;
                tris.reserve(ml.triangle_count * 3);
                for (u32 t = 0; t < ml.triangle_count * 3; ++t) tris.push_back(pieces.vertices[ml.vertex_offset + pieces.triangles[ml.triangle_offset + t]]);
                const u32 id = add_cluster(tris, 0, ml.triangle_count, pieces.bounds[m], level);
                Cluster& c = out.clusters[id];
                c.center = group.center;
                c.radius = group.radius;
                c.error = group.error;
                c.source_group = group_id;
                group.outputs.push_back(id);
                next.push_back(id);
            }
            out.groups.push_back(std::move(group));
        }
        if (!progressed) break;
        out.level_count = level + 1;
        current = std::move(next);
    }
    return true;
}

f32 ProjectedError(const Vec3& center, f32 radius, f32 error, const ClusterView& view) {
    if (!(error > 0.0f)) return 0.0f;
    if (error >= FLT_MAX) return INFINITY;
    const f32 distance = std::max((center - view.position).Length() - radius, std::max(view.near_distance, 1e-6f));
    const f32 pixels_per_unit = view.viewport_height * 0.5f / std::tan(view.vertical_fov * 0.5f);
    return error * pixels_per_unit / distance;
}

bool SelectClusters(const ClusterHierarchy& h, const ClusterView& view, f32 threshold_pixels, ClusterSelection& out) {
    out = ClusterSelection();
    const bool finite_view = std::isfinite(view.position.x) && std::isfinite(view.position.y) && std::isfinite(view.position.z) && std::isfinite(view.vertical_fov) &&
                             view.vertical_fov > 0.0f && view.vertical_fov < 3.1f && std::isfinite(view.viewport_height) && view.viewport_height > 0.0f;
    if (!finite_view || std::isnan(threshold_pixels)) return false;
    const f32 threshold = std::max(threshold_pixels, 0.0f);
    for (u32 i = 0; i < h.clusters.size(); ++i) {
        const Cluster& c = h.clusters[i];
        // Drawn when it is fine enough by itself and its parent is not. At a threshold of 0 "fine enough" means no error at all.
        if (!(ProjectedError(c.center, c.radius, c.error, view) <= threshold)) continue;
        if (c.group >= 0 && !(ProjectedError(c.parent_center, c.parent_radius, c.parent_error, view) > threshold)) continue;
        if (view.has_frustum) {
            bool inside = true;
            for (const ClusterPlane& plane : view.frustum) {
                if (plane.normal.Dot(c.bounds.center) + plane.d < -c.bounds.radius) {
                    inside = false;
                    break;
                }
            }
            if (!inside) continue;
        }
        if (view.cull_backfaces) {
            // A cluster whose normal cone points away from the viewer (cutoff 1 means it has no useful cone: never culled).
            const Vec3 to_cluster = c.bounds.center - view.position;
            const f32 distance = to_cluster.Length();
            if (distance > 1e-6f && to_cluster.Dot(c.bounds.cone_axis) >= (c.bounds.cone_cutoff * distance + c.bounds.radius)) continue;
        }
        out.clusters.push_back(i);
        out.triangles += c.triangle_count;
    }
    return true;
}

} // namespace aether::cook
