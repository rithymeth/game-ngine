#pragma once

#include "aether/nav/dynamic.h"

#include <vector>

namespace aether::nav {

// Navigation debug drawing (Phase 20 step 6, docs/design/PHASE_SPECS.md
// §20.6): the mesh, links, volumes and paths as plain world-space lines
// and triangles, so it's tested headless; the editor viewport (and a
// game's debug overlay) draws them. Colours are ImGui order (ABGR).

struct NavDebugLine {
    Vec3 a, b;
    u32 color = 0xFFFFFFFF;
};
struct NavDebugTriangle {
    Vec3 a, b, c;
    u32 color = 0xFFFFFFFF;
};
struct NavDebugDraw {
    std::vector<NavDebugLine> lines;
    std::vector<NavDebugTriangle> triangles;
    void Clear() { lines.clear(), triangles.clear(); }
    void Line(const Vec3& a, const Vec3& b, u32 color) { lines.push_back({a, b, color}); }
};

struct NavDebugOptions {
    bool polygons = true;     // filled, by area
    bool edges = true;        // polygon outlines
    bool tile_bounds = false; // the tile grid
    bool links = true;        // off-mesh links as arcs
    bool volumes = true;      // obstacles and area volumes (DynamicNavMesh)
    f32 lift = 0.05f;         // drawn this far above the mesh (no z-fighting)
};

namespace nav_colors {
inline constexpr u32 kEdge = 0xC0302010;
inline constexpr u32 kTile = 0x80808080;
inline constexpr u32 kLink = 0xFF30C0FF;
inline constexpr u32 kOneWayLink = 0xFF3080FF;
inline constexpr u32 kObstacle = 0xFF3030E0;
inline constexpr u32 kPath = 0xFF40FF40;
} // namespace nav_colors

// Walkable ground is blue; other areas get a steady colour of their own. `alpha` 0..255.
u32 NavAreaColor(u8 area, u8 alpha = 0x70);

void DrawNavMesh(const NavMesh& mesh, const NavDebugOptions& options, NavDebugDraw& out);
// The mesh, then the dynamic volumes and links (their outlines and arcs).
void DrawDynamicNavMesh(const DynamicNavMesh& nav, const NavDebugOptions& options, NavDebugDraw& out);
void DrawNavVolume(const NavVolume& volume, NavDebugDraw& out);
// An arc from start to end, up by a quarter of its length; one-way links get an arrowhead.
void DrawNavLink(const NavLink& link, NavDebugDraw& out);
void DrawNavPath(const NavPath& path, u32 color, NavDebugDraw& out);

} // namespace aether::nav
