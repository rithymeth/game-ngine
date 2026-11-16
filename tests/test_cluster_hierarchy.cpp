#include "test_framework.h"

#include "aether/cook/cluster_hierarchy.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <map>
#include <set>

// The cluster hierarchy (the first stage of virtual geometry): the leaves are the mesh exactly, every level is smaller, errors and spheres only grow towards
// the root, and the cut SelectClusters makes covers the mesh exactly once at any distance, with fewer triangles the further away it is.

using namespace aether;
using namespace aether::cook;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

// A lumpy sphere of about 3 * n * n triangles: deterministic noise on the radius, so no region simplifies for free.
void MakeSphere(u32 n, std::vector<Vec3>& positions, std::vector<u32>& indices) {
    positions.clear();
    indices.clear();
    const u32 rings = n, segments = 2 * n;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = 3.14159265f * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = 2.0f * 3.14159265f * static_cast<f32>(s) / static_cast<f32>(segments);
            const f32 lump = 1.0f + 0.08f * std::sin(5.0f * theta) * std::sin(4.0f * phi) + 0.03f * std::sin(13.0f * theta + 2.0f * phi);
            positions.push_back(Vec3(lump * std::sin(phi) * std::cos(theta), lump * std::cos(phi), lump * std::sin(phi) * std::sin(theta)));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + 1, c = a + (segments + 1), d = c + 1;
            for (const u32 i : {a, c, b, b, c, d}) indices.push_back(i);
        }
    }
}

std::array<u32, 3> Key(u32 a, u32 b, u32 c) {
    std::array<u32, 3> k = {a, b, c};
    std::sort(k.begin(), k.end());
    return k;
}

ClusterView ViewAt(f32 distance) {
    ClusterView v;
    v.position = Vec3(0, 0, distance);
    v.vertical_fov = 1.0f;
    v.viewport_height = 1080.0f;
    return v;
}

// The leaves under every cluster, by walking the DAG down: a cluster's source group's inputs are the clusters one level finer.
std::vector<std::set<u32>> LeavesUnder(const ClusterHierarchy& h) {
    std::vector<std::set<u32>> under(h.clusters.size());
    for (u32 i = 0; i < h.clusters.size(); ++i) {
        const Cluster& c = h.clusters[i];
        if (c.source_group < 0) {
            under[i].insert(i);
        } else {
            for (const u32 input : h.groups[static_cast<usize>(c.source_group)].inputs) under[i].insert(under[input].begin(), under[input].end());
        }
    }
    return under;
}

} // namespace

AETHER_TEST(ClusterHierarchy_LeavesAreTheMeshExactlyAndLimitsHold) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    std::string error;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h, &error));
    CHECK(h.leaf_count > 10 && h.leaf_triangles() == indices.size() / 3);
    // Every source triangle is in exactly one leaf.
    std::map<std::array<u32, 3>, int> seen;
    for (u32 i = 0; i < h.leaf_count; ++i) {
        const Cluster& c = h.clusters[i];
        CHECK(c.level == 0 && c.error == 0.0f && c.source_group == -1);
        for (u32 t = 0; t < c.triangle_count; ++t) ++seen[Key(h.indices[c.index_offset + 3 * t], h.indices[c.index_offset + 3 * t + 1], h.indices[c.index_offset + 3 * t + 2])];
    }
    CHECK(seen.size() == indices.size() / 3);
    for (const auto& [key, count] : seen) {
        (void)key;
        CHECK(count == 1);
    }
    // Every cluster of every level stays within the meshlet limits and has a real sphere.
    const MeshletParams limits;
    for (const Cluster& c : h.clusters) {
        CHECK(c.triangle_count >= 1 && c.triangle_count <= limits.max_triangles && c.radius >= 0.0f && c.bounds.radius > 0.0f);
        std::set<u32> vertices(h.indices.begin() + c.index_offset, h.indices.begin() + c.index_offset + c.triangle_count * 3);
        CHECK(vertices.size() <= limits.max_vertices);
    }
}

