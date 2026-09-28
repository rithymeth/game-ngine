#include "aether/nav/debug_draw.h"

#include <algorithm>
#include <cmath>

namespace aether::nav {

namespace {
constexpr f32 kPi = 3.14159265f;
}

u32 NavAreaColor(u8 area, u8 alpha) {
    const u32 a = static_cast<u32>(alpha) << 24;
    if (area == kAreaWalkable) return a | 0x00C08020; // blue (ABGR)
    if (area == kAreaNull) return a | 0x00202020;
    // A hash of the area into a bright colour.
    u32 h = static_cast<u32>(area) * 2654435761u;
    const u32 r = 64 + ((h >> 8) & 0xBF), g = 64 + ((h >> 16) & 0xBF), b = 64 + ((h >> 24) & 0xBF);
    return a | (b << 16) | (g << 8) | r;
}

void DrawNavMesh(const NavMesh& mesh, const NavDebugOptions& o, NavDebugDraw& out) {
    if (!mesh.Loaded()) return;
    const Vec3 up(0, o.lift, 0);
    for (const NavPolygon& poly : mesh.Polygons()) {
        const auto& v = poly.vertices;
        if (v.size() < 3) continue;
        if (o.polygons)
            for (usize i = 1; i + 1 < v.size(); ++i) out.triangles.push_back({v[0] + up, v[i] + up, v[i + 1] + up, NavAreaColor(poly.area)});
        if (o.edges)
            for (usize i = 0; i < v.size(); ++i) out.Line(v[i] + up, v[(i + 1) % v.size()] + up, nav_colors::kEdge);
    }
    const NavMeshData& d = mesh.Data();
    if (o.tile_bounds) {
        const f32 y = d.origin.y + o.lift;
        for (const NavMeshData::Tile& t : d.tiles) {
            const f32 x0 = d.origin.x + static_cast<f32>(t.x) * d.tile_world_size, z0 = d.origin.z + static_cast<f32>(t.z) * d.tile_world_size;
            const f32 x1 = x0 + d.tile_world_size, z1 = z0 + d.tile_world_size;
            out.Line({x0, y, z0}, {x1, y, z0}, nav_colors::kTile);
            out.Line({x1, y, z0}, {x1, y, z1}, nav_colors::kTile);
            out.Line({x1, y, z1}, {x0, y, z1}, nav_colors::kTile);
            out.Line({x0, y, z1}, {x0, y, z0}, nav_colors::kTile);
        }
    }
    if (o.links)
        for (const NavLink& l : mesh.Links()) DrawNavLink(l, out);
}

void DrawDynamicNavMesh(const DynamicNavMesh& nav, const NavDebugOptions& o, NavDebugDraw& out) {
    DrawNavMesh(nav.Mesh(), o, out);
    if (!o.volumes) return;
    for (const NavVolume& v : nav.Geometry().volumes) DrawNavVolume(v, out);
    for (const auto& [id, v] : nav.Volumes()) DrawNavVolume(v, out);
}

void DrawNavVolume(const NavVolume& v, NavDebugDraw& out) {
    const u32 color = v.area == kAreaNull ? nav_colors::kObstacle : NavAreaColor(v.area, 0xFF);
    // The outline at the bottom and the top, and the uprights.
    std::vector<Vec3> ring;
    f32 lo = 0.0f, hi = 0.0f;
    switch (v.shape) {
    case NavVolume::Shape::Box: {
        const f32 a = v.yaw_degrees * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
        const f32 sx[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
        for (int i = 0; i < 4; ++i) {
            const f32 x = sx[i] * v.half_extents.x, z = sz[i] * v.half_extents.z;
            ring.push_back({v.center.x + x * c + z * s, 0, v.center.z - x * s + z * c});
        }
        lo = v.center.y - v.half_extents.y, hi = v.center.y + v.half_extents.y;
        break;
    }
    case NavVolume::Shape::Cylinder:
        for (int i = 0; i < 16; ++i) {
            const f32 a = static_cast<f32>(i) / 16.0f * 2.0f * kPi;
            ring.push_back({v.center.x + std::cos(a) * v.radius, 0, v.center.z + std::sin(a) * v.radius});
        }
        lo = v.center.y - v.height * 0.5f, hi = v.center.y + v.height * 0.5f;
        break;
    case NavVolume::Shape::Prism:
        for (const Vec3& p : v.points) ring.push_back({p.x, 0, p.z});
        lo = v.min_y, hi = v.max_y;
        break;
    }
    const usize n = ring.size();
    for (usize i = 0; i < n; ++i) {
        const Vec3& p = ring[i];
        const Vec3& q = ring[(i + 1) % n];
        out.Line({p.x, lo, p.z}, {q.x, lo, q.z}, color);
        out.Line({p.x, hi, p.z}, {q.x, hi, q.z}, color);
        if (v.shape != NavVolume::Shape::Cylinder || i % 4 == 0) out.Line({p.x, lo, p.z}, {p.x, hi, p.z}, color);
    }
}

void DrawNavLink(const NavLink& l, NavDebugDraw& out) {
    const u32 color = l.bidirectional ? nav_colors::kLink : nav_colors::kOneWayLink;
    const f32 height = (l.end - l.start).Length() * 0.25f;
    constexpr int kSegments = 8;
    Vec3 prev = l.start;
    for (int i = 1; i <= kSegments; ++i) {
        const f32 t = static_cast<f32>(i) / kSegments;
        const Vec3 p = l.start + (l.end - l.start) * t + Vec3(0, height * 4.0f * t * (1.0f - t), 0);
        out.Line(prev, p, color);
        prev = p;
    }
    if (!l.bidirectional) {
        // An arrowhead at the end, across the link's direction.
        Vec3 dir = l.end - l.start;
        dir.y = 0.0f;
        dir = dir.Normalized();
        const Vec3 side(-dir.z, 0, dir.x);
        const f32 s = std::max(0.2f, l.radius);
        out.Line(l.end, l.end - dir * s + side * (s * 0.5f), color);
        out.Line(l.end, l.end - dir * s - side * (s * 0.5f), color);
    }
    // The ends' reach.
    for (const Vec3& c : {l.start, l.end}) {
        for (int i = 0; i < 8; ++i) {
            const f32 a0 = static_cast<f32>(i) / 8.0f * 2.0f * kPi, a1 = static_cast<f32>(i + 1) / 8.0f * 2.0f * kPi;
            out.Line(c + Vec3(std::cos(a0) * l.radius, 0, std::sin(a0) * l.radius), c + Vec3(std::cos(a1) * l.radius, 0, std::sin(a1) * l.radius), color);
        }
    }
}

void DrawNavPath(const NavPath& path, u32 color, NavDebugDraw& out) {
    const Vec3 up(0, 0.1f, 0);
    for (usize i = 1; i < path.points.size(); ++i) out.Line(path.points[i - 1] + up, path.points[i] + up, color);
    for (const Vec3& p : path.points) out.Line(p, p + Vec3(0, 0.5f, 0), color);
}

} // namespace aether::nav
