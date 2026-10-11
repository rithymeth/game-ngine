#include "aether/nav/hierarchical.h"

#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

namespace aether::nav {

namespace {

constexpr i32 kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr i32 kDz[8] = {0, 1, 1, 1, 0, -1, -1, -1};

f32 Dist(const Vec3& a, const Vec3& b) { return (a - b).Length(); }

} // namespace

i32 HierarchicalPathfinder::TileOf(const Vec3& p, i32& z) const {
    const NavMeshData& d = mesh_->Data();
    const f32 size = d.tile_world_size > 0 ? d.tile_world_size : 1.0f;
    z = static_cast<i32>(std::floor((p.z - d.origin.z) / size));
    return static_cast<i32>(std::floor((p.x - d.origin.x) / size));
}

bool HierarchicalPathfinder::Build(const NavMesh& mesh) {
    mesh_ = nullptr;
    portals_.clear();
    stats_ = {};
    const dtNavMesh* nav = mesh.Detour();
    if (!mesh.Loaded() || nav == nullptr) return false;
    mesh_ = &mesh;

    std::map<std::pair<i32, i32>, std::vector<u32>> by_tile;
    for (const NavMeshData::Tile& t : mesh.Data().tiles) {
        const dtMeshTile* tile = nav->getTileAt(t.x, t.z, 0);
        if (tile == nullptr || tile->header == nullptr) continue;
        ++stats_.tiles;
        for (int p = 0; p < tile->header->polyCount; ++p) {
            const dtPoly& poly = tile->polys[p];
            if (poly.getType() != DT_POLYTYPE_GROUND) continue;
            for (int j = 0; j < poly.vertCount; ++j) {
                if ((poly.neis[j] & DT_EXT_LINK) == 0) continue;
                const int side = poly.neis[j] & 0xff;
                if (side >= 8) continue;
                const float* a = &tile->verts[poly.verts[j] * 3];
                const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
                Portal portal;
                portal.tile_x = t.x, portal.tile_z = t.z;
                portal.to_x = t.x + kDx[side], portal.to_z = t.z + kDz[side];
                portal.point = Vec3((a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f);
                by_tile[{t.x, t.z}].push_back(static_cast<u32>(portals_.size()));
                portals_.push_back(std::move(portal));
            }
        }
    }
    stats_.portals = portals_.size();

    // Inside a tile: every portal to every other, by straight-line distance.
    for (const auto& [tile, ids] : by_tile) {
        for (u32 a : ids)
            for (u32 b : ids)
                if (a != b) {
                    portals_[a].edges.emplace_back(b, Dist(portals_[a].point, portals_[b].point));
                    ++stats_.edges;
                }
    }
    // Across a border: to the nearest portal on the other side that leads back, if it is really the same gap.
    const f32 tolerance = mesh.Data().settings.cell_size * 3.0f + mesh.Data().settings.agent_radius;
    for (u32 a = 0; a < portals_.size(); ++a) {
        const auto it = by_tile.find({portals_[a].to_x, portals_[a].to_z});
        if (it == by_tile.end()) continue;
        u32 best = ~0u;
        f32 best_d = tolerance;
        for (u32 b : it->second) {
            if (portals_[b].to_x != portals_[a].tile_x || portals_[b].to_z != portals_[a].tile_z) continue;
            const f32 d = Dist(portals_[a].point, portals_[b].point);
            if (d <= best_d) best = b, best_d = d;
        }
        if (best != ~0u) {
            portals_[a].edges.emplace_back(best, best_d);
            ++stats_.edges;
        }
    }
    return true;
}

bool HierarchicalPathfinder::FindPath(const Vec3& start, const Vec3& end, NavPath& out, const NavQueryFilter& filter) const {
    if (mesh_ == nullptr) return false;
    ++stats_.queries;
    stats_.graph_nodes_expanded = 0;
    stats_.legs = 0;
    const auto direct = [&]() {
        ++stats_.direct;
        stats_.legs = 1;
        return mesh_->FindPath(start, end, out, filter);
    };

    Vec3 s, e;
    if (!mesh_->NearestPoint(start, s) || !mesh_->NearestPoint(end, e)) return direct();
    i32 sz, ez;
    const i32 sx = TileOf(s, sz), ex = TileOf(e, ez);
    if (std::abs(sx - ex) <= 1 && std::abs(sz - ez) <= 1) return direct();

    // A* over the portals, from the start's tile to the goal's. Node n = portal n; start and goal are virtual.
    const u32 n = static_cast<u32>(portals_.size());
    const u32 kGoal = n;
    std::vector<f32> g(n + 1, 1e30f);
    std::vector<u32> from(n + 1, ~0u);
    std::vector<bool> closed(n + 1, false);
    using Item = std::pair<f32, u32>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    for (u32 i = 0; i < n; ++i) {
        if (portals_[i].tile_x != sx || portals_[i].tile_z != sz) continue;
        g[i] = Dist(s, portals_[i].point);
        open.push({g[i] + Dist(portals_[i].point, e), i});
    }
    bool found = false;
    while (!open.empty()) {
        const u32 cur = open.top().second;
        open.pop();
        if (closed[cur]) continue;
        closed[cur] = true;
        ++stats_.graph_nodes_expanded;
        if (cur == kGoal) {
            found = true;
            break;
        }
        const Portal& p = portals_[cur];
        if (p.tile_x == ex && p.tile_z == ez) {
            const f32 ng = g[cur] + Dist(p.point, e);
            if (ng < g[kGoal]) g[kGoal] = ng, from[kGoal] = cur, open.push({ng, kGoal});
        }
        for (const auto& [next, cost] : p.edges) {
            if (closed[next]) continue;
            const f32 ng = g[cur] + cost;
            if (ng < g[next]) {
                g[next] = ng, from[next] = cur;
                open.push({ng + Dist(portals_[next].point, e), next});
            }
        }
    }
    if (!found) return mesh_->FindPath(start, end, out, filter);

    // The portals to pass, in order; consecutive ones across a border are the same gap, so keep one of each pair.
    std::vector<Vec3> via;
    for (u32 at = from[kGoal]; at != ~0u; at = from[at]) via.push_back(portals_[at].point);
    std::reverse(via.begin(), via.end());

    out = NavPath{};
    out.status = PathStatus::Complete;
    Vec3 leg_start = start;
    std::vector<Vec3> waypoints;
    for (const Vec3& v : via)
        if (waypoints.empty() || Dist(waypoints.back(), v) > 0.05f) waypoints.push_back(v);
    waypoints.push_back(end);
    for (const Vec3& target : waypoints) {
        NavPath leg;
        ++stats_.legs;
        if (!mesh_->FindPath(leg_start, target, leg, filter) || leg.points.empty() || leg.status == PathStatus::None) {
            return mesh_->FindPath(start, end, out, filter);
        }
        if (leg.status == PathStatus::Partial && &target == &waypoints.back()) out.status = PathStatus::Partial;
        usize skip = out.points.empty() ? 0 : 1; // the leg's first point is where the last one ended
        for (usize i = skip; i < leg.points.size(); ++i) {
            out.points.push_back(leg.points[i]);
            out.flags.push_back(i < leg.flags.size() ? leg.flags[i] : u8{0});
        }
        leg_start = leg.points.back();
    }
    return !out.points.empty();
}

} // namespace aether::nav
