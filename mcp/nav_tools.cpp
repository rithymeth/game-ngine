#include "nav_tools.h"

#include "image_util.h"
#include "terrain_tools.h"

#include "aether/nav/components.h"
#include "aether/nav/crowd.h"
#include "aether/nav/navmesh.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace aether::mcp {

namespace {

namespace fs = std::filesystem;

constexpr usize kMaxItems = 20000;

struct NavBench {
    nav::NavGeometry geometry;
    nav::NavMeshSettings settings;
    std::unique_ptr<nav::NavMesh> mesh;
    nav::NavBuildStats stats;
    bool from_geometry = false; // false: the mesh was imported, so there is no geometry behind it
};

Json Schema(Json properties, std::vector<std::string> required = {}) {
    Json schema = {{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = required;
    return schema;
}

Json Vec3Json(const Vec3& v) { return Json::array({v.x, v.y, v.z}); }

Vec3 V3(const Json& j, const std::string& what) {
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) throw ToolError(what + " must be [x, y, z]");
    return Vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
}

double Number(const Json& j, const char* key, double fallback, double lo, double hi) {
    if (!j.contains(key)) return fallback;
    if (!j[key].is_number() || !(j[key].get<double>() >= lo) || !(j[key].get<double>() <= hi)) {
        throw ToolError(std::string("\"") + key + "\" must be a number from " + std::to_string(lo) + " to " + std::to_string(hi));
    }
    return j[key].get<double>();
}

u8 Area(const Json& j, const char* key, u8 fallback) {
    if (!j.contains(key)) return fallback;
    if (!j[key].is_number_integer() || j[key].get<long long>() < 0 || j[key].get<long long>() > 63) throw ToolError(std::string("\"") + key + "\" must be an area id, 0 to 63");
    return static_cast<u8>(j[key].get<long long>());
}

void RequireMesh(const NavBench& b) {
    if (!b.mesh || !b.mesh->Loaded()) throw ToolError("No navigation mesh: add geometry and call nav_bake (or nav_import) first");
}

nav::NavQueryFilter FilterFrom(const Json& args) {
    nav::NavQueryFilter filter;
    if (args.contains("area_costs")) {
        if (!args["area_costs"].is_object()) throw ToolError("\"area_costs\" must be an object of area id to cost, e.g. {\"2\": 4}");
        for (auto& [key, value] : args["area_costs"].items()) {
            int id = -1;
            try { id = std::stoi(key); } catch (...) {}
            if (id < 0 || id > 63 || !value.is_number() || !(value.get<double>() >= 1.0)) throw ToolError("\"area_costs\" maps an area id (0-63) to a cost of at least 1");
            filter.area_costs[static_cast<usize>(id)] = value.get<f32>();
        }
    }
    if (args.contains("excluded_areas")) {
        if (!args["excluded_areas"].is_array()) throw ToolError("\"excluded_areas\" must be an array of area ids");
        for (const Json& a : args["excluded_areas"]) {
            if (!a.is_number_integer() || a.get<long long>() < 0 || a.get<long long>() > 63) throw ToolError("\"excluded_areas\" holds area ids, 0 to 63");
            filter.excluded[static_cast<usize>(a.get<long long>())] = true;
        }
    }
    return filter;
}

const char* StatusName(nav::PathStatus s) {
    switch (s) {
    case nav::PathStatus::Complete: return "complete";
    case nav::PathStatus::Partial: return "partial";
    case nav::PathStatus::None: return "none";
    }
    return "none";
}

const char* MoveStatusName(NavMoveStatus s) {
    switch (s) {
    case NavMoveStatus::Idle: return "idle";
    case NavMoveStatus::Moving: return "moving";
    case NavMoveStatus::Arrived: return "arrived";
    case NavMoveStatus::Failed: return "failed";
    }
    return "idle";
}

Json GeometryJson(const nav::NavGeometry& g) {
    Json out = {{"triangles", g.TriangleCount()}, {"volumes", g.volumes.size()}, {"links", g.links.size()}};
    Vec3 lo, hi;
    if (g.Bounds(lo, hi)) out["bounds"] = {{"min", Vec3Json(lo)}, {"max", Vec3Json(hi)}};
    return out;
}

Json SettingsJson(const nav::NavMeshSettings& s) {
    return {{"cell_size", s.cell_size}, {"cell_height", s.cell_height}, {"agent_height", s.agent_height}, {"agent_radius", s.agent_radius},
            {"agent_max_climb", s.agent_max_climb}, {"agent_max_slope", s.agent_max_slope}, {"tile_size", s.tile_size}};
}

Json MeshJson(const NavBench& b) {
    Json out = {{"baked", b.mesh && b.mesh->Loaded()}, {"geometry", GeometryJson(b.geometry)}, {"settings", SettingsJson(b.settings)}};
    if (b.mesh && b.mesh->Loaded()) {
        out["tiles"] = b.mesh->TileCount();
        out["polygons"] = b.mesh->PolygonCount();
        out["links_in_mesh"] = b.mesh->Links().size();
        out["from_geometry"] = b.from_geometry;
    }
    return out;
}

// --- pictures --------------------------------------------------------------------------------

struct Canvas {
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
    Canvas(u32 width, u32 height) : w(width), h(height), rgba(static_cast<usize>(width) * height * 4, 255) {
        for (usize i = 0; i < rgba.size(); i += 4) {
            rgba[i] = 28;
            rgba[i + 1] = 30;
            rgba[i + 2] = 36;
        }
    }
    void Put(i32 x, i32 y, Vec3 c) {
        if (x < 0 || y < 0 || x >= static_cast<i32>(w) || y >= static_cast<i32>(h)) return;
        const usize o = (static_cast<usize>(y) * w + static_cast<usize>(x)) * 4;
        rgba[o] = static_cast<u8>(std::clamp(c.x, 0.0f, 1.0f) * 255.0f);
        rgba[o + 1] = static_cast<u8>(std::clamp(c.y, 0.0f, 1.0f) * 255.0f);
        rgba[o + 2] = static_cast<u8>(std::clamp(c.z, 0.0f, 1.0f) * 255.0f);
    }
    void Triangle(f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy, Vec3 color) {
        const i32 x0 = static_cast<i32>(std::floor(std::min({ax, bx, cx}))), x1 = static_cast<i32>(std::ceil(std::max({ax, bx, cx})));
        const i32 y0 = static_cast<i32>(std::floor(std::min({ay, by, cy}))), y1 = static_cast<i32>(std::ceil(std::max({ay, by, cy})));
        const f32 d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (std::fabs(d) < 1e-9f) return;
        for (i32 y = std::max(0, y0); y <= std::min<i32>(h - 1, y1); ++y) {
            for (i32 x = std::max(0, x0); x <= std::min<i32>(w - 1, x1); ++x) {
                const f32 px = static_cast<f32>(x) + 0.5f, py = static_cast<f32>(y) + 0.5f;
                const f32 l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d;
                const f32 l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d;
                const f32 l3 = 1.0f - l1 - l2;
                if (l1 >= -0.001f && l2 >= -0.001f && l3 >= -0.001f) Put(x, y, color);
            }
        }
    }
    void Line(f32 ax, f32 ay, f32 bx, f32 by, Vec3 color, i32 thickness) {
        const f32 len = std::max(std::fabs(bx - ax), std::fabs(by - ay));
        const i32 steps = std::max(1, static_cast<i32>(std::ceil(len)));
        for (i32 i = 0; i <= steps; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
            const i32 x = static_cast<i32>(std::lround(ax + (bx - ax) * t)), y = static_cast<i32>(std::lround(ay + (by - ay) * t));
            for (i32 dy = -(thickness / 2); dy <= thickness / 2; ++dy)
                for (i32 dx = -(thickness / 2); dx <= thickness / 2; ++dx) Put(x + dx, y + dy, color);
        }
    }
    void Dot(f32 x, f32 y, i32 radius, Vec3 color) {
        for (i32 dy = -radius; dy <= radius; ++dy)
            for (i32 dx = -radius; dx <= radius; ++dx)
                if (dx * dx + dy * dy <= radius * radius) Put(static_cast<i32>(std::lround(x)) + dx, static_cast<i32>(std::lround(y)) + dy, color);
    }
};

Vec3 AreaColor(u8 area) {
    if (area == nav::kAreaWalkable) return Vec3(0.28f, 0.55f, 0.32f);
    static const Vec3 palette[] = {Vec3(0.25f, 0.45f, 0.8f), Vec3(0.75f, 0.6f, 0.25f), Vec3(0.6f, 0.4f, 0.7f), Vec3(0.7f, 0.35f, 0.35f), Vec3(0.3f, 0.7f, 0.7f)};
    return palette[area % std::size(palette)];
}

} // namespace

void RegisterNavTools(McpServer& server, EditorSession& session) {
    auto bench = std::make_shared<NavBench>();
    EditorSession* scene = &session;

    server.AddTool(
        {"nav_geometry_add",
         "Describe the ground to bake: add shapes to the navigation workbench (all or nothing). planes: [{center, half_x, half_z, area?}] flat floors; boxes: "
         "[{center, half_extents, area?}] solid blocks (walls with a walkable top); triangles: [{vertices: [[x,y,z]...], indices: [...], area?}] (counter-clockwise "
         "seen from above); volumes: [{shape: box|cylinder|prism, center, half_extents?, yaw_degrees?, radius?, height?, points?, min_y?, max_y?, area}] that relabel the "
         "ground inside (area 0 blocks it: an obstacle; others are water, mud, road); links: [{start, end, radius?, bidirectional?, area?, user_id?}] off-mesh jumps "
         "(Detour joins a link only inside a tile or between neighbouring tiles, so bake with tile_size 0 for long ones). Area 63 is plain ground; 1-62 are yours, with costs set per query.",
         Schema({{"planes", {{"type", "array"}}}, {"boxes", {{"type", "array"}}}, {"triangles", {{"type", "array"}}}, {"volumes", {{"type", "array"}}}, {"links", {{"type", "array"}}}}),
         [bench](const Json& args) -> Json {
             nav::NavGeometry next = bench->geometry;
             auto list = [&](const char* key) -> const Json* {
                 if (!args.contains(key)) return nullptr;
                 if (!args[key].is_array() || args[key].size() > kMaxItems) throw ToolError(std::string("\"") + key + "\" must be an array of up to 20000 items");
                 return &args[key];
             };
             if (const Json* planes = list("planes")) {
                 for (const Json& p : *planes) {
                     if (!p.is_object() || !p.contains("center")) throw ToolError("A plane needs center, half_x and half_z");
                     next.AddPlane(V3(p["center"], "A plane's center"), static_cast<f32>(Number(p, "half_x", 0, 0.001, 1.0e6)), static_cast<f32>(Number(p, "half_z", 0, 0.001, 1.0e6)), Area(p, "area", nav::kAreaWalkable));
                 }
             }
             if (const Json* boxes = list("boxes")) {
                 for (const Json& b : *boxes) {
                     if (!b.is_object() || !b.contains("center") || !b.contains("half_extents")) throw ToolError("A box needs center and half_extents");
                     const Vec3 half = V3(b["half_extents"], "A box's half_extents");
                     if (!(half.x > 0 && half.y > 0 && half.z > 0)) throw ToolError("A box's half_extents must be above 0");
                     next.AddBox(V3(b["center"], "A box's center"), half, Area(b, "area", nav::kAreaWalkable));
                 }
             }
             if (const Json* tris = list("triangles")) {
                 for (const Json& t : *tris) {
                     if (!t.is_object() || !t.contains("vertices") || !t.contains("indices") || !t["vertices"].is_array() || !t["indices"].is_array()) throw ToolError("A triangle set needs vertices and indices");
                     std::vector<Vec3> vertices;
                     std::vector<u32> indices;
                     for (const Json& v : t["vertices"]) vertices.push_back(V3(v, "A vertex"));
                     for (const Json& i : t["indices"]) {
                         if (!i.is_number_integer() || i.get<long long>() < 0 || i.get<usize>() >= vertices.size()) throw ToolError("An index is out of range of the vertices");
                         indices.push_back(i.get<u32>());
                     }
                     if (indices.empty() || indices.size() % 3 != 0) throw ToolError("indices must be a multiple of 3");
                     next.AddTriangles(vertices, indices, Area(t, "area", nav::kAreaWalkable));
                 }
             }
             if (const Json* volumes = list("volumes")) {
                 for (const Json& v : *volumes) {
                     if (!v.is_object() || !v.contains("shape") || !v["shape"].is_string()) throw ToolError("A volume needs a shape: box, cylinder or prism");
                     const std::string shape = v["shape"].get<std::string>();
                     const u8 area = Area(v, "area", nav::kAreaNull);
                     if (shape == "box") {
                         if (!v.contains("center") || !v.contains("half_extents")) throw ToolError("A box volume needs center and half_extents");
                         next.volumes.push_back(nav::NavVolume::Box(V3(v["center"], "center"), V3(v["half_extents"], "half_extents"), area, static_cast<f32>(Number(v, "yaw_degrees", 0, -360, 360))));
                     } else if (shape == "cylinder") {
                         if (!v.contains("center")) throw ToolError("A cylinder volume needs center, radius and height");
                         next.volumes.push_back(nav::NavVolume::Cylinder(V3(v["center"], "center"), static_cast<f32>(Number(v, "radius", 0.5, 0.01, 1.0e5)), static_cast<f32>(Number(v, "height", 2.0, 0.01, 1.0e5)), area));
                     } else if (shape == "prism") {
                         if (!v.contains("points") || !v["points"].is_array() || v["points"].size() < 3) throw ToolError("A prism volume needs at least 3 points");
                         std::vector<Vec3> pts;
                         for (const Json& pt : v["points"]) pts.push_back(V3(pt, "A prism point"));
                         next.volumes.push_back(nav::NavVolume::Prism(pts, static_cast<f32>(Number(v, "min_y", 0, -1.0e6, 1.0e6)), static_cast<f32>(Number(v, "max_y", 2, -1.0e6, 1.0e6)), area));
                     } else {
                         throw ToolError("A volume's shape must be box, cylinder or prism");
                     }
                 }
             }
             if (const Json* links = list("links")) {
                 for (const Json& l : *links) {
                     if (!l.is_object() || !l.contains("start") || !l.contains("end")) throw ToolError("A link needs start and end");
                     nav::NavLink link;
                     link.start = V3(l["start"], "A link's start");
                     link.end = V3(l["end"], "A link's end");
                     link.radius = static_cast<f32>(Number(l, "radius", 0.5, 0.0, 100.0));
                     if (l.contains("bidirectional")) {
                         if (!l["bidirectional"].is_boolean()) throw ToolError("\"bidirectional\" must be true or false");
                         link.bidirectional = l["bidirectional"].get<bool>();
                     }
                     link.area = Area(l, "area", nav::kAreaWalkable);
                     link.user_id = static_cast<u32>(Number(l, "user_id", 0, 0, 4.0e9));
                     next.links.push_back(link);
                 }
             }
             bench->geometry = std::move(next);
             return GeometryJson(bench->geometry);
         }});

    server.AddTool(
        {"nav_geometry_add_terrain",
         "Add the terrain workbench (terrain_create / terrain_sculpt) as ground to bake: its heightmap as triangles, one quad per `stride` samples (default chosen "
         "so the mesh stays under about 128 x 128 quads). Sculpt the terrain first, then add it and bake.",
         Schema({{"stride", {{"type", "integer"}, {"description", "Samples per quad side, 1-32"}}}, {"area", {{"type", "integer"}, {"description", "Area id (default 63)"}}}}),
         [bench](const Json& args) -> Json {
             u32 stride = 1;
             if (args.contains("stride")) {
                 if (!args["stride"].is_number_integer() || args["stride"].get<long long>() < 1 || args["stride"].get<long long>() > 32) throw ToolError("\"stride\" must be an integer from 1 to 32");
                 stride = args["stride"].get<u32>();
             } else {
                 std::vector<Vec3> probe;
                 std::vector<u32> unused;
                 std::string error;
                 if (!TerrainTriangles(1, probe, unused, &error)) throw ToolError(error);
                 const u32 samples = static_cast<u32>(std::sqrt(static_cast<double>(probe.size())));
                 stride = std::max(1u, (samples + 127) / 128);
             }
             std::vector<Vec3> vertices;
             std::vector<u32> indices;
             std::string error;
             if (!TerrainTriangles(stride, vertices, indices, &error)) throw ToolError(error);
             bench->geometry.AddTriangles(vertices, indices, Area(args, "area", nav::kAreaWalkable));
             Json out = GeometryJson(bench->geometry);
             out["stride"] = stride;
             return out;
         }});

    server.AddTool(
        {"nav_geometry_add_scene",
         "Add the editor scene's navigation components as geometry: NavObstacle (carves a hole, when carve is on), NavModifierVolume (relabels the ground) and "
         "NavLinkProxy (off-mesh links), each placed by its entity's Transform position and yaw (parents and scale are not applied). The ground itself still "
         "comes from nav_geometry_add or the terrain: ModelRenderer geometry is not available here.",
         Schema(Json::object()), [bench, scene](const Json&) -> Json {
             auto yaw_of = [](const Transform& t) { return std::atan2(2.0f * (t.rotation.x * t.rotation.z + t.rotation.w * t.rotation.y), 1.0f - 2.0f * (t.rotation.x * t.rotation.x + t.rotation.y * t.rotation.y)); };
             auto to_world = [&](const Transform& t, const Vec3& local) {
                 const f32 yaw = yaw_of(t), c = std::cos(yaw), s = std::sin(yaw);
                 return Vec3(t.position.x + local.x * c + local.z * s, t.position.y + local.y, t.position.z - local.x * s + local.z * c);
             };
             usize obstacles = 0, modifiers = 0, links = 0, skipped = 0;
             nav::NavGeometry& g = bench->geometry;
             scene->world.ForEach<Transform, NavObstacle>([&](Transform& t, NavObstacle& o) {
                 if (!o.carve) {
                     ++skipped;
                     return;
                 }
                 const Vec3 centre = to_world(t, o.center);
                 if (o.shape == NavObstacleShape::Box) g.volumes.push_back(nav::NavVolume::Box(centre, o.half_extents, nav::kAreaNull, yaw_of(t) * 57.29578f));
                 else g.volumes.push_back(nav::NavVolume::Cylinder(centre, o.radius, o.height, nav::kAreaNull));
                 ++obstacles;
             });
             scene->world.ForEach<Transform, NavModifierVolume>([&](Transform& t, NavModifierVolume& m) {
                 g.volumes.push_back(nav::NavVolume::Box(t.position, m.half_extents, static_cast<u8>(std::clamp(m.area, 0, 63)), yaw_of(t) * 57.29578f));
                 ++modifiers;
             });
             scene->world.ForEach<Transform, NavLinkProxy>([&](Transform& t, NavLinkProxy& l) {
                 if (!l.enabled) {
                     ++skipped;
                     return;
                 }
                 nav::NavLink link;
                 link.start = to_world(t, l.start);
                 link.end = to_world(t, l.end);
                 link.radius = l.radius;
                 link.bidirectional = l.bidirectional;
                 link.area = static_cast<u8>(std::clamp(l.area, 1, 63));
                 link.user_id = static_cast<u32>(l.user_id);
                 g.links.push_back(link);
                 ++links;
             });
             Json out = GeometryJson(g);
             out["added"] = {{"obstacles", obstacles}, {"modifier_volumes", modifiers}, {"links", links}, {"skipped", skipped}};
             return out;
         }});

    server.AddTool({"nav_geometry_clear", "Empty the workbench's geometry (the baked mesh stays until you bake again).", Schema(Json::object()), [bench](const Json&) -> Json {
                        bench->geometry = nav::NavGeometry{};
                        return GeometryJson(bench->geometry);
                    }});

    server.AddTool(
        {"nav_bake",
         "Bake the geometry into a navigation mesh with Recast, for an agent of the given size (the settings are kept for the next bake). Returns the tile, "
         "polygon and vertex counts and the time taken. Fails if there is no geometry or no walkable ground comes out.",
         Schema({{"agent_height", {{"type", "number"}, {"description", "Default 2"}}}, {"agent_radius", {{"type", "number"}, {"description", "The mesh keeps this far from walls (default 0.6)"}}},
                 {"agent_max_climb", {{"type", "number"}, {"description", "Steps up to this are walkable (default 0.9)"}}}, {"agent_max_slope", {{"type", "number"}, {"description", "Degrees (default 45)"}}},
                 {"cell_size", {{"type", "number"}, {"description", "Voxel size, smaller is finer and slower (default 0.3)"}}}, {"cell_height", {{"type", "number"}}},
                 {"tile_size", {{"type", "integer"}, {"description", "Cells per tile side, 0 = one tile (default 48)"}}}}),
         [bench](const Json& args) -> Json {
             nav::NavMeshSettings s = bench->settings;
             s.agent_height = static_cast<f32>(Number(args, "agent_height", s.agent_height, 0.1, 100));
             s.agent_radius = static_cast<f32>(Number(args, "agent_radius", s.agent_radius, 0.0, 50));
             s.agent_max_climb = static_cast<f32>(Number(args, "agent_max_climb", s.agent_max_climb, 0.0, 50));
             s.agent_max_slope = static_cast<f32>(Number(args, "agent_max_slope", s.agent_max_slope, 0.0, 89));
             s.cell_size = static_cast<f32>(Number(args, "cell_size", s.cell_size, 0.02, 10));
             s.cell_height = static_cast<f32>(Number(args, "cell_height", s.cell_height, 0.01, 10));
             s.tile_size = static_cast<i32>(Number(args, "tile_size", s.tile_size, 0, 1024));
             if (bench->geometry.TriangleCount() == 0) throw ToolError("No geometry to bake: add planes, boxes, triangles or the terrain first");
             nav::NavMeshData data;
             nav::NavBuildStats stats;
             std::string error;
             if (!nav::BuildNavMesh(bench->geometry, s, data, &error, &stats)) throw ToolError("Bake failed: " + error);
             auto mesh = std::make_unique<nav::NavMesh>();
             if (!mesh->Load(data, &error)) throw ToolError("The baked mesh could not be loaded: " + error);
             bench->settings = s;
             bench->mesh = std::move(mesh);
             bench->stats = stats;
             bench->from_geometry = true;
             Json out = MeshJson(*bench);
             out["bake"] = {{"tiles", stats.tiles}, {"empty_tiles", stats.empty_tiles}, {"polygons", stats.polygons}, {"vertices", stats.vertices}, {"milliseconds", stats.milliseconds}};
             return out;
         }});

    server.AddTool({"nav_info", "The workbench: its geometry, the settings of the last bake, and the baked mesh (tiles, polygons, off-mesh links).", Schema(Json::object()),
                    [bench](const Json&) -> Json { return MeshJson(*bench); }});

    server.AddTool(
        {"nav_path",
         "Shortest walkable paths over the mesh, from the point on the mesh nearest each start to the one nearest each end. queries: [{start, end}] (up to 50). "
         "`area_costs` ({\"2\": 4} makes area 2 four times as costly) and `excluded_areas` ([3]) shape the search. Each result has a status (complete, "
         "partial = as near as it gets, none), the length, the corner points and which of them start an off-mesh link.",
         Schema({{"queries", {{"type", "array"}}}, {"area_costs", {{"type", "object"}}}, {"excluded_areas", {{"type", "array"}}}}, {"queries"}), [bench](const Json& args) -> Json {
             RequireMesh(*bench);
             if (!args["queries"].is_array() || args["queries"].empty() || args["queries"].size() > 50) throw ToolError("\"queries\" must be 1 to 50 {start, end} objects");
             const nav::NavQueryFilter filter = FilterFrom(args);
             Json results = Json::array();
             for (const Json& q : args["queries"]) {
                 if (!q.is_object() || !q.contains("start") || !q.contains("end")) throw ToolError("Each query needs start and end");
                 nav::NavPath path;
                 const bool found = bench->mesh->FindPath(V3(q["start"], "start"), V3(q["end"], "end"), path, filter);
                 Json row = {{"start", q["start"]}, {"end", q["end"]}, {"status", found ? StatusName(path.status) : "none"}};
                 if (found && path.status != nav::PathStatus::None) {
                     Json pts = Json::array(), links = Json::array();
                     for (usize i = 0; i < path.points.size(); ++i) {
                         pts.push_back(Vec3Json(path.points[i]));
                         if (i < path.flags.size() && (path.flags[i] & nav::kNavPointLinkStart) != 0) links.push_back(i);
                     }
                     row["length"] = path.Length();
                     row["points"] = pts;
                     row["link_starts_at_points"] = links;
                 }
                 results.push_back(row);
             }
             return {{"paths", results}};
         }});

    server.AddTool(
        {"nav_query",
         "Other questions of the mesh. kind: nearest (the closest mesh point and its area for each of `points`, within `extents` half sizes, default [2,4,2]); "
         "raycast (walk a straight line over the mesh from start to end: blocked? where and the wall's normal); random (`count` random walkable points, "
         "or within `radius` of `center`, repeatable with `seed`); reachable (whether a full path joins a and b). area_costs / excluded_areas as in nav_path.",
         Schema({{"kind", {{"type", "string"}, {"description", "nearest, raycast, random or reachable"}}}, {"points", {{"type", "array"}}}, {"extents", {{"type", "array"}}},
                 {"start", {{"type", "array"}}}, {"end", {{"type", "array"}}}, {"a", {{"type", "array"}}}, {"b", {{"type", "array"}}}, {"center", {{"type", "array"}}},
                 {"radius", {{"type", "number"}}}, {"count", {{"type", "integer"}}}, {"seed", {{"type", "integer"}}}, {"area_costs", {{"type", "object"}}}, {"excluded_areas", {{"type", "array"}}}},
                {"kind"}),
         [bench](const Json& args) -> Json {
             RequireMesh(*bench);
             const nav::NavQueryFilter filter = FilterFrom(args);
             const std::string kind = args["kind"].is_string() ? args["kind"].get<std::string>() : "";
             const nav::NavMesh& mesh = *bench->mesh;
             if (kind == "nearest") {
                 if (!args.contains("points") || !args["points"].is_array() || args["points"].empty() || args["points"].size() > 200) throw ToolError("\"points\" must be 1 to 200 [x, y, z] points");
                 const Vec3 extents = args.contains("extents") ? V3(args["extents"], "extents") : Vec3(2, 4, 2);
                 Json out = Json::array();
                 for (const Json& p : args["points"]) {
                     const Vec3 point = V3(p, "A point");
                     Vec3 near;
                     u8 area = 0;
                     if (mesh.NearestPoint(point, near, extents, &area)) out.push_back({{"point", p}, {"on_mesh", true}, {"nearest", Vec3Json(near)}, {"area", area}, {"distance", (near - point).Length()}});
                     else out.push_back({{"point", p}, {"on_mesh", false}});
                 }
                 return {{"points", out}};
             }
             if (kind == "raycast") {
                 if (!args.contains("start") || !args.contains("end")) throw ToolError("raycast needs start and end");
                 Vec3 hit, normal;
                 const bool blocked = mesh.Raycast(V3(args["start"], "start"), V3(args["end"], "end"), hit, &normal, filter);
                 Json out = {{"blocked", blocked}};
                 if (blocked) out["hit"] = Vec3Json(hit), out["normal"] = Vec3Json(normal);
                 return out;
             }
             if (kind == "random") {
                 const long long count = args.contains("count") && args["count"].is_number_integer() ? args["count"].get<long long>() : 1;
                 if (count < 1 || count > 100) throw ToolError("\"count\" must be 1 to 100");
                 u64 seed = args.contains("seed") && args["seed"].is_number_integer() ? static_cast<u64>(args["seed"].get<long long>()) : 1;
                 Json out = Json::array();
                 for (long long i = 0; i < count; ++i, ++seed) {
                     Vec3 p;
                     bool ok = false;
                     if (args.contains("center")) ok = mesh.RandomPointNear(V3(args["center"], "center"), static_cast<f32>(Number(args, "radius", 10.0, 0.01, 1.0e6)), seed * 7919 + 13, p, filter);
                     else ok = mesh.RandomPoint(seed * 7919 + 13, p, filter);
                     if (ok) out.push_back(Vec3Json(p));
                 }
                 return {{"points", out}};
             }
             if (kind == "reachable") {
                 if (!args.contains("a") || !args.contains("b")) throw ToolError("reachable needs a and b");
                 return {{"reachable", mesh.Reachable(V3(args["a"], "a"), V3(args["b"], "b"), filter)}};
             }
             throw ToolError("\"kind\" must be nearest, raycast, random or reachable");
         }});

    server.AddTool(
        {"nav_preview",
         "Look at the mesh: a top-down PNG returned as an image (and saved to `path` if given). Walkable polygons are coloured by area (plain ground green), off-mesh "
         "links are magenta lines, `paths` ([{start, end}]) are drawn as red routes with a white start and blue end, and `points` ([[x,y,z]]) as yellow dots.",
         Schema({{"size", {{"type", "integer"}, {"description", "Longest side in pixels, 64-1024 (default 512)"}}}, {"paths", {{"type", "array"}}}, {"points", {{"type", "array"}}},
                 {"area_costs", {{"type", "object"}}}, {"excluded_areas", {{"type", "array"}}}, {"path", {{"type", "string"}, {"description", "Also save the PNG here"}}}}),
         [bench](const Json& args) -> Json {
             RequireMesh(*bench);
             const std::vector<nav::NavPolygon> polys = bench->mesh->Polygons();
             if (polys.empty()) throw ToolError("The mesh has no polygons");
             Vec3 lo(1e30f, 0, 1e30f), hi(-1e30f, 0, -1e30f);
             for (const nav::NavPolygon& p : polys) {
                 for (const Vec3& v : p.vertices) {
                     lo.x = std::min(lo.x, v.x), lo.z = std::min(lo.z, v.z);
                     hi.x = std::max(hi.x, v.x), hi.z = std::max(hi.z, v.z);
                 }
             }
             const f32 W = std::max(hi.x - lo.x, 0.01f), D = std::max(hi.z - lo.z, 0.01f);
             const u32 size = static_cast<u32>(Number(args, "size", 512, 64, 1024));
             const f32 scale = static_cast<f32>(size - 16) / std::max(W, D);
             const u32 pw = static_cast<u32>(W * scale) + 16, ph = static_cast<u32>(D * scale) + 16;
             Canvas canvas(pw, ph);
             auto px = [&](f32 x) { return (x - lo.x) * scale + 8.0f; };
             auto pz = [&](f32 z) { return (z - lo.z) * scale + 8.0f; };
             for (const nav::NavPolygon& p : polys) {
                 for (usize i = 1; i + 1 < p.vertices.size(); ++i) canvas.Triangle(px(p.vertices[0].x), pz(p.vertices[0].z), px(p.vertices[i].x), pz(p.vertices[i].z), px(p.vertices[i + 1].x), pz(p.vertices[i + 1].z), AreaColor(p.area));
             }
             for (const nav::NavLink& l : bench->mesh->Links()) canvas.Line(px(l.start.x), pz(l.start.z), px(l.end.x), pz(l.end.z), Vec3(0.9f, 0.2f, 0.9f), 2);
             const nav::NavQueryFilter filter = FilterFrom(args);
             Json drawn = Json::array();
             if (args.contains("paths")) {
                 if (!args["paths"].is_array() || args["paths"].size() > 50) throw ToolError("\"paths\" must be up to 50 {start, end} objects");
                 for (const Json& q : args["paths"]) {
                     if (!q.is_object() || !q.contains("start") || !q.contains("end")) throw ToolError("Each path needs start and end");
                     nav::NavPath path;
                     const bool found = bench->mesh->FindPath(V3(q["start"], "start"), V3(q["end"], "end"), path, filter);
                     drawn.push_back({{"status", found ? StatusName(path.status) : "none"}, {"length", found ? path.Length() : 0.0f}});
                     if (!found || path.points.size() < 2) continue;
                     for (usize i = 0; i + 1 < path.points.size(); ++i) canvas.Line(px(path.points[i].x), pz(path.points[i].z), px(path.points[i + 1].x), pz(path.points[i + 1].z), Vec3(0.95f, 0.25f, 0.2f), 3);
                     canvas.Dot(px(path.points.front().x), pz(path.points.front().z), 4, Vec3(1, 1, 1));
                     canvas.Dot(px(path.points.back().x), pz(path.points.back().z), 4, Vec3(0.3f, 0.5f, 1.0f));
                 }
             }
             if (args.contains("points")) {
                 if (!args["points"].is_array() || args["points"].size() > 500) throw ToolError("\"points\" must be up to 500 [x, y, z] points");
                 for (const Json& p : args["points"]) {
                     const Vec3 v = V3(p, "A point");
                     canvas.Dot(px(v.x), pz(v.z), 3, Vec3(1.0f, 0.9f, 0.2f));
                 }
             }
             const std::vector<u8> png = EncodePngRgba(canvas.rgba, pw, ph);
             if (png.empty()) throw ToolError("Could not encode the PNG");
             Json out = {{"width", pw}, {"height", ph}, {"polygons", polys.size()}, {"world_bounds", {{"min", Vec3Json(lo)}, {"max", Vec3Json(hi)}}}, {"paths", drawn}};
             if (args.contains("path") && args["path"].is_string()) {
                 std::error_code ec;
                 const fs::path file = args["path"].get<std::string>();
                 if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
                 std::ofstream f(file, std::ios::binary | std::ios::trunc);
                 f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                 if (!f) throw ToolError("Could not write " + file.string());
                 out["path"] = args["path"];
             }
             out["mcp_content"] = Json::array({{{"type", "image"}, {"data", Base64Encode(png)}, {"mimeType", "image/png"}}});
             return out;
         }});

    server.AddTool(
        {"nav_simulate",
         "Run a crowd of NavAgents over the mesh headless: each of `agents` ([{start, goal, radius?, max_speed?, max_acceleration?, avoidance_quality?, allow_partial?}], "
         "up to 64) is placed on the mesh at its start and told to walk to its goal; the engine's own crowd steers them (avoiding each other). Reports each agent's "
         "outcome (arrived, failed, still moving), when it arrived, how far it walked against the straight path length, and the closest any two agents came.",
         Schema({{"agents", {{"type", "array"}}}, {"seconds", {{"type", "number"}, {"description", "Up to 120 (default 20)"}}}, {"dt", {{"type", "number"}, {"description", "1/120 to 1/10 (default 1/30)"}}},
                 {"sample_every", {{"type", "number"}, {"description", "Seconds between trajectory samples (default 1)"}}}},
                {"agents"}),
         [bench](const Json& args) -> Json {
             RequireMesh(*bench);
             if (!args["agents"].is_array() || args["agents"].empty() || args["agents"].size() > 64) throw ToolError("\"agents\" must be 1 to 64 agent objects");
             const double seconds = Number(args, "seconds", 20.0, 0.05, 120.0);
             const double dt = Number(args, "dt", 1.0 / 30.0, 1.0 / 120.0, 0.1);
             const double sample_every = Number(args, "sample_every", 1.0, 0.05, 120.0);

             World world;
             nav::NavWorld nav_world(world, [](const ModelRenderer&, std::vector<Vec3>&, std::vector<u32>&) { return false; });
             std::string error;
             if (!nav_world.Load(bench->mesh->Data(), &error)) throw ToolError("Could not load the mesh into a navigation world: " + error);
             nav::NavCrowd crowd(world, nav_world, static_cast<i32>(args["agents"].size()) + 8);

             struct Tracked {
                 Entity entity;
                 Vec3 start, goal, last;
                 f64 walked = 0.0, arrived_at = -1.0;
                 NavMoveStatus final_status = NavMoveStatus::Idle;
                 Json samples = Json::array();
             };
             std::vector<Tracked> agents;
             for (const Json& a : args["agents"]) {
                 if (!a.is_object() || !a.contains("start") || !a.contains("goal")) throw ToolError("Each agent needs start and goal");
                 Vec3 start = V3(a["start"], "An agent's start"), goal = V3(a["goal"], "An agent's goal");
                 Vec3 snapped;
                 if (!bench->mesh->NearestPoint(start, snapped, Vec3(4, 8, 4))) throw ToolError("An agent's start is not near the mesh");
                 start = snapped;
                 NavAgent agent;
                 agent.radius = static_cast<f32>(Number(a, "radius", 0.5, 0.05, 4.0));
                 agent.max_speed = static_cast<f32>(Number(a, "max_speed", 3.5, 0.0, 100.0));
                 agent.max_acceleration = static_cast<f32>(Number(a, "max_acceleration", 8.0, 0.0, 1000.0));
                 agent.avoidance_quality = static_cast<i32>(Number(a, "avoidance_quality", 3, 0, 3));
                 agent.goal_tolerance = 4.0f;
                 if (a.contains("allow_partial")) {
                     if (!a["allow_partial"].is_boolean()) throw ToolError("\"allow_partial\" must be true or false");
                     agent.allow_partial = a["allow_partial"].get<bool>();
                 }
                 Entity e = world.CreateEntity(Transform{start, Quaternion::Identity()});
                 world.AddComponent(e, agent);
                 world.GetComponent<NavAgent>(e)->MoveTo(goal);
                 Tracked t;
                 t.entity = e;
                 t.start = t.last = start;
                 t.goal = goal;
                 agents.push_back(std::move(t));
             }

             const unsigned steps = static_cast<unsigned>(std::ceil(seconds / dt));
             const unsigned sample_steps = std::max(1u, static_cast<unsigned>(sample_every / dt));
             double closest = 1.0e30;
             double time = 0.0;
             for (unsigned i = 1; i <= steps; ++i) {
                 crowd.Update(static_cast<f32>(dt));
                 time += dt;
                 bool any_moving = false;
                 for (usize k = 0; k < agents.size(); ++k) {
                     Tracked& t = agents[k];
                     const Transform* tr = world.GetComponent<Transform>(t.entity);
                     const NavAgent* na = world.GetComponent<NavAgent>(t.entity);
                     if (tr == nullptr || na == nullptr) continue;
                     t.walked += (tr->position - t.last).Length();
                     t.last = tr->position;
                     t.final_status = na->status;
                     if (na->status == NavMoveStatus::Arrived && t.arrived_at < 0.0) t.arrived_at = time;
                     if (na->status == NavMoveStatus::Moving) any_moving = true;
                     for (usize j = k + 1; j < agents.size(); ++j) {
                         const Transform* other = world.GetComponent<Transform>(agents[j].entity);
                         if (other != nullptr) closest = std::min(closest, static_cast<double>((tr->position - other->position).Length()));
                     }
                     if (i % sample_steps == 0 && t.samples.size() < 200) t.samples.push_back({{"time", time}, {"position", Vec3Json(tr->position)}, {"status", MoveStatusName(na->status)}});
                 }
                 if (!any_moving && i > 2) break;
             }

             Json results = Json::array();
             usize arrived = 0, failed = 0;
             for (const Tracked& t : agents) {
                 nav::NavPath path;
                 const bool found = bench->mesh->FindPath(t.start, t.goal, path);
                 Json row = {{"start", Vec3Json(t.start)}, {"goal", Vec3Json(t.goal)}, {"final_position", Vec3Json(t.last)}, {"status", MoveStatusName(t.final_status)},
                             {"distance_walked", t.walked}, {"samples", t.samples}};
                 if (found && path.status != nav::PathStatus::None) row["shortest_path_length"] = path.Length();
                 row["arrived_at"] = t.arrived_at >= 0.0 ? Json(t.arrived_at) : Json(nullptr);
                 arrived += t.final_status == NavMoveStatus::Arrived ? 1 : 0;
                 failed += t.final_status == NavMoveStatus::Failed ? 1 : 0;
                 results.push_back(row);
             }
             Json out = {{"simulated_seconds", time}, {"arrived", arrived}, {"failed", failed}, {"still_moving", agents.size() - arrived - failed}, {"agents", results}, {"problems", crowd.Problems()}};
             out["closest_agent_distance"] = agents.size() > 1 ? Json(closest) : Json(nullptr);
             return out;
         }});

    server.AddTool(
        {"nav_export", "Save the baked mesh as a .anav file (the engine's navigation mesh format).", Schema({{"path", {{"type", "string"}, {"description", "File path (.anav added if missing)"}}}}, {"path"}),
         [bench](const Json& args) -> Json {
             RequireMesh(*bench);
             if (!args["path"].is_string() || args["path"].get<std::string>().empty()) throw ToolError("\"path\" must be a file path");
             fs::path file = args["path"].get<std::string>();
             if (file.extension() != ".anav") file += ".anav";
             const std::vector<u8> bytes = nav::SaveNavMesh(bench->mesh->Data());
             std::error_code ec;
             if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
             std::ofstream f(file, std::ios::binary | std::ios::trunc);
             f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
             if (!f) throw ToolError("Could not write " + file.string());
             return {{"path", file.string()}, {"bytes", bytes.size()}};
         }});

    server.AddTool({"nav_import", "Load a .anav navigation mesh into the workbench (replacing the baked mesh; the geometry is kept but no longer matches).",
                    Schema({{"path", {{"type", "string"}}}}, {"path"}), [bench](const Json& args) -> Json {
                        if (!args["path"].is_string()) throw ToolError("\"path\" must be a file path");
                        std::ifstream in(args["path"].get<std::string>(), std::ios::binary);
                        if (!in) throw ToolError("Could not read " + args["path"].get<std::string>());
                        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        nav::NavMeshData data;
                        std::string error;
                        if (!nav::LoadNavMesh(std::vector<u8>(raw.begin(), raw.end()), data, &error)) throw ToolError("Not a navigation mesh: " + error);
                        auto mesh = std::make_unique<nav::NavMesh>();
                        if (!mesh->Load(data, &error)) throw ToolError("Could not load the mesh: " + error);
                        bench->mesh = std::move(mesh);
                        bench->settings = data.settings;
                        bench->from_geometry = false;
                        return MeshJson(*bench);
                    }});
}

} // namespace aether::mcp