AETHER_TEST(ClusterHierarchy_LevelsShrinkAndErrorsAndSpheresOnlyGrow) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(48, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    CHECK(h.level_count >= 4 && h.groups.size() > 10);
    // Each level has fewer triangles than the one below, by roughly the reduction.
    std::vector<usize> per_level(h.level_count, 0);
    std::vector<usize> clusters_per_level(h.level_count, 0);
    for (const Cluster& c : h.clusters) {
        per_level[c.level] += c.triangle_count;
        ++clusters_per_level[c.level];
    }
    // A group that cannot shrink waits and is merged at a later level, so one level can hold a little more than the one below it; what always holds is
    // that every group's outputs have clearly fewer triangles than its inputs, and the top is far smaller than the bottom.
    const ClusterParams defaults;
    for (const ClusterGroup& g : h.groups) {
        usize in = 0, out = 0;
        for (const u32 id : g.inputs) in += h.clusters[id].triangle_count;
        for (const u32 id : g.outputs) out += h.clusters[id].triangle_count;
        CHECK(out > 0 && static_cast<f64>(out) <= static_cast<f64>(in) * defaults.min_progress);
    }
    usize top = 0;
    for (const Cluster& c : h.clusters) top += c.group < 0 ? c.triangle_count : 0;
    CHECK(top < per_level[0] / 10);
    CHECK(per_level[1] < per_level[0] * 0.7);
    CHECK(h.root_count() >= 1);
    // Parents are above their children: more error, a sphere that holds the child's, and shared by every cluster of the group.
    for (const Cluster& c : h.clusters) {
        if (c.group < 0) {
            CHECK(c.parent_error == FLT_MAX);
            continue;
        }
        const ClusterGroup& g = h.groups[static_cast<usize>(c.group)];
        CHECK(c.parent_error >= c.error);
        CHECK((c.center - c.parent_center).Length() + c.radius <= c.parent_radius + 1e-3f);
        CHECK(c.parent_error == g.error && c.parent_center.x == g.center.x && c.parent_radius == g.radius);
        for (const u32 out : g.outputs) CHECK(h.clusters[out].error == g.error && h.clusters[out].radius == g.radius && h.clusters[out].source_group == c.group);
    }
    // A coarse level really is coarse: its error is a good part of the sphere's size, a leaf's is nothing.
    f32 top_error = 0.0f;
    for (const Cluster& c : h.clusters) top_error = std::max(top_error, c.error);
    CHECK(top_error > 0.01f * h.mesh_extent);
}

AETHER_TEST(ClusterHierarchy_TheCutIsWatertightAtEveryDistance) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    // Edges as vertex pairs. A cut with a crack or a hole has a boundary edge the source mesh does not have; an overlap shares an edge between more than two
    // triangles. (The source has open edges of its own at its texture seam, which the locked group borders keep.)
    const auto edge_counts = [](std::span<const u32> tris) {
        std::map<std::pair<u32, u32>, int> counts;
        for (usize t = 0; t + 2 < tris.size(); t += 3) {
            for (int k = 0; k < 3; ++k) {
                const u32 a = tris[t + k], b = tris[t + (k + 1) % 3];
                ++counts[{std::min(a, b), std::max(a, b)}];
            }
        }
        return counts;
    };
    std::set<std::pair<u32, u32>> source_open;
    for (const auto& [edge, count] : edge_counts(indices)) {
        if (count == 1) source_open.insert(edge);
    }
    CHECK(!source_open.empty());
    usize previous = ~usize(0);
    for (const f32 distance : {1.5f, 3.0f, 8.0f, 25.0f, 80.0f, 300.0f, 2000.0f}) {
        ClusterSelection sel;
        CHECK(SelectClusters(h, ViewAt(distance), 1.0f, sel));
        std::vector<u32> tris;
        usize triangles = 0;
        for (const u32 id : sel.clusters) {
            const Cluster& c = h.clusters[id];
            tris.insert(tris.end(), h.indices.begin() + c.index_offset, h.indices.begin() + c.index_offset + c.triangle_count * 3);
            triangles += c.triangle_count;
        }
        CHECK(triangles == sel.triangles && !tris.empty());
        std::set<std::pair<u32, u32>> open;
        bool manifold = true;
        for (const auto& [edge, count] : edge_counts(tris)) {
            if (count == 1) open.insert(edge);
            if (count > 2) manifold = false;
        }
        CHECK(manifold);
        CHECK(open == source_open);
        CHECK(sel.triangles <= previous); // never more triangles as it gets further away
        previous = sel.triangles;
    }
    // Every selected cluster really is fine enough, and its parent really is not.
    ClusterSelection sel;
    const ClusterView view = ViewAt(20.0f);
    CHECK(SelectClusters(h, view, 2.0f, sel));
    for (const u32 id : sel.clusters) {
        const Cluster& c = h.clusters[id];
        CHECK(ProjectedError(c.center, c.radius, c.error, view) <= 2.0f);
        CHECK(c.group < 0 || ProjectedError(c.parent_center, c.parent_radius, c.parent_error, view) > 2.0f);
    }
}

AETHER_TEST(ClusterHierarchy_NearIsFullDetailAndFarIsAFewClusters) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    ClusterSelection near_sel, far_sel, off;
    CHECK(SelectClusters(h, ViewAt(1.2f), 0.0f, near_sel));
    CHECK(near_sel.triangles == indices.size() / 3 && near_sel.clusters.size() == h.leaf_count); // a threshold of 0: the leaves, the mesh exactly
    CHECK(SelectClusters(h, ViewAt(100000.0f), 1.0f, far_sel));
    CHECK(far_sel.triangles < indices.size() / 3 / 10 && far_sel.clusters.size() <= std::max<usize>(h.root_count(), 4));
    CHECK(SelectClusters(h, ViewAt(30.0f), 1.0f, off) && off.triangles > far_sel.triangles && off.triangles < near_sel.triangles);
    // A coarser threshold keeps fewer triangles at the same distance.
    ClusterSelection loose, tight;
    CHECK(SelectClusters(h, ViewAt(10.0f), 8.0f, loose) && SelectClusters(h, ViewAt(10.0f), 0.5f, tight));
    CHECK(loose.triangles < tight.triangles);
}

