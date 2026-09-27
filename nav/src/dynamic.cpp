#include "aether/nav/dynamic.h"

#include <algorithm>

namespace aether::nav {

namespace {
bool Same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Same(const NavVolume& a, const NavVolume& b) {
    if (a.shape != b.shape || a.area != b.area || !Same(a.center, b.center) || a.points.size() != b.points.size()) return false;
    for (usize i = 0; i < a.points.size(); ++i)
        if (!Same(a.points[i], b.points[i])) return false;
    return Same(a.half_extents, b.half_extents) && a.yaw_degrees == b.yaw_degrees && a.radius == b.radius && a.height == b.height && a.min_y == b.min_y &&
           a.max_y == b.max_y;
}
bool Same(const NavLink& a, const NavLink& b) {
    return Same(a.start, b.start) && Same(a.end, b.end) && a.radius == b.radius && a.bidirectional == b.bidirectional && a.area == b.area &&
           a.user_id == b.user_id;
}
} // namespace

bool DynamicNavMesh::Build(NavGeometry geometry, const NavMeshSettings& settings, std::string* error, NavBuildStats* stats) {
    NavMeshData data;
    if (!BuildNavMesh(geometry, settings, data, error, stats)) return false;
    return Load(data, std::move(geometry), error);
}

bool DynamicNavMesh::Load(const NavMeshData& data, NavGeometry geometry, std::string* error) {
    if (!mesh_.Load(data, error)) return false;
    geometry_ = std::move(geometry);
    dirty_.clear();
    // Dynamic volumes and links added before a (re)build are baked in by the next Update.
    for (const auto& [id, v] : volumes_) TouchVolume(v);
    for (const auto& [id, l] : links_) TouchLink(l);
    ++revision_;
    return true;
}

void DynamicNavMesh::Reset() {
    mesh_.Unload();
    geometry_ = {};
    volumes_.clear();
    links_.clear();
    dirty_.clear();
    ++revision_;
}

void DynamicNavMesh::Touch(const Vec3& lo, const Vec3& hi) {
    if (!mesh_.Loaded()) return;
    for (const auto& t : NavTilesTouching(mesh_.Data(), lo, hi)) dirty_.insert(t);
}

void DynamicNavMesh::TouchVolume(const NavVolume& v) {
    Vec3 lo, hi;
    v.Bounds(lo, hi);
    Touch(lo, hi);
}

void DynamicNavMesh::TouchLink(const NavLink& l) {
    // The tile holding the link is the start's; its neighbours connect to the end, so rebake both ends' tiles.
    Touch(l.start, l.start);
    Touch(l.end, l.end);
}

DynamicNavMesh::Id DynamicNavMesh::AddVolume(const NavVolume& v) {
    const Id id = next_id_++;
    volumes_[id] = v;
    TouchVolume(v);
    return id;
}

bool DynamicNavMesh::UpdateVolume(Id id, const NavVolume& v) {
    auto it = volumes_.find(id);
    if (it == volumes_.end()) return false;
    if (Same(it->second, v)) return true;
    TouchVolume(it->second);
    it->second = v;
    TouchVolume(v);
    return true;
}

bool DynamicNavMesh::RemoveVolume(Id id) {
    auto it = volumes_.find(id);
    if (it == volumes_.end()) return false;
    TouchVolume(it->second);
    volumes_.erase(it);
    return true;
}

const NavVolume* DynamicNavMesh::Volume(Id id) const {
    auto it = volumes_.find(id);
    return it == volumes_.end() ? nullptr : &it->second;
}

DynamicNavMesh::Id DynamicNavMesh::AddLink(const NavLink& l) {
    const Id id = next_id_++;
    links_[id] = l;
    TouchLink(l);
    return id;
}

bool DynamicNavMesh::UpdateLink(Id id, const NavLink& l) {
    auto it = links_.find(id);
    if (it == links_.end()) return false;
    if (Same(it->second, l)) return true;
    TouchLink(it->second);
    it->second = l;
    TouchLink(l);
    return true;
}

bool DynamicNavMesh::RemoveLink(Id id) {
    auto it = links_.find(id);
    if (it == links_.end()) return false;
    TouchLink(it->second);
    links_.erase(it);
    return true;
}

void DynamicNavMesh::MarkAllDirty() {
    if (!mesh_.Loaded()) return;
    const NavMeshData& d = mesh_.Data();
    for (i32 z = 0; z < d.tiles_z; ++z)
        for (i32 x = 0; x < d.tiles_x; ++x) dirty_.insert({x, z});
}

usize DynamicNavMesh::Update(usize max_tiles, std::string* error) {
    if (dirty_.empty() || !mesh_.Loaded()) return 0;
    std::vector<NavVolume> volumes;
    volumes.reserve(volumes_.size());
    for (const auto& [id, v] : volumes_) volumes.push_back(v);
    std::vector<NavLink> links;
    links.reserve(links_.size());
    for (const auto& [id, l] : links_) links.push_back(l);
    // The grid alone (ReplaceTile changes the mesh's tiles).
    NavMeshData grid;
    grid.settings = mesh_.Data().settings;
    grid.origin = mesh_.Data().origin;
    grid.tile_world_size = mesh_.Data().tile_world_size;
    grid.tiles_x = mesh_.Data().tiles_x;
    grid.tiles_z = mesh_.Data().tiles_z;
    usize done = 0;
    while (!dirty_.empty() && (max_tiles == 0 || done < max_tiles)) {
        const auto [x, z] = *dirty_.begin();
        dirty_.erase(dirty_.begin());
        NavMeshData::Tile tile;
        if (!BuildNavTile(geometry_, grid, x, z, tile, error, &volumes, &links)) return done;
        if (!mesh_.ReplaceTile(tile, error)) return done;
        ++done;
    }
    rebuilt_ += done;
    if (done > 0) ++revision_;
    return done;
}

} // namespace aether::nav
