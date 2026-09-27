#pragma once

#include "aether/nav/navmesh.h"

#include <map>
#include <set>

namespace aether::nav {

// A navigation mesh that changes at runtime (Phase 20 step 2,
// docs/design/PHASE_SPECS.md §20.2): volumes (carving obstacles, or areas)
// and off-mesh links come and go, and only the tiles they touch are
// rebaked, a few per Update so a frame never stalls.
class DynamicNavMesh {
public:
    using Id = u32; // 0 is never used

    // Bakes everything from `geometry` (its own volumes and links are the static ones).
    bool Build(NavGeometry geometry, const NavMeshSettings& settings, std::string* error = nullptr, NavBuildStats* stats = nullptr);
    // Starts from a baked mesh (a loaded .anav), keeping `geometry` for rebakes.
    bool Load(const NavMeshData& data, NavGeometry geometry, std::string* error = nullptr);

    // Forgets the mesh, volumes and links.
    void Reset();

    Id AddVolume(const NavVolume& volume);
    bool UpdateVolume(Id id, const NavVolume& volume); // false for an unknown id
    bool RemoveVolume(Id id);
    const NavVolume* Volume(Id id) const;
    usize VolumeCount() const { return volumes_.size(); }

    Id AddLink(const NavLink& link);
    bool UpdateLink(Id id, const NavLink& link);
    bool RemoveLink(Id id);
    usize LinkCount() const { return links_.size(); }

    // Rebakes up to `max_tiles` changed tiles (0: all of them). The count rebaked.
    usize Update(usize max_tiles = 4, std::string* error = nullptr);
    usize PendingTiles() const { return dirty_.size(); }
    bool TilePending(i32 x, i32 z) const { return dirty_.count({x, z}) != 0; }
    void MarkAllDirty();
    // Goes up whenever tiles change (agents replan when it moves).
    u64 Revision() const { return revision_; }
    usize TilesRebuilt() const { return rebuilt_; } // in all

    NavMesh& Mesh() { return mesh_; }
    const NavMesh& Mesh() const { return mesh_; }
    const NavGeometry& Geometry() const { return geometry_; }

private:
    void Touch(const Vec3& lo, const Vec3& hi);
    void TouchVolume(const NavVolume& v);
    void TouchLink(const NavLink& l);

    NavGeometry geometry_;
    NavMesh mesh_;
    std::map<Id, NavVolume> volumes_;
    std::map<Id, NavLink> links_;
    std::set<std::pair<i32, i32>> dirty_;
    Id next_id_ = 1;
    u64 revision_ = 0;
    usize rebuilt_ = 0;
};

} // namespace aether::nav