AETHER_TEST(ClusterHierarchy_FrustumCullingDropsWhatIsOutOfView) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(40, positions, indices);
    ClusterHierarchy h;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    // A camera at z = 6 looking down -z: a box frustum around the middle (x, y within 0.3) keeps a thin column of the sphere; looking away keeps nothing.
    ClusterView view = ViewAt(6.0f);
    view.has_frustum = true;
    view.frustum = {ClusterPlane{Vec3(1, 0, 0), 0.3f}, ClusterPlane{Vec3(-1, 0, 0), 0.3f}, ClusterPlane{Vec3(0, 1, 0), 0.3f},
                    ClusterPlane{Vec3(0, -1, 0), 0.3f}, ClusterPlane{Vec3(0, 0, -1), 6.0f}, ClusterPlane{Vec3(0, 0, 1), 100.0f}};
    ClusterSelection all, column, away;
    ClusterView open = ViewAt(6.0f);
    CHECK(SelectClusters(h, open, 1.0f, all) && SelectClusters(h, view, 1.0f, column));
    CHECK(!column.clusters.empty() && column.triangles < all.triangles);
    view.frustum[4] = ClusterPlane{Vec3(0, 0, 1), -50.0f}; // everything is behind this plane
    CHECK(SelectClusters(h, view, 1.0f, away) && away.clusters.empty() && away.triangles == 0);
}

AETHER_TEST(ClusterHierarchy_IsDeterministicAndRefusesBadInput) {
    std::vector<Vec3> positions;
    std::vector<u32> indices;
    MakeSphere(32, positions, indices);
    ClusterHierarchy a, b;
    CHECK(BuildClusterHierarchy(positions, indices, {}, a) && BuildClusterHierarchy(positions, indices, {}, b));
    CHECK(a.indices == b.indices && a.clusters.size() == b.clusters.size() && a.groups.size() == b.groups.size() && a.level_count == b.level_count);
    bool same = true;
    for (usize i = 0; i < a.clusters.size(); ++i) same = same && a.clusters[i].error == b.clusters[i].error && a.clusters[i].group == b.clusters[i].group;
    CHECK(same);

    ClusterHierarchy h;
    std::string error;
    CHECK(BuildClusterHierarchy({}, {}, {}, h, &error) && h.clusters.empty() && h.leaf_count == 0); // empty mesh: nothing
    ClusterParams bad;
    bad.group_size = 1;
    CHECK(!BuildClusterHierarchy(positions, indices, bad, h, &error) && !error.empty());
    bad = ClusterParams{};
    bad.reduction = 0.95f;
    CHECK(!BuildClusterHierarchy(positions, indices, bad, h, &error));
    bad = ClusterParams{};
    bad.min_progress = 0.3f;
    CHECK(!BuildClusterHierarchy(positions, indices, bad, h, &error));
    std::vector<u32> broken = indices;
    broken.push_back(0); // not a multiple of 3
    CHECK(!BuildClusterHierarchy(positions, broken, {}, h, &error));
    broken = indices;
    broken[5] = static_cast<u32>(positions.size()); // out of range
    CHECK(!BuildClusterHierarchy(positions, broken, {}, h, &error));
    std::vector<Vec3> nan_positions = positions;
    nan_positions[3].x = std::nanf("");
    CHECK(!BuildClusterHierarchy(nan_positions, indices, {}, h, &error));

    ClusterSelection sel;
    CHECK(BuildClusterHierarchy(positions, indices, {}, h));
    CHECK(!SelectClusters(h, ViewAt(5.0f), std::nanf(""), sel));
    ClusterView bad_view = ViewAt(5.0f);
    bad_view.viewport_height = 0.0f;
    CHECK(!SelectClusters(h, bad_view, 1.0f, sel));
    bad_view = ViewAt(5.0f);
    bad_view.position.x = INFINITY;
    CHECK(!SelectClusters(h, bad_view, 1.0f, sel));
}

AETHER_TEST(ClusterHierarchy_ProjectedErrorGrowsWithErrorAndShrinksWithDistance) {
    const ClusterView view = ViewAt(0.0f);
    const f32 near_px = ProjectedError(Vec3(0, 0, -10), 1.0f, 0.1f, view);
    const f32 far_px = ProjectedError(Vec3(0, 0, -100), 1.0f, 0.1f, view);
    const f32 big_px = ProjectedError(Vec3(0, 0, -10), 1.0f, 0.4f, view);
    CHECK(near_px > far_px * 5.0f && std::fabs(big_px - 4.0f * near_px) < 1e-3f * big_px);
    CHECK(ProjectedError(Vec3(0, 0, -10), 1.0f, 0.0f, view) == 0.0f && std::isinf(ProjectedError(Vec3(0, 0, -10), 1.0f, FLT_MAX, view)));
    // Inside the sphere the distance is held to the near distance: big, but finite.
    const f32 inside = ProjectedError(Vec3(0, 0, 0), 5.0f, 0.1f, view);
    CHECK(std::isfinite(inside) && inside > near_px);
}
