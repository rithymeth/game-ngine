#include "aether/physics/physics_scene.h"

#include "aether/core/log.h"
#include "aether/reflection/serialize.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>
#include <unordered_set>

namespace aether {

namespace {

JPH::Vec3 ToJolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat ToJolt(const Quaternion& q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }
Quaternion ToAether(JPH::QuatArg q) { return Quaternion(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

bool Same(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
bool Same(const Quaternion& a, const Quaternion& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }

// One collider's shape, before its offset.
struct Part {
    JPH::RefConst<JPH::Shape> shape;
    Vec3 center;
    bool trigger = false;
    f32 friction = 0.5f, restitution = 0.0f;
};

bool Make(const JPH::ShapeSettings& settings, JPH::RefConst<JPH::Shape>& out, std::string& problem, const char* what) {
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        problem = std::string(what) + ": " + result.GetError().c_str();
        return false;
    }
    out = result.Get();
    return true;
}

// The largest convex radius a box can have: Jolt's default, or less for thin boxes.
f32 BoxConvexRadius(const Vec3& half) {
    return std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
}

bool HullOf(const std::vector<Vec3>& points, JPH::RefConst<JPH::Shape>& out, std::string& problem, const char* what) {
    if (points.size() < 4) {
        problem = std::string(what) + ": a hull needs at least 4 points";
        return false;
    }
    JPH::Array<JPH::Vec3> jolt;
    jolt.reserve(points.size());
    for (const Vec3& p : points) jolt.push_back(ToJolt(p));
    return Make(JPH::ConvexHullShapeSettings(jolt), out, problem, what);
}

template <typename T>
void AppendSettings(const World& world, Entity e, u8 tag, std::vector<u8>& out) {
    if (const T* c = world.GetComponent<T>(e)) {
        out.push_back(tag);
        const std::vector<u8> bytes = reflect::SaveBinary(*c);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
}

std::vector<u8> SettingsOf(const World& world, Entity e) {
    std::vector<u8> out;
    AppendSettings<BoxCollider>(world, e, 1, out);
    AppendSettings<SphereCollider>(world, e, 2, out);
    AppendSettings<CapsuleCollider>(world, e, 3, out);
    AppendSettings<ConvexCollider>(world, e, 4, out);
    AppendSettings<MeshCollider>(world, e, 5, out);
    AppendSettings<RigidBody>(world, e, 6, out);
    AppendSettings<Layer>(world, e, 7, out); // a layer change moves the body to another object layer
    return out;
}

} // namespace

const char* PhysicsEventName(PhysicsEventType type) {
    switch (type) {
    case PhysicsEventType::CollisionBegin: return "Event.OnCollisionBegin";
    case PhysicsEventType::CollisionStay: return "Event.OnCollisionStay";
    case PhysicsEventType::CollisionEnd: return "Event.OnCollisionEnd";
    case PhysicsEventType::TriggerEnter: return "Event.OnTriggerEnter";
    case PhysicsEventType::TriggerExit: return "Event.OnTriggerExit";
    }
    return "";
}

PhysicsScene::PhysicsScene(World& world, PhysicsWorld& physics) : world_(world), physics_(physics) {}

PhysicsScene::~PhysicsScene() {
    for (auto& [key, tracked] : bodies_) {
        (void)key;
        if (!tracked.body.IsInvalid()) physics_.DestroyBody(tracked.body);
    }
}

JPH::BodyID PhysicsScene::BodyOf(Entity entity) const {
    auto it = bodies_.find(EntityKey(entity));
    return it == bodies_.end() ? JPH::BodyID() : it->second.body;
}

Entity PhysicsScene::EntityOf(JPH::BodyID body) const {
    auto it = by_body_.find(body.GetIndexAndSequenceNumber());
    return it == by_body_.end() ? kNullEntity : EntityFromKey(it->second);
}

void PhysicsScene::Destroy(Tracked& tracked) {
    if (tracked.body.IsInvalid()) return;
    const u32 id = tracked.body.GetIndexAndSequenceNumber();
    graveyard_[id] = by_body_[id]; // its contacts End at the next step
    by_body_.erase(id);
    physics_.SetReportStay(tracked.body, false);
    physics_.DestroyBody(tracked.body);
    tracked.body = JPH::BodyID();
    if (world_.IsAlive(tracked.entity)) {
        if (RigidBody* rb = world_.GetComponent<RigidBody>(tracked.entity)) rb->body_id = JPH::BodyID();
    }
}

bool PhysicsScene::Build(Entity e, Tracked& tracked, std::string& problem) {
    const RigidBody* rb = world_.GetComponent<RigidBody>(e);
    const BodyMotion motion = rb != nullptr ? rb->motion : BodyMotion::Static;

    std::vector<Part> parts;
    auto add = [&](const JPH::ShapeSettings& settings, const char* what, const auto& c) {
        Part part;
        if (!Make(settings, part.shape, problem, what)) return false;
        part.center = c.center;
        part.trigger = c.is_trigger;
        part.friction = c.friction;
        part.restitution = c.restitution;
        parts.push_back(std::move(part));
        return true;
    };
    if (const BoxCollider* c = world_.GetComponent<BoxCollider>(e)) {
        if (!add(JPH::BoxShapeSettings(ToJolt(c->half_extents), BoxConvexRadius(c->half_extents)), "Box collider", *c)) return false;
    }
    if (const SphereCollider* c = world_.GetComponent<SphereCollider>(e)) {
        if (!add(JPH::SphereShapeSettings(c->radius), "Sphere collider", *c)) return false;
    }
    if (const CapsuleCollider* c = world_.GetComponent<CapsuleCollider>(e)) {
        const f32 half_cylinder = std::max(0.0f, 0.5f * c->height - c->radius);
        const bool ok = half_cylinder > 0.0f ? add(JPH::CapsuleShapeSettings(half_cylinder, c->radius), "Capsule collider", *c)
                                             : add(JPH::SphereShapeSettings(c->radius), "Capsule collider", *c);
        if (!ok) return false;
    }
    if (const ConvexCollider* c = world_.GetComponent<ConvexCollider>(e)) {
        Part part;
        if (!HullOf(c->points, part.shape, problem, "Convex collider")) return false;
        part.center = c->center, part.trigger = c->is_trigger, part.friction = c->friction, part.restitution = c->restitution;
        parts.push_back(std::move(part));
    }
    if (const MeshCollider* c = world_.GetComponent<MeshCollider>(e)) {
        Part part;
        if (motion != BodyMotion::Static) {
            // Moving triangle meshes aren't supported by Jolt (or Unity, or Unreal): use the hull.
            if (!HullOf(c->vertices, part.shape, problem, "Mesh collider (moving, so its convex hull)")) return false;
        } else {
            if (c->indices.empty() || c->indices.size() % 3 != 0) {
                problem = "Mesh collider: needs triangles (three indices each)";
                return false;
            }
            JPH::VertexList vertices;
            for (const Vec3& v : c->vertices) vertices.push_back(JPH::Float3(v.x, v.y, v.z));
            JPH::IndexedTriangleList triangles;
            for (usize i = 0; i + 2 < c->indices.size(); i += 3) {
                if (c->indices[i] >= c->vertices.size() || c->indices[i + 1] >= c->vertices.size() ||
                    c->indices[i + 2] >= c->vertices.size()) {
                    problem = "Mesh collider: an index is past the last vertex";
                    return false;
                }
                triangles.push_back(JPH::IndexedTriangle(c->indices[i], c->indices[i + 1], c->indices[i + 2]));
            }
            if (!Make(JPH::MeshShapeSettings(vertices, triangles), part.shape, problem, "Mesh collider")) return false;
        }
        part.center = c->center, part.trigger = c->is_trigger, part.friction = c->friction, part.restitution = c->restitution;
        parts.push_back(std::move(part));
    }
    if (parts.empty()) {
        problem = "no collider";
        return false;
    }

    JPH::RefConst<JPH::Shape> shape;
    if (parts.size() == 1) {
        shape = parts[0].shape;
        if (!Same(parts[0].center, Vec3(0, 0, 0)) &&
            !Make(JPH::RotatedTranslatedShapeSettings(ToJolt(parts[0].center), JPH::Quat::sIdentity(), shape), shape, problem,
                  "Collider offset")) {
            return false;
        }
    } else {
        JPH::StaticCompoundShapeSettings compound;
        for (const Part& p : parts) compound.AddShape(ToJolt(p.center), JPH::Quat::sIdentity(), p.shape);
        if (!Make(compound, shape, problem, "Compound collider")) return false;
    }

    const Transform* t = world_.GetComponent<Transform>(e);
    const Vec3 position = t != nullptr ? t->position : Vec3(0, 0, 0);
    const Quaternion rotation = t != nullptr ? t->rotation : Quaternion::Identity();
    const JPH::EMotionType type = motion == BodyMotion::Static      ? JPH::EMotionType::Static
                                  : motion == BodyMotion::Kinematic ? JPH::EMotionType::Kinematic
                                                                    : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings settings(shape, JPH::RVec3(position.x, position.y, position.z), ToJolt(rotation), type,
                                       physics_.ObjectLayerFor(motion != BodyMotion::Static, LayerOf(world_, e)));
    settings.mUserData = EntityKey(e);
    // A body is a trigger if any of its colliders is (Jolt's sensors are per body).
    settings.mIsSensor = std::any_of(parts.begin(), parts.end(), [](const Part& p) { return p.trigger; });
    settings.mFriction = parts[0].friction;
    settings.mRestitution = parts[0].restitution;
    if (rb != nullptr) {
        settings.mLinearDamping = rb->linear_damping;
        settings.mAngularDamping = rb->angular_damping;
        settings.mGravityFactor = rb->gravity_scale;
        settings.mMotionQuality = rb->continuous_collision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
        if (motion == BodyMotion::Dynamic) {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = std::max(rb->mass, 0.001f);
            JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
            if (rb->lock_rotation_x) dofs = dofs & ~JPH::EAllowedDOFs::RotationX;
            if (rb->lock_rotation_y) dofs = dofs & ~JPH::EAllowedDOFs::RotationY;
            if (rb->lock_rotation_z) dofs = dofs & ~JPH::EAllowedDOFs::RotationZ;
            settings.mAllowedDOFs = dofs;
        }
    }
    // Kinematic sensors should notice static geometry too (a trigger on a moving platform).
    settings.mCollideKinematicVsNonDynamic = motion == BodyMotion::Kinematic && settings.mIsSensor;

    tracked.entity = e;
    tracked.body = physics_.CreateBody(settings, motion != BodyMotion::Static);
    if (tracked.body.IsInvalid()) {
        problem = "the physics world is full";
        return false;
    }
    tracked.position = position;
    tracked.rotation = rotation;
    tracked.motion = motion;
    by_body_[tracked.body.GetIndexAndSequenceNumber()] = EntityKey(e);
    auto stay = [&](const auto* c) { return c != nullptr && c->report_stay; };
    physics_.SetReportStay(tracked.body, stay(world_.GetComponent<BoxCollider>(e)) || stay(world_.GetComponent<SphereCollider>(e)) ||
                                             stay(world_.GetComponent<CapsuleCollider>(e)) ||
                                             stay(world_.GetComponent<ConvexCollider>(e)) ||
                                             stay(world_.GetComponent<MeshCollider>(e)));
    if (RigidBody* body = world_.GetComponent<RigidBody>(e)) body->body_id = tracked.body;
    return true;
}

void PhysicsScene::Sync() {
    // Every entity with a collider, in a stable order (by entity index).
    ComponentMask colliders;
    colliders.set(GetComponentId<BoxCollider>());
    colliders.set(GetComponentId<SphereCollider>());
    colliders.set(GetComponentId<CapsuleCollider>());
    colliders.set(GetComponentId<ConvexCollider>());
    colliders.set(GetComponentId<MeshCollider>());
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if ((archetype.Mask() & colliders).none()) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });

    std::unordered_set<u64> seen;
    for (Entity e : entities) {
        const u64 key = EntityKey(e);
        seen.insert(key);
        std::vector<u8> settings = SettingsOf(world_, e);
        auto it = bodies_.find(key);
        if (it != bodies_.end() && it->second.settings == settings) continue;
        Tracked tracked;
        if (it != bodies_.end()) {
            Destroy(it->second); // changed: rebuild at the current Transform
            bodies_.erase(it);
        }
        std::string problem;
        if (Build(e, tracked, problem)) {
            tracked.settings = std::move(settings);
            bodies_[key] = std::move(tracked);
            problems_.erase(e.index);
        } else {
            auto [p, inserted] = problems_.try_emplace(e.index, problem);
            if (inserted || p->second != problem) {
                p->second = problem;
                AETHER_LOG_WARN("Physics", "Entity %u has no physics body: %s", e.index, problem.c_str());
            }
        }
    }
    for (auto it = bodies_.begin(); it != bodies_.end();) {
        if (seen.count(it->first) == 0) {
            Destroy(it->second);
            it = bodies_.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsScene::Step(f32 dt) {
    Sync();
    JPH::BodyInterface& bodies = physics_.BodyInterface();
    for (auto& [key, tracked] : bodies_) {
        (void)key;
        const Transform* t = world_.GetComponent<Transform>(tracked.entity);
        if (t == nullptr || (Same(t->position, tracked.position) && Same(t->rotation, tracked.rotation))) continue;
        const JPH::RVec3 position(t->position.x, t->position.y, t->position.z);
        if (tracked.motion == BodyMotion::Kinematic && dt > 0.0f) {
            bodies.MoveKinematic(tracked.body, position, ToJolt(t->rotation), dt); // arrives by the end of the step
        } else {
            bodies.SetPositionAndRotation(tracked.body, position, ToJolt(t->rotation), JPH::EActivation::Activate);
        }
        tracked.position = t->position;
        tracked.rotation = t->rotation;
    }
    physics_.Step(dt);
    for (auto& [key, tracked] : bodies_) {
        (void)key;
        if (tracked.motion != BodyMotion::Dynamic) continue;
        Transform* t = world_.GetComponent<Transform>(tracked.entity);
        if (t == nullptr) continue;
        JPH::RVec3 position;
        JPH::Quat rotation;
        bodies.GetPositionAndRotation(tracked.body, position, rotation);
        t->position = Vec3(static_cast<f32>(position.GetX()), static_cast<f32>(position.GetY()), static_cast<f32>(position.GetZ()));
        t->rotation = ToAether(rotation);
        tracked.position = t->position;
        tracked.rotation = t->rotation;
    }
    Dispatch();
}

namespace {
QueryFilter FilterFor(const PhysicsScene& scene, LayerMask layers, Entity ignore, bool include_triggers) {
    QueryFilter filter;
    filter.layers = layers;
    filter.include_triggers = include_triggers;
    if (!ignore.IsNull()) {
        const JPH::BodyID body = scene.BodyOf(ignore);
        if (!body.IsInvalid()) filter.ignore.push_back(body);
    }
    return filter;
}
} // namespace

SceneHit PhysicsScene::RayCast(const Vec3& from, const Vec3& to, LayerMask layers, Entity ignore, bool include_triggers) const {
    const QueryHit hit = physics_.RayCast(from, to, FilterFor(*this, layers, ignore, include_triggers));
    return {hit.hit, hit.hit ? EntityOf(hit.body) : kNullEntity, hit.point, hit.normal, hit.distance};
}

SceneHit PhysicsScene::SphereCast(f32 radius, const Vec3& from, const Vec3& to, LayerMask layers, Entity ignore,
                                  bool include_triggers) const {
    const QueryHit hit = physics_.SphereCast(radius, from, to, FilterFor(*this, layers, ignore, include_triggers));
    return {hit.hit, hit.hit ? EntityOf(hit.body) : kNullEntity, hit.point, hit.normal, hit.distance};
}

std::vector<Entity> PhysicsScene::OverlapSphere(const Vec3& center, f32 radius, LayerMask layers, Entity ignore,
                                                bool include_triggers) const {
    std::vector<Entity> out;
    for (JPH::BodyID body : physics_.OverlapSphere(center, radius, FilterFor(*this, layers, ignore, include_triggers))) {
        const Entity e = EntityOf(body);
        if (!e.IsNull()) out.push_back(e);
    }
    return out;
}

Entity PhysicsScene::Owner(JPH::BodyID body) const {
    const u32 id = body.GetIndexAndSequenceNumber();
    if (auto it = by_body_.find(id); it != by_body_.end()) return EntityFromKey(it->second);
    if (auto it = graveyard_.find(id); it != graveyard_.end()) return EntityFromKey(it->second);
    return kNullEntity;
}

void PhysicsScene::Dispatch() {
    events_.clear();
    // Map every contact to entities first: handlers may destroy entities
    // (and a later Sync their bodies) while we deliver.
    std::vector<PhysicsEvent> pending;
    for (const ContactEvent& c : physics_.Contacts()) {
        const Entity e1 = Owner(c.body1), e2 = Owner(c.body2);
        PhysicsEventType type;
        if (c.trigger) {
            if (c.type == ContactType::Stay) continue;
            type = c.type == ContactType::Begin ? PhysicsEventType::TriggerEnter : PhysicsEventType::TriggerExit;
        } else {
            type = c.type == ContactType::Begin  ? PhysicsEventType::CollisionBegin
                   : c.type == ContactType::Stay ? PhysicsEventType::CollisionStay
                                                 : PhysicsEventType::CollisionEnd;
        }
        // The contact normal points from body 1 to body 2; each side gets it pointing at itself.
        if (!e1.IsNull()) pending.push_back({e1, e2, type, c.point, Vec3(-c.normal.x, -c.normal.y, -c.normal.z), c.approach_speed});
        if (!e2.IsNull()) pending.push_back({e2, e1, type, c.point, c.normal, c.approach_speed});
    }
    for (const PhysicsEvent& e : pending) {
        if (!world_.IsAlive(e.self)) continue; // destroyed earlier in this dispatch (or before it)
        events_.push_back(e);
        if (handler_) handler_(e);
    }
    graveyard_.clear();
}

} // namespace aether
