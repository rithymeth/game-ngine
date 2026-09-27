#include "aether/physics/physics_world.h"

#include "aether/core/log.h"
#include "aether/physics/jolt_job_system_adapter.h"
#include "aether/reflection/serialize.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <atomic>
#include <tuple>
#include <cstring>
#include <cstdarg>
#include <cstdio>

namespace aether {

namespace {

// Object layers (Phase 13 step 2): one per game layer (0..31) and motion,
// `layer * 2 + moving`. Two broadphase trees, non-moving and moving, as
// Jolt recommends. Whether two layers collide comes from the project's
// CollisionMatrix; two non-moving bodies never do.
namespace ObjectLayers {
constexpr JPH::ObjectLayer kNumLayers = kMaxLayers * 2;
constexpr JPH::ObjectLayer Make(u8 layer, bool moving) { return static_cast<JPH::ObjectLayer>(layer * 2 + (moving ? 1 : 0)); }
constexpr bool IsMoving(JPH::ObjectLayer l) { return (l & 1) != 0; }
constexpr u8 GameLayer(JPH::ObjectLayer l) { return static_cast<u8>(l / 2); }
constexpr JPH::ObjectLayer kNonMoving = Make(0, false);
constexpr JPH::ObjectLayer kMoving = Make(0, true);
} // namespace ObjectLayers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr u32 kNumLayers = 2;
} // namespace BroadPhaseLayers

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectLayerPairFilterImpl(const CollisionMatrix& matrix) : matrix_(matrix) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!ObjectLayers::IsMoving(a) && !ObjectLayers::IsMoving(b)) return false;
        return matrix_.ShouldCollide(ObjectLayers::GameLayer(a), ObjectLayers::GameLayer(b));
    }

private:
    const CollisionMatrix& matrix_;
};

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    u32 GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kNumLayers; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return ObjectLayers::IsMoving(layer) ? BroadPhaseLayers::kMoving : BroadPhaseLayers::kNonMoving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    // Jolt's profiler builds (Debug) require names.
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::kMoving ? "Moving" : "NonMoving";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase) const override {
        // Non-moving bodies only need the moving tree; the pair filter checks the matrix.
        return ObjectLayers::IsMoving(layer) || broad_phase == BroadPhaseLayers::kMoving;
    }
};

void JoltTraceImpl(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    AETHER_LOG_INFO("Jolt", "%s", buffer);
}

std::atomic<int>& JoltRefCount() {
    static std::atomic<int> count{0};
    return count;
}

// Jolt's Factory/type registry is process-global; only the first
// PhysicsWorld pays the init cost, and it's torn down only once the last one
// is destroyed, so multiple PhysicsWorld instances (e.g. in tests) are safe.
#ifdef JPH_ENABLE_ASSERTS
// Jolt's own checks (Debug builds): log what failed before breaking, so a
// failure says more than "trace/breakpoint trap".
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    AETHER_LOG_ERROR("Jolt", "%s:%u: assertion failed: %s%s%s", file, line, expression, message != nullptr ? " - " : "",
                     message != nullptr ? message : "");
    return true; // break into the debugger
}
#endif

