#include "aether/physics/physics_world.h"

#include "aether/core/log.h"

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace aether {

namespace {

// Two object layers (moving/non-moving) mapped onto two broadphase layers —
// the simplest configuration Jolt supports, adequate for a demo scene of a
// static floor plus dynamic spheres. A real game would likely add more
// layers (e.g. triggers, characters) as gameplay needs grow.
namespace ObjectLayers {
constexpr JPH::ObjectLayer kNonMoving = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kNumLayers = 2;
} // namespace ObjectLayers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
constexpr u32 kNumLayers = 2;
} // namespace BroadPhaseLayers

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer object1, JPH::ObjectLayer object2) const override {
        if (object1 == ObjectLayers::kNonMoving) {
            return object2 == ObjectLayers::kMoving;
        }
        return true;
    }
};

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BroadPhaseLayerInterfaceImpl() {
        object_to_broad_phase_[ObjectLayers::kNonMoving] = BroadPhaseLayers::kNonMoving;
        object_to_broad_phase_[ObjectLayers::kMoving] = BroadPhaseLayers::kMoving;
    }

    u32 GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kNumLayers; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return object_to_broad_phase_[layer];
    }

private:
    JPH::BroadPhaseLayer object_to_broad_phase_[ObjectLayers::kNumLayers];
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer1, JPH::BroadPhaseLayer layer2) const override {
        if (layer1 == ObjectLayers::kNonMoving) {
            return layer2 == BroadPhaseLayers::kMoving;
        }
        return true;
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
    ObjectLayerPairFilterImpl object_layer_pair_filter;
    BroadPhaseLayerInterfaceImpl broad_phase_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl object_vs_broad_phase_layer_filter;
};

PhysicsWorld::PhysicsWorld() {
    EnsureJoltInitialized();

    temp_allocator_ = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);

    u32 worker_count = std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1;
    job_system_ =
        std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, worker_count);

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

    AETHER_LOG_INFO("Physics", "Jolt PhysicsSystem initialized (%u worker threads)", worker_count);
}

PhysicsWorld::~PhysicsWorld() {
    physics_system_.reset();
    layers_.reset();
    job_system_.reset();
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

void PhysicsWorld::DestroyBody(JPH::BodyID id) {
    JPH::BodyInterface& bi = BodyInterface();
    bi.RemoveBody(id);
    bi.DestroyBody(id);
}

void PhysicsWorld::Step(f32 dt) {
    constexpr int kCollisionSteps = 1;
    physics_system_->Update(dt, kCollisionSteps, temp_allocator_.get(), job_system_.get());
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

void RegisterPhysicsComponentSerializers() {
    SetComponentSerializer<RigidBody>(
        "aether::RigidBody",
        [](const void* component, std::vector<u8>& out) {
            const auto* body = static_cast<const RigidBody*>(component);
            // Body id is deliberately NOT serialized — it's a live handle
            // into a specific PhysicsWorld's body manager and meaningless
            // once reloaded. Only the data needed to recreate the body is
            // saved; PhysicsWorld re-creates it on load (see the editor).
            AppendComponentField(out, body->radius);
            AppendComponentField(out, body->mass);
            AppendComponentField(out, body->is_static);
        },
        [](void* component, const u8* data, usize size) {
            auto* body = static_cast<RigidBody*>(component);
            usize offset = 0;
            ReadComponentField(data, size, offset, body->radius);
            ReadComponentField(data, size, offset, body->mass);
            ReadComponentField(data, size, offset, body->is_static);
            body->body_id = JPH::BodyID(); // invalid; caller must recreate the physics body
        });
}

} // namespace aether
