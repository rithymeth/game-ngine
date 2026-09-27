#include "aether/nav/components.h"

#include "aether/scene/hierarchy.h"

#include <algorithm>
#include <cmath>

namespace aether {

void RegisterNavComponents() {
    (void)GetComponentId<NavObstacle>();
    (void)GetComponentId<NavModifierVolume>();
    (void)GetComponentId<NavLinkProxy>();
}

} // namespace aether

namespace aether::nav {

namespace {
NavWorld* g_active = nullptr;

Vec3 Rotate(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    const Vec3 t = u.Cross(v) * 2.0f;
    return v + t * q.w + u.Cross(t);
}

f32 YawDegrees(const Quaternion& q) {
    const Vec3 f = Rotate(q, Vec3(0, 0, 1));
    return std::atan2(f.x, f.z) * 180.0f / 3.14159265f;
}

u8 Area(i32 a) { return static_cast<u8>(std::clamp(a, 0, 63)); }
} // namespace

NavWorld::NavWorld(World& world, MeshProvider meshes, const GuidIndex* guids) : world_(world), meshes_(std::move(meshes)), guids_(guids) {
    RegisterNavComponents();
    MakeActive();
}

NavWorld::~NavWorld() {
    if (g_active == this) g_active = nullptr;
}

NavWorld* NavWorld::Active() { return g_active; }
void NavWorld::MakeActive() { g_active = this; }

bool NavWorld::WorldPose(Entity e, Pose& out) const {
    const Transform* t = world_.IsAlive(e) ? world_.GetComponent<Transform>(e) : nullptr;
    if (t == nullptr) return false;
    out.position = t->position;
    out.rotation = t->rotation;
    if (guids_ != nullptr) {
        Entity at = GetParent(world_, *guids_, e);
        for (int depth = 0; !at.IsNull() && depth < kMaxHierarchyDepth; ++depth) {
            const Transform* p = world_.GetComponent<Transform>(at);
            if (p == nullptr) break;
            out.position = p->position + Rotate(p->rotation, out.position);
            out.rotation = p->rotation * out.rotation;
            at = GetParent(world_, *guids_, at);
        }
    }
    return true;
}

template <typename C>
std::vector<Entity> NavWorld::With() const {
    std::vector<Entity> out;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<C>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            out.insert(out.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    return out;
}

NavGeometry NavWorld::Gather() const {
    NavGeometry g;
    if (!meshes_) return g;
    std::vector<Vec3> verts;
    std::vector<u32> idx;
    for (Entity e : With<ModelRenderer>()) {
        if (const NavObstacle* o = world_.GetComponent<NavObstacle>(e); o != nullptr && o->carve) continue; // dynamic: carved instead
        Pose pose;
        if (!WorldPose(e, pose)) continue;
        verts.clear(), idx.clear();
        if (!meshes_(*world_.GetComponent<ModelRenderer>(e), verts, idx)) continue;
        for (Vec3& v : verts) v = pose.position + Rotate(pose.rotation, v);
        g.AddTriangles(verts, idx);
    }
    return g;
}

bool NavWorld::Bake(const NavMeshSettings& settings, std::string* error, NavBuildStats* stats) {
    obstacles_.clear(), modifiers_.clear(), links_.clear();
    nav_.Reset();
    Sync(); // the volumes and links first, so the bake below has them
    if (!nav_.Build(Gather(), settings, error, stats)) return false;
    nav_.Update(0, error); // the tiles they touch
    return true;
}

bool NavWorld::Load(const NavMeshData& data, std::string* error) {
    obstacles_.clear(), modifiers_.clear(), links_.clear();
    nav_.Reset();
    if (!nav_.Load(data, Gather(), error)) return false;
    Sync();
    nav_.Update(0, error);
    return true;
}

void NavWorld::Sync() {
    ++frame_;
    const auto follow = [&](std::unordered_map<u32, Tracked>& tracked, Entity e, bool wanted, const auto& make, bool moved_enough_only,
                            f32 threshold, bool is_link) {
        auto it = tracked.find(e.index);
        if (it != tracked.end() && it->second.entity != e) { // a recycled entity
            is_link ? nav_.RemoveLink(it->second.id) : nav_.RemoveVolume(it->second.id);
            tracked.erase(it);
            it = tracked.end();
        }
        Pose pose;
        if (!wanted || !WorldPose(e, pose)) return;
        if (it == tracked.end()) {
            Tracked t{e, 0, pose, frame_};
            t.id = make(pose, true);
            tracked.emplace(e.index, t);
            return;
        }
        Tracked& t = it->second;
        t.seen = frame_;
        if (moved_enough_only) {
            const f32 moved = (pose.position - t.pose.position).Length();
            f32 turned = std::fabs(YawDegrees(pose.rotation) - YawDegrees(t.pose.rotation));
            turned = std::min(turned, 360.0f - turned);
            // Shape edits always apply; motion waits for the threshold.
            if (moved < threshold && turned < 5.0f) pose = t.pose;
        }
        t.pose = pose;
        make(pose, false);
    };
    const auto sweep = [&](std::unordered_map<u32, Tracked>& tracked, bool is_link) {
        for (auto it = tracked.begin(); it != tracked.end();) {
            if (it->second.seen != frame_) {
                is_link ? nav_.RemoveLink(it->second.id) : nav_.RemoveVolume(it->second.id);
                it = tracked.erase(it);
            } else {
                ++it;
            }
        }
    };

    for (Entity e : With<NavObstacle>()) {
        const NavObstacle o = *world_.GetComponent<NavObstacle>(e);
        auto& tracked = obstacles_;
        follow(tracked, e, o.carve, [&](const Pose& p, bool add) {
            const Vec3 c = p.position + Rotate(p.rotation, o.center);
            const NavVolume v = o.shape == NavObstacleShape::Box ? NavVolume::Box(c, o.half_extents, kAreaNull, YawDegrees(p.rotation))
                                                                  : NavVolume::Cylinder(c, o.radius, o.height, kAreaNull);
            if (add) return nav_.AddVolume(v);
            nav_.UpdateVolume(tracked.at(e.index).id, v);
            return DynamicNavMesh::Id{0};
        }, true, o.move_threshold, false);
    }
    sweep(obstacles_, false);

    for (Entity e : With<NavModifierVolume>()) {
        const NavModifierVolume m = *world_.GetComponent<NavModifierVolume>(e);
        follow(modifiers_, e, true, [&](const Pose& p, bool add) {
            const NavVolume v = NavVolume::Box(p.position, m.half_extents, Area(m.area), YawDegrees(p.rotation));
            if (add) return nav_.AddVolume(v);
            nav_.UpdateVolume(modifiers_.at(e.index).id, v);
            return DynamicNavMesh::Id{0};
        }, false, 0.0f, false);
    }
    sweep(modifiers_, false);

    for (Entity e : With<NavLinkProxy>()) {
        const NavLinkProxy l = *world_.GetComponent<NavLinkProxy>(e);
        follow(links_, e, l.enabled, [&](const Pose& p, bool add) {
            NavLink link;
            link.start = p.position + Rotate(p.rotation, l.start);
            link.end = p.position + Rotate(p.rotation, l.end);
            link.radius = l.radius;
            link.bidirectional = l.bidirectional;
            link.area = std::max<u8>(1, Area(l.area));
            link.user_id = static_cast<u32>(l.user_id);
            if (add) return nav_.AddLink(link);
            nav_.UpdateLink(links_.at(e.index).id, link);
            return DynamicNavMesh::Id{0};
        }, false, 0.0f, true);
    }
    sweep(links_, true);
}

void NavWorld::Update(usize max_tiles) {
    Sync();
    nav_.Update(max_tiles);
}

} // namespace aether::nav
