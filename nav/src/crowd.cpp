#include "aether/nav/crowd.h"

#include <DetourCrowd.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether {

void NavAgent::MoveTo(const Vec3& location) { commands.push_back({NavAgentCommand::Kind::MoveTo, location, kNullEntity}); }
void NavAgent::MoveToEntity(const Entity& target) { commands.push_back({NavAgentCommand::Kind::MoveToEntity, Vec3(), target}); }
void NavAgent::Stop() { commands.push_back({NavAgentCommand::Kind::Stop, Vec3(), kNullEntity}); }
void NavAgent::Warp(const Vec3& location) { commands.push_back({NavAgentCommand::Kind::Warp, location, kNullEntity}); }

namespace {
const nav::NavMesh* ActiveMesh() {
    const nav::NavWorld* w = nav::NavWorld::Active();
    return w != nullptr && w->Ready() ? &w->Mesh() : nullptr;
}
} // namespace

bool Navigation::IsReachable(const Vec3& from, const Vec3& to) {
    const nav::NavMesh* m = ActiveMesh();
    return m != nullptr && m->Reachable(from, to);
}

f32 Navigation::PathLength(const Vec3& from, const Vec3& to) {
    const nav::NavMesh* m = ActiveMesh();
    nav::NavPath path;
    if (m == nullptr || !m->FindPath(from, to, path) || path.status != nav::PathStatus::Complete) return -1.0f;
    return path.Length();
}

Vec3 Navigation::ProjectPoint(const Vec3& point) {
    const nav::NavMesh* m = ActiveMesh();
    Vec3 out;
    return m != nullptr && m->NearestPoint(point, out) ? out : point;
}

bool Navigation::IsOnNavMesh(const Vec3& point) {
    const nav::NavMesh* m = ActiveMesh();
    Vec3 out;
    if (m == nullptr || !m->NearestPoint(point, out, Vec3(0.5f, 2, 0.5f))) return false;
    const f32 dx = out.x - point.x, dz = out.z - point.z;
    return dx * dx + dz * dz < 0.01f;
}

Vec3 Navigation::RandomReachablePoint(const Vec3& origin, f32 radius) {
    static u64 calls = 0;
    const nav::NavMesh* m = ActiveMesh();
    Vec3 out;
    return m != nullptr && m->RandomPointNear(origin, radius, ++calls, out) ? out : origin;
}

Vec3 Navigation::Raycast(const Vec3& from, const Vec3& to) {
    const nav::NavMesh* m = ActiveMesh();
    Vec3 hit;
    return m != nullptr && m->Raycast(from, to, hit) ? hit : to;
}

void RegisterNavAgentComponents() {
    RegisterNavComponents();
    (void)GetComponentId<NavAgent>();
}

} // namespace aether

