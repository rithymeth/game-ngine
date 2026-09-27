#include "aether/physics/physics_world.h"

#include "aether/core/log.h"
#include "aether/physics/jolt_job_system_adapter.h"
#include "aether/reflection/serialize.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <atomic>
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
void EnsureJoltInitialized() {
    if (JoltRefCount().fetch_add(1, std::memory_order_acq_rel) == 0) {
        JPH::Trace = JoltTraceImpl;
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

    AETHER_LOG_INFO("Physics", "Jolt PhysicsSystem initialized (routed through aether::JobSystem, %d max concurrency)",
                     jolt_job_system_->GetMaxConcurrency());
}

PhysicsWorld::~PhysicsWorld() {
    physics_system_.reset();
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
    physics_system_->Update(dt, kCollisionSteps, temp_allocator_.get(), jolt_job_system_.get());
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