void EnsureJoltInitialized() {
    if (JoltRefCount().fetch_add(1, std::memory_order_acq_rel) == 0) {
        JPH::Trace = JoltTraceImpl;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = JoltAssertFailed;
#endif
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void ReleaseJolt() {
    if (JoltRefCount().fetch_sub(1, std::memory_order_acq_rel) == 1) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

Vec3 ToAether(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
JPH::Vec3 ToJolt(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
Quaternion ToAether(JPH::QuatArg q) { return Quaternion(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

} // namespace

// Contacts as Jolt reports them, from worker threads during a Step.
struct PhysicsWorld::ContactQueue final : public JPH::ContactListener {
    enum Kind : u8 { Added, Persisted, Removed };
    struct Raw {
        u32 body1 = 0, body2 = 0; // BodyID index + sequence
        u32 sub1 = 0, sub2 = 0;
        Kind kind = Added;
        bool trigger = false;
        Vec3 point, normal;
        f32 approach_speed = 0.0f;
    };
    static constexpr u32 kCapacity = 16384;

    std::vector<Raw> buffer = std::vector<Raw>(kCapacity); // allocated once; slots claimed with an atomic counter
    std::atomic<u32> count{0};
    std::atomic<u64> dropped{0};
    std::vector<u8> report_stay; // by body index; only read during a Step

    void Push(const Raw& raw) {
        const u32 slot = count.fetch_add(1, std::memory_order_relaxed);
        if (slot < kCapacity) {
            buffer[slot] = raw;
        } else {
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool WantsStay(const JPH::Body& a, const JPH::Body& b) const {
        const u32 ia = a.GetID().GetIndex(), ib = b.GetID().GetIndex();
        return (ia < report_stay.size() && report_stay[ia]) || (ib < report_stay.size() && report_stay[ib]);
    }
    void Record(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, Kind kind) {
        Raw raw;
        raw.body1 = b1.GetID().GetIndexAndSequenceNumber();
        raw.body2 = b2.GetID().GetIndexAndSequenceNumber();
        raw.sub1 = m.mSubShapeID1.GetValue();
        raw.sub2 = m.mSubShapeID2.GetValue();
        raw.kind = kind;
        raw.trigger = b1.IsSensor() || b2.IsSensor();
        const JPH::RVec3 p = m.GetWorldSpaceContactPointOn1(0);
        raw.point = Vec3(static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()), static_cast<f32>(p.GetZ()));
        raw.normal = ToAether(m.mWorldSpaceNormal);
        // Jolt sorts the pair (body 1 has the lower id); the normal moves body 2 out.
        raw.approach_speed = std::max(0.0f, -(b2.GetLinearVelocity() - b1.GetLinearVelocity()).Dot(m.mWorldSpaceNormal));
        Push(raw);
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        Record(b1, b2, m, Added);
    }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                            JPH::ContactSettings&) override {
        if (!b1.IsSensor() && !b2.IsSensor() && WantsStay(b1, b2)) Record(b1, b2, m, Persisted);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        Raw raw;
        raw.body1 = pair.GetBody1ID().GetIndexAndSequenceNumber();
        raw.body2 = pair.GetBody2ID().GetIndexAndSequenceNumber();
        raw.sub1 = pair.GetSubShapeID1().GetValue();
        raw.sub2 = pair.GetSubShapeID2().GetValue();
        raw.kind = Removed;
        Push(raw);
    }
};

struct PhysicsWorld::Layers {
    CollisionMatrix matrix; // read by the pair filter; SetCollisionMatrix changes it
    ObjectLayerPairFilterImpl object_layer_pair_filter{matrix};
    BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
};

PhysicsWorld::PhysicsWorld(JobSystem& job_system) {
    EnsureJoltInitialized();

    temp_allocator_ = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    jolt_job_system_ =
        std::make_unique<JoltJobSystemAdapter>(job_system, JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers);

    layers_ = std::make_unique<Layers>();

    constexpr JPH::uint kMaxBodies = 4096;
    constexpr JPH::uint kNumBodyMutexes = 0; // 0 = default
    constexpr JPH::uint kMaxBodyPairs = 4096;
    constexpr JPH::uint kMaxContactConstraints = 2048;

    physics_system_ = std::make_unique<JPH::PhysicsSystem>();
    physics_system_->Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints,
                           layers_->broad_phase_layer_interface, layers_->object_vs_broad_phase_layer_filter,
                           layers_->object_layer_pair_filter);
    physics_system_->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    contact_queue_ = std::make_unique<ContactQueue>();
    contact_queue_->report_stay.assign(kMaxBodies, 0);
    physics_system_->SetContactListener(contact_queue_.get());

    AETHER_LOG_INFO("Physics", "Jolt PhysicsSystem initialized (routed through aether::JobSystem, %d max concurrency)",
                     jolt_job_system_->GetMaxConcurrency());
}

PhysicsWorld::~PhysicsWorld() {
    physics_system_.reset();
    contact_queue_.reset();
    layers_.reset();
    jolt_job_system_.reset();
    temp_allocator_.reset();
    ReleaseJolt();
}

void PhysicsWorld::SetGravity(const Vec3& gravity) {
    physics_system_->SetGravity(ToJolt(gravity));
}

JPH::BodyInterface& PhysicsWorld::BodyInterface() {
    return physics_system_->GetBodyInterface();
}

JPH::BodyID PhysicsWorld::CreateBox(const Vec3& position, const Vec3& half_extents) {
    JPH::BoxShapeSettings shape_settings(ToJolt(half_extents));
    JPH::ShapeSettings::ShapeResult shape_result = shape_settings.Create();
    AETHER_ASSERT(!shape_result.HasError());

    JPH::BodyCreationSettings body_settings(shape_result.Get(), JPH::RVec3(position.x, position.y, position.z),
                                             JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                             ObjectLayers::kNonMoving);
    return BodyInterface().CreateAndAddBody(body_settings, JPH::EActivation::DontActivate);
}

JPH::BodyID PhysicsWorld::CreateSphere(const Vec3& position, f32 radius, f32 mass, bool is_static) {
    JPH::BodyCreationSettings body_settings(
        new JPH::SphereShape(radius), JPH::RVec3(position.x, position.y, position.z), JPH::Quat::sIdentity(),
        is_static ? JPH::EMotionType::Static : JPH::EMotionType::Dynamic,
        is_static ? ObjectLayers::kNonMoving : ObjectLayers::kMoving);

    if (!is_static) {
        body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        body_settings.mMassPropertiesOverride.mMass = mass;
    }

    return BodyInterface().CreateAndAddBody(body_settings,
                                             is_static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
}

JPH::ObjectLayer PhysicsWorld::ObjectLayerFor(bool moving, u8 layer) const {
    return ObjectLayers::Make(layer < kMaxLayers ? layer : 0, moving);
}

u8 PhysicsWorld::GameLayerOf(JPH::ObjectLayer layer) { return ObjectLayers::GameLayer(layer); }

void PhysicsWorld::SetCollisionMatrix(const CollisionMatrix& matrix) {
    layers_->matrix = matrix; // applies to pairs the broadphase finds from now on
}

const CollisionMatrix& PhysicsWorld::GetCollisionMatrix() const { return layers_->matrix; }

JPH::BodyID PhysicsWorld::CreateBody(const JPH::BodyCreationSettings& settings, bool activate) {
    return BodyInterface().CreateAndAddBody(settings, activate ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
}

void PhysicsWorld::DestroyBody(JPH::BodyID id) {
    JPH::BodyInterface& bi = BodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
}

void PhysicsWorld::Step(f32 dt) {
    constexpr int kCollisionSteps = 1;
    contact_queue_->count.store(0, std::memory_order_relaxed);
    physics_system_->Update(dt, kCollisionSteps, temp_allocator_.get(), jolt_job_system_.get());
    ProcessContacts();
}

namespace {

class LayerMaskFilter final : public JPH::ObjectLayerFilter {
public:
    explicit LayerMaskFilter(LayerMask mask) : mask_(mask) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override { return (mask_ & (1u << ObjectLayers::GameLayer(layer))) != 0; }

private:
    LayerMask mask_;
};

class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& filter) : filter_(filter) {}
    bool ShouldCollide(const JPH::BodyID& body) const override {
        return std::find(filter_.ignore.begin(), filter_.ignore.end(), body) == filter_.ignore.end();
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override { return filter_.include_triggers || !body.IsSensor(); }

private:
    const QueryFilter& filter_;
};

Vec3 FromR(JPH::RVec3Arg v) { return Vec3(static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ())); }
JPH::RVec3 ToR(const Vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
JPH::Quat ToJoltQ(const Quaternion& q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }

} // namespace

QueryHit PhysicsWorld::RayCast(const Vec3& from, const Vec3& to, const QueryFilter& filter) const {
    QueryHit out;
    const JPH::RRayCast ray(ToR(from), ToJolt(to - from));
    JPH::RayCastResult result;
    const LayerMaskFilter layers(filter.layers);
    const QueryBodyFilter bodies(filter);
    if (!physics_system_->GetNarrowPhaseQuery().CastRay(ray, result, {}, layers, bodies)) return out;
    out.hit = true;
    out.body = result.mBodyID;
    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    out.point = FromR(point);
    out.distance = (to - from).Length() * result.mFraction;
    JPH::BodyLockRead lock(physics_system_->GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) out.normal = ToAether(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    return out;
}

QueryHit PhysicsWorld::ShapeCast(const JPH::Shape& shape, const Vec3& from, const Vec3& to, const Quaternion& rotation,
                                 const QueryFilter& filter) const {
    QueryHit out;
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        &shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(ToJoltQ(rotation), ToR(from)), ToJolt(to - from));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const LayerMaskFilter layers(filter.layers);
    const QueryBodyFilter bodies(filter);
    physics_system_->GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, layers, bodies);
    if (!collector.HadHit()) return out;
    const JPH::ShapeCastResult& hit = collector.mHit;
    out.hit = true;
    out.body = hit.mBodyID2;
    out.point = ToAether(hit.mContactPointOn2);
    const JPH::Vec3 axis = hit.mPenetrationAxis;
    out.normal = axis.LengthSq() > 1e-12f ? ToAether(-axis.Normalized()) : Vec3(0, 0, 0);
    out.distance = (to - from).Length() * std::max(0.0f, hit.mFraction);
    return out;
}

QueryHit PhysicsWorld::SphereCast(f32 radius, const Vec3& from, const Vec3& to, const QueryFilter& filter) const {
    const JPH::SphereShape sphere(std::max(radius, 0.001f));
    sphere.SetEmbedded(); // on the stack: never reference-counted away
    return ShapeCast(sphere, from, to, Quaternion::Identity(), filter);
}

std::vector<JPH::BodyID> PhysicsWorld::Overlap(const JPH::Shape& shape, const Vec3& position, const Quaternion& rotation,
                                               const QueryFilter& filter) const {
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const LayerMaskFilter layers(filter.layers);
    const QueryBodyFilter bodies(filter);
    physics_system_->GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sReplicate(1.0f),
                                                        JPH::RMat44::sRotationTranslation(ToJoltQ(rotation), ToR(position)), settings,
                                                        JPH::RVec3::sZero(), collector, {}, layers, bodies);
    std::vector<JPH::BodyID> out;
    for (const JPH::CollideShapeResult& hit : collector.mHits) out.push_back(hit.mBodyID2);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<JPH::BodyID> PhysicsWorld::OverlapSphere(const Vec3& center, f32 radius, const QueryFilter& filter) const {
    const JPH::SphereShape sphere(std::max(radius, 0.001f));
    sphere.SetEmbedded();
    return Overlap(sphere, center, Quaternion::Identity(), filter);
}

void PhysicsWorld::SetReportStay(JPH::BodyID body, bool report) {
    if (!body.IsInvalid() && body.GetIndex() < contact_queue_->report_stay.size()) {
        contact_queue_->report_stay[body.GetIndex()] = report ? 1 : 0;
    }
}

u64 PhysicsWorld::DroppedContacts() const { return contact_queue_->dropped.load(std::memory_order_relaxed); }

void PhysicsWorld::ProcessContacts() {
    ++step_;
    contacts_.clear();
    ContactQueue& queue = *contact_queue_;
    const u32 filled = std::min(queue.count.load(std::memory_order_relaxed), ContactQueue::kCapacity);
    if (queue.count.load(std::memory_order_relaxed) > ContactQueue::kCapacity) {
        AETHER_LOG_WARN("Physics", "%u contact callbacks didn't fit the contact buffer this step (capacity %u); events were lost",
                        queue.count.load(std::memory_order_relaxed) - ContactQueue::kCapacity, ContactQueue::kCapacity);
    }
    // Worker threads fill the buffer in any order: sort for a deterministic one.
    std::sort(queue.buffer.begin(), queue.buffer.begin() + filled, [](const ContactQueue::Raw& a, const ContactQueue::Raw& b) {
        return std::tie(a.body1, a.body2, a.sub1, a.sub2, a.kind) < std::tie(b.body1, b.body2, b.sub1, b.sub2, b.kind);
    });
    auto key_of = [](u32 b1, u32 b2) { return (static_cast<u64>(b1) << 32) | b2; };
    auto event = [](u32 b1, u32 b2, ContactType type, bool trigger) {
        ContactEvent e;
        e.body1 = JPH::BodyID(b1);
        e.body2 = JPH::BodyID(b2);
        e.type = type;
        e.trigger = trigger;
        return e;
    };
    std::vector<u64> check; // pairs that lost a sub-shape contact this step
    for (u32 i = 0; i < filled; ++i) {
        const ContactQueue::Raw& raw = queue.buffer[i];
        const u64 key = key_of(raw.body1, raw.body2);
        PairState& pair = pairs_[key];
        switch (raw.kind) {
        case ContactQueue::Added:
            if (pair.touching == 0 && !pair.dormant) {
                ContactEvent e = event(raw.body1, raw.body2, ContactType::Begin, raw.trigger);
                e.point = raw.point;
                e.normal = raw.normal;
                e.approach_speed = raw.approach_speed;
                contacts_.push_back(e);
            }
            ++pair.touching;
            pair.dormant = false;
            pair.trigger = raw.trigger;
            break;
        case ContactQueue::Persisted:
            if (pair.touching > 0 && !pair.trigger && pair.stay_step != step_) {
                pair.stay_step = step_; // once per pair per step
                ContactEvent e = event(raw.body1, raw.body2, ContactType::Stay, false);
                e.point = raw.point;
                e.normal = raw.normal;
                contacts_.push_back(e);
            }
            break;
        case ContactQueue::Removed:
            if (pair.touching > 0) --pair.touching;
            if (pair.touching == 0) check.push_back(key);
            break;
        }
    }
    // A pair with no contacts left ends, unless both bodies just fell asleep
    // (Jolt drops sleeping contacts; they come back as Added on waking).
    // Dormant pairs end once a body wakes without touching, or goes away.
    std::sort(check.begin(), check.end());
    JPH::BodyInterface& bodies = physics_system_->GetBodyInterface();
    for (auto it = pairs_.begin(); it != pairs_.end();) {
        PairState& pair = it->second;
        const bool candidate =
            pair.touching == 0 && (pair.dormant || std::binary_search(check.begin(), check.end(), it->first));
        if (!candidate) {
            if (pair.touching == 0 && !pair.dormant) {
                it = pairs_.erase(it); // e.g. a Removed for a pair we never saw begin
                continue;
            }
            ++it;
            continue;
        }
        const JPH::BodyID b1(static_cast<u32>(it->first >> 32)), b2(static_cast<u32>(it->first));
        const bool exist = bodies.IsAdded(b1) && bodies.IsAdded(b2);
        if (exist && !bodies.IsActive(b1) && !bodies.IsActive(b2)) {
            pair.dormant = true;
            ++it;
            continue;
        }
        contacts_.push_back(event(b1.GetIndexAndSequenceNumber(), b2.GetIndexAndSequenceNumber(), ContactType::End, pair.trigger));
        it = pairs_.erase(it);
    }
}

Vec3 PhysicsWorld::GetPosition(JPH::BodyID id) const {
    return ToAether(const_cast<PhysicsWorld*>(this)->BodyInterface().GetPosition(id));
}

Quaternion PhysicsWorld::GetRotation(JPH::BodyID id) const {
    return ToAether(const_cast<PhysicsWorld*>(this)->BodyInterface().GetRotation(id));
}

void PhysicsWorld::SetPosition(JPH::BodyID id, const Vec3& position) {
    BodyInterface().SetPosition(id, ToJolt(position), JPH::EActivation::Activate);
}

void SyncPhysicsToTransforms(World& world, PhysicsWorld& physics, f32 dt) {
    physics.Step(dt);
    world.ForEach<RigidBody, Transform>([&](RigidBody& body, Transform& transform) {
        transform.position = physics.GetPosition(body.body_id);
        transform.rotation = physics.GetRotation(body.body_id);
    });
}

namespace {

// Binary RigidBody payloads. Before the collider split (Phase 13 step 1) it
// was exactly radius (f32), mass (f32), is_static (bool). Now it's a magic
// tag, then the reflected binary form, so later fields need no new format.
constexpr u32 kRigidBodyMagic = 0x32304252; // "RB02"
constexpr usize kLegacyRigidBodySize = sizeof(f32) + sizeof(f32) + sizeof(bool);

// JSON: version 1 had {radius, mass, is_static}.
void MigrateRigidBodyJson(u16 from_version, reflect::Json& data) {
    if (from_version >= 2) return;
    if (data.contains("is_static")) {
        data["motion"] = data["is_static"].get<bool>() ? "Static" : "Dynamic";
        data.erase("is_static");
    }
    if (data.contains("radius")) {
        data["legacy_radius"] = data["radius"];
        data.erase("radius");
    }
}

} // namespace

void RegisterPhysicsComponentSerializers() {
    static const bool migration = (reflect::RegisterMigration<RigidBody>(MigrateRigidBodyJson), true);
    (void)migration;
    SetComponentSerializer<RigidBody>(
        "aether::RigidBody",
        [](const void* component, std::vector<u8>& out) {
            // The body id is deliberately not serialized: it's a live handle
            // into one PhysicsWorld, meaningless once reloaded.
            AppendComponentField(out, kRigidBodyMagic);
            const std::vector<u8> body = reflect::SaveBinary(*static_cast<const RigidBody*>(component));
            out.insert(out.end(), body.begin(), body.end());
        },
        [](void* component, const u8* data, usize size) {
            auto* body = static_cast<RigidBody*>(component);
            *body = RigidBody{};
            usize offset = 0;
            u32 magic = 0;
            if (size >= sizeof(u32)) std::memcpy(&magic, data, sizeof(u32));
            if (magic == kRigidBodyMagic) {
                reflect::LoadBinary(*body, std::span<const u8>(data + sizeof(u32), size - sizeof(u32)));
            } else if (size == kLegacyRigidBodySize) {
                f32 radius = 0.0f;
                bool is_static = false;
                ReadComponentField(data, size, offset, radius);
                ReadComponentField(data, size, offset, body->mass);
                ReadComponentField(data, size, offset, is_static);
                body->legacy_radius = radius;
                body->motion = is_static ? BodyMotion::Static : BodyMotion::Dynamic;
            } else {
                AETHER_LOG_WARN("Physics", "RigidBody data of an unknown format (%zu bytes); using defaults", size);
            }
            body->body_id = JPH::BodyID(); // invalid; the caller recreates the physics body
        });
}

usize MigrateLegacyRigidBodies(World& world) {
    std::vector<Entity> entities;
    world.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<RigidBody>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    usize migrated = 0;
    for (Entity e : entities) {
        RigidBody* body = world.GetComponent<RigidBody>(e);
        if (body == nullptr || body->legacy_radius <= 0.0f) continue;
        const f32 radius = body->legacy_radius;
        body->legacy_radius = 0.0f;
        if (HasCollider(world, e)) continue;
        SphereCollider sphere;
        sphere.radius = radius;
        world.AddComponent(e, sphere);
        ++migrated;
    }
    return migrated;
}

bool HasCollider(const World& world, Entity entity) {
    return world.HasComponent<BoxCollider>(entity) || world.HasComponent<SphereCollider>(entity) ||
           world.HasComponent<CapsuleCollider>(entity) || world.HasComponent<ConvexCollider>(entity) ||
           world.HasComponent<MeshCollider>(entity);
}

} // namespace aether