namespace aether::nav {

namespace {
constexpr f32 kPi = 3.14159265f;
f32 Flat(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }
f32 YawOf(const Quaternion& q) {
    const Vec3 u(q.x, q.y, q.z), v(0, 0, 1);
    const Vec3 t = u.Cross(v) * 2.0f;
    const Vec3 f = v + t * q.w + u.Cross(t);
    return std::atan2(f.x, f.z);
}
void FreeCrowd(dtCrowd* c) { dtFreeCrowd(c); }
} // namespace

NavCrowd::NavCrowd(World& world, NavWorld& nav, i32 max_agents) : world_(world), nav_(nav), max_agents_(max_agents), crowd_(nullptr, FreeCrowd) {
    RegisterNavAgentComponents();
}

NavCrowd::~NavCrowd() = default;

bool NavCrowd::Ensure() {
    const dtNavMesh* mesh = nav_.Ready() ? nav_.Mesh().Detour() : nullptr;
    if (mesh == nullptr) {
        crowd_.reset();
        mesh_ = nullptr;
        for (auto& [key, a] : agents_) a.index = -1;
        return false;
    }
    if (crowd_ && mesh_ == mesh) return true;
    // A new mesh (a bake or load): a new crowd, with every agent added again.
    crowd_.reset(dtAllocCrowd());
    if (!crowd_ || !crowd_->init(max_agents_, 4.0f, const_cast<dtNavMesh*>(mesh))) {
        crowd_.reset();
        problems_.push_back("the navigation crowd couldn't be set up");
        return false;
    }
    mesh_ = mesh;
    revision_ = nav_.Dynamic().Revision();
    // Avoidance at four qualities (as in Recast's demo).
    dtObstacleAvoidanceParams p;
    std::memcpy(&p, crowd_->getObstacleAvoidanceParams(0), sizeof(p));
    const u8 divs[4] = {5, 5, 7, 7}, rings[4] = {2, 2, 2, 3}, depth[4] = {1, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        p.velBias = 0.5f;
        p.adaptiveDivs = divs[i];
        p.adaptiveRings = rings[i];
        p.adaptiveDepth = depth[i];
        crowd_->setObstacleAvoidanceParams(i, &p);
    }
    for (auto& [key, a] : agents_) a.index = -1;
    return true;
}

void NavCrowd::Configure(Agent& a, const NavAgent& c) {
    dtCrowdAgentParams p{};
    p.radius = std::clamp(c.radius, 0.05f, 4.0f);
    p.height = std::max(c.height, 0.1f);
    p.maxAcceleration = std::max(c.max_acceleration, 0.0f);
    p.maxSpeed = std::max(c.max_speed, 0.0f);
    p.collisionQueryRange = p.radius * 12.0f;
    p.pathOptimizationRange = p.radius * 30.0f;
    p.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
    const i32 quality = std::clamp(c.avoidance_quality, 0, 3);
    if (quality > 0) p.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE;
    if (c.separation_weight > 0.0f) p.updateFlags |= DT_CROWD_SEPARATION;
    p.obstacleAvoidanceType = static_cast<unsigned char>(quality);
    p.separationWeight = c.separation_weight;
    p.queryFilterType = 0;
    if (a.index < 0) {
        const Transform* t = world_.GetComponent<Transform>(a.entity);
        Vec3 at = t != nullptr ? t->position : Vec3();
        Vec3 on;
        if (nav_.Mesh().NearestPoint(at - Vec3(0, c.base_offset, 0), on)) at = on;
        const float pos[3] = {at.x, at.y, at.z};
        a.index = crowd_->addAgent(pos, &p);
        if (a.index < 0) problems_.push_back("too many navigation agents (the crowd holds " + std::to_string(max_agents_) + ")");
    } else {
        crowd_->updateAgentParameters(a.index, &p);
    }
}

void NavCrowd::Finish(Agent& a, NavAgent& c, bool success) {
    if (a.index >= 0) crowd_->resetMoveTarget(a.index);
    a.follow = kNullEntity;
    c.status = success ? NavMoveStatus::Arrived : NavMoveStatus::Failed;
    c.remaining = success ? 0.0f : c.remaining;
    events_.push_back({a.entity, success});
}

void NavCrowd::Request(Agent& a, NavAgent& c, const Vec3& target) {
    a.target = target;
    const dtCrowdAgent* ag = crowd_->getAgent(a.index);
    const Vec3 from(ag->npos[0], ag->npos[1], ag->npos[2]);
    NavPath path;
    // Cut off, or a goal that's off the mesh (inside an obstacle, say): fails unless partial moves are allowed.
    const bool found = nav_.Mesh().FindPath(from, target, path) && !path.points.empty();
    const bool whole = found && path.status == PathStatus::Complete && Flat(path.points.back(), target) <= c.goal_tolerance;
    if (!found || (!whole && !c.allow_partial)) {
        c.status = NavMoveStatus::Moving; // so Finish reports it
        Finish(a, c, false);
        return;
    }
    a.partial = !whole;
    const Vec3 goal = path.points.back();
    const float pos[3] = {goal.x, goal.y, goal.z};
    float nearest[3];
    dtPolyRef ref = 0;
    crowd_->getNavMeshQuery()->findNearestPoly(pos, crowd_->getQueryExtents(), crowd_->getFilter(0), &ref, nearest);
    if (ref == 0 || !crowd_->requestMoveTarget(a.index, ref, nearest)) {
        c.status = NavMoveStatus::Moving;
        Finish(a, c, false);
        return;
    }
    c.goal = Vec3(nearest[0], nearest[1], nearest[2]);
    c.remaining = Flat(from, c.goal);
    c.status = NavMoveStatus::Moving;
}

void NavCrowd::Remove(u32 key) {
    auto it = agents_.find(key);
    if (it == agents_.end()) return;
    if (crowd_ && it->second.index >= 0) crowd_->removeAgent(it->second.index);
    agents_.erase(it);
}

void NavCrowd::Update(f32 dt) {
    events_.clear();
    const bool ready = Ensure();

    // Agents that went, or were recycled.
    std::vector<u32> gone;
    for (const auto& [key, a] : agents_) {
        if (!world_.IsAlive(a.entity) || world_.GetComponent<NavAgent>(a.entity) == nullptr) gone.push_back(key);
    }
    for (u32 key : gone) Remove(key);

    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<NavAgent>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    if (!ready) return; // commands wait for a mesh

    const bool replan = nav_.Dynamic().Revision() != revision_;
    revision_ = nav_.Dynamic().Revision();
    for (Entity e : entities) {
        NavAgent& c = *world_.GetComponent<NavAgent>(e);
        auto [it, added] = agents_.try_emplace(e.index);
        Agent& a = it->second;
        if (!added && a.entity != e) {
            if (a.index >= 0) crowd_->removeAgent(a.index);
            a = Agent{};
        }
        a.entity = e;
        const bool fresh = a.index < 0;
        Configure(a, c);
        if (a.index < 0) continue;
        if (fresh && c.status == NavMoveStatus::Moving && a.follow.IsNull()) Request(a, c, a.target); // a new crowd: carry on

        for (const NavAgentCommand& cmd : c.commands) {
            switch (cmd.kind) {
            case NavAgentCommand::Kind::MoveTo:
                a.follow = kNullEntity;
                Request(a, c, cmd.point);
                break;
            case NavAgentCommand::Kind::MoveToEntity: {
                const Transform* t = world_.IsAlive(cmd.target) ? world_.GetComponent<Transform>(cmd.target) : nullptr;
                if (t == nullptr) {
                    c.status = NavMoveStatus::Moving;
                    Finish(a, c, false);
                    break;
                }
                Request(a, c, t->position);
                if (c.status == NavMoveStatus::Moving) {
                    a.follow = cmd.target;
                    a.follow_seen = t->position;
                }
                break;
            }
            case NavAgentCommand::Kind::Stop:
                crowd_->resetMoveTarget(a.index);
                a.follow = kNullEntity;
                c.status = NavMoveStatus::Idle;
                break;
            case NavAgentCommand::Kind::Warp: {
                crowd_->removeAgent(a.index);
                a.index = -1;
                a.follow = kNullEntity;
                if (Transform* t = world_.GetComponent<Transform>(e)) t->position = cmd.point;
                Configure(a, c);
                c.status = NavMoveStatus::Idle;
                break;
            }
            }
            if (a.index < 0) break;
        }
        c.commands.clear();
        if (a.index < 0 || c.status != NavMoveStatus::Moving) continue;

        // Following: go after the target when it has moved on; give up when it's gone.
        if (!a.follow.IsNull()) {
            const Transform* t = world_.IsAlive(a.follow) ? world_.GetComponent<Transform>(a.follow) : nullptr;
            if (t == nullptr) {
                Finish(a, c, false);
                continue;
            }
            if (Flat(t->position, a.follow_seen) > std::max(0.5f, c.stopping_distance)) {
                const Entity follow = a.follow;
                Request(a, c, t->position);
                a.follow = c.status == NavMoveStatus::Moving ? follow : kNullEntity;
                a.follow_seen = t->position;
            }
        } else if (replan) {
            Request(a, c, a.target); // the mesh changed under it
        }
    }

    crowd_->update(dt, nullptr);

    for (Entity e : entities) {
        auto it = agents_.find(e.index);
        if (it == agents_.end() || it->second.index < 0) continue;
        Agent& a = it->second;
        NavAgent& c = *world_.GetComponent<NavAgent>(e);
        const dtCrowdAgent* ag = crowd_->getAgent(a.index);
        if (ag == nullptr || !ag->active) continue;
        const Vec3 pos(ag->npos[0], ag->npos[1], ag->npos[2]);
        c.velocity = Vec3(ag->vel[0], ag->vel[1], ag->vel[2]);
        if (Transform* t = world_.GetComponent<Transform>(e)) {
            t->position = pos + Vec3(0, c.base_offset, 0);
            const f32 speed = std::sqrt(c.velocity.x * c.velocity.x + c.velocity.z * c.velocity.z);
            if (c.update_rotation && speed > 0.1f) {
                const f32 want = std::atan2(c.velocity.x, c.velocity.z);
                const f32 have = YawOf(t->rotation);
                f32 diff = std::remainder(want - have, 2.0f * kPi);
                const f32 step = c.turn_speed * kPi / 180.0f * dt;
                diff = std::clamp(diff, -step, step);
                t->rotation = Quaternion::FromAxisAngle(Vec3(0, 1, 0), have + diff);
            }
        }
        if (c.status != NavMoveStatus::Moving) continue;
        c.remaining = Flat(pos, c.goal);
        if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED) {
            Finish(a, c, false);
        } else if (c.remaining <= std::max(c.stopping_distance, 0.05f)) {
            Finish(a, c, true);
        }
    }
}

std::vector<Vec3> NavCrowd::Corners(Entity e) const {
    std::vector<Vec3> out;
    auto it = agents_.find(e.index);
    if (!crowd_ || it == agents_.end() || it->second.index < 0) return out;
    const dtCrowdAgent* ag = crowd_->getAgent(it->second.index);
    for (int i = 0; ag != nullptr && i < ag->ncorners; ++i) out.push_back(Vec3(ag->cornerVerts[i * 3], ag->cornerVerts[i * 3 + 1], ag->cornerVerts[i * 3 + 2]));
    return out;
}

} // namespace aether::nav
