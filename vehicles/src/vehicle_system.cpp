#include "aether/vehicles/vehicle_system.h"

#include "aether/physics/components.h"
#include "aether/scene/components.h"

#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace aether::vehicles {

namespace {

VehicleSystem* g_active = nullptr;

constexpr f32 kPi = 3.14159265358979f;

u64 Key(Entity e) { return (static_cast<u64>(e.generation) << 32) | e.index; }
bool Finite(f32 v) { return std::isfinite(v); }
f32 Clamp(f32 v, f32 lo, f32 hi) { return std::clamp(v, lo, hi); }

// Everything the built constraint depends on: when any of it differs, the vehicle is built again.
std::vector<f32> Signature(const Vehicle& v, const RigidBody& rb, const BoxCollider& box, const Transform& t) {
    return {static_cast<f32>(v.drive), v.wheel_radius, v.wheel_width, v.track_fraction, v.wheelbase_fraction, v.ground_clearance, v.suspension_min,
            v.suspension_max, v.suspension_frequency, v.suspension_damping, v.engine_torque, v.max_rpm, v.max_steer_degrees, v.brake_torque, v.handbrake_torque,
            v.grip, rb.mass, rb.linear_damping, rb.angular_damping, rb.gravity_scale, static_cast<f32>(rb.motion), box.half_extents.x, box.half_extents.y,
            box.half_extents.z, box.center.x, box.center.y, box.center.z, t.scale.x, t.scale.y, t.scale.z};
}

JPH::Vec3 J(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }

} // namespace

struct VehicleSystem::Impl {
    struct Record {
        Entity entity;
        JPH::BodyID body;
        JPH::Ref<JPH::VehicleConstraint> constraint;
        std::vector<f32> signature;
    };
    std::unordered_map<u64, Record> records;
};

VehicleSystem::VehicleSystem(World& world, PhysicsScene& scene) : impl_(std::make_unique<Impl>()), world_(world), scene_(scene) { MakeActive(); }

VehicleSystem::~VehicleSystem() {
    JPH::PhysicsSystem& system = scene_.Physics().System();
    for (auto& [key, r] : impl_->records) {
        (void)key;
        system.RemoveStepListener(r.constraint);
        system.RemoveConstraint(r.constraint);
    }
    if (g_active == this) g_active = nullptr;
}

VehicleSystem* VehicleSystem::Active() { return g_active; }
void VehicleSystem::MakeActive() { g_active = this; }

usize VehicleSystem::VehicleCount() const { return impl_->records.size(); }

usize VehicleSystem::Apply() {
    JPH::PhysicsSystem& system = scene_.Physics().System();
    JPH::BodyInterface& bodies = scene_.Physics().BodyInterface();
    const auto release = [&](Impl::Record& r) {
        system.RemoveStepListener(r.constraint);
        system.RemoveConstraint(r.constraint);
    };

    // 1. Take apart what no longer holds, while its body still exists.
    for (auto it = impl_->records.begin(); it != impl_->records.end();) {
        Impl::Record& r = it->second;
        bool keep = world_.IsAlive(r.entity);
        if (keep) {
            const Vehicle* v = world_.GetComponent<Vehicle>(r.entity);
            const RigidBody* rb = world_.GetComponent<RigidBody>(r.entity);
            const BoxCollider* box = world_.GetComponent<BoxCollider>(r.entity);
            const Transform* t = world_.GetComponent<Transform>(r.entity);
            keep = v != nullptr && rb != nullptr && box != nullptr && t != nullptr && v->enabled && Signature(*v, *rb, *box, *t) == r.signature;
        }
        if (!keep) {
            release(r);
            it = impl_->records.erase(it);
        } else {
            ++it;
        }
    }

    // 2. Make the bodies of any new entities.
    scene_.Sync();

    // 3. Build the vehicles that have none yet, in entity order.
    std::vector<Entity> candidates;
    const ComponentId vehicle_id = GetComponentId<Vehicle>();
    world_.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(vehicle_id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            candidates.insert(candidates.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    std::sort(candidates.begin(), candidates.end(), [](Entity x, Entity y) { return x.index != y.index ? x.index < y.index : x.generation < y.generation; });
    for (const Entity e : candidates) {
        if (impl_->records.count(Key(e)) != 0 || !world_.IsAlive(e)) continue;
        const Vehicle* vp = world_.GetComponent<Vehicle>(e);
        const RigidBody* rb = world_.GetComponent<RigidBody>(e);
        const BoxCollider* boxp = world_.GetComponent<BoxCollider>(e);
        const Transform* tp = world_.GetComponent<Transform>(e);
        if (vp == nullptr || !vp->enabled || rb == nullptr || boxp == nullptr || tp == nullptr || rb->motion != BodyMotion::Dynamic) continue;
        const JPH::BodyID id = scene_.BodyOf(e);
        if (id.IsInvalid()) continue;
        const Vehicle v = *vp;
        const BoxCollider box = *boxp;
        const Transform t = *tp;
        const std::vector<f32> signature = Signature(v, *rb, box, t);
        // Every vehicle has its input and its state beside it (adding them moves the entity's data, so the pointers above are not used after this).
        if (!world_.HasComponent<VehicleInput>(e)) world_.AddComponent<VehicleInput>(e);
        if (!world_.HasComponent<VehicleState>(e)) world_.AddComponent<VehicleState>(e);

        JPH::BodyLockWrite lock(system.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) continue;
        JPH::Body& body = lock.GetBody();
        const JPH::Vec3 com = body.GetShape()->GetCenterOfMass();

        const f32 half_x = std::fabs(box.half_extents.x * t.scale.x), half_y = std::fabs(box.half_extents.y * t.scale.y), half_z = std::fabs(box.half_extents.z * t.scale.z);
        const f32 radius = Clamp(v.wheel_radius, 0.05f, 5.0f);
        const f32 min_len = Clamp(v.suspension_min, 0.01f, 3.0f);
        const f32 max_len = std::max(Clamp(v.suspension_max, 0.02f, 3.0f), min_len + 0.01f);
        const f32 mid = 0.5f * (min_len + max_len);
        const f32 attach_y = box.center.y * t.scale.y - half_y - v.ground_clearance + mid + radius; // so the wheels' bottoms are on flat ground at rest
        const f32 x = half_x * Clamp(v.track_fraction, 0.1f, 1.5f), z = half_z * Clamp(v.wheelbase_fraction, 0.1f, 1.5f);
        const f32 cx = box.center.x * t.scale.x, cz = box.center.z * t.scale.z;
        const f32 grip = Clamp(v.grip, 0.05f, 5.0f);

        JPH::VehicleConstraintSettings settings;
        settings.mMaxPitchRollAngle = kPi; // never limit it: the player decides what upside down means
        const Vec3 corners[kWheelCount] = {Vec3(cx - x, attach_y, cz + z), Vec3(cx + x, attach_y, cz + z), Vec3(cx - x, attach_y, cz - z), Vec3(cx + x, attach_y, cz - z)};
        for (u32 i = 0; i < kWheelCount; ++i) {
            auto* wheel = new JPH::WheelSettingsWV;
            wheel->mPosition = J(corners[i]) - com; // Jolt places wheels from the body's centre of mass
            wheel->mSuspensionDirection = JPH::Vec3(0, -1, 0);
            wheel->mSuspensionMinLength = min_len;
            wheel->mSuspensionMaxLength = max_len;
            wheel->mSuspensionSpring = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, Clamp(v.suspension_frequency, 0.1f, 20.0f), Clamp(v.suspension_damping, 0.0f, 5.0f));
            wheel->mRadius = radius;
            wheel->mWidth = Clamp(v.wheel_width, 0.02f, 2.0f);
            wheel->mMaxSteerAngle = i < 2 ? Clamp(v.max_steer_degrees, 0.0f, 80.0f) * kPi / 180.0f : 0.0f;
            wheel->mMaxBrakeTorque = std::max(v.brake_torque, 0.0f);
            wheel->mMaxHandBrakeTorque = i >= 2 ? std::max(v.handbrake_torque, 0.0f) : 0.0f;
            wheel->mLongitudinalFriction.Clear();
            wheel->mLongitudinalFriction.AddPoint(0.0f, 0.0f);
            wheel->mLongitudinalFriction.AddPoint(0.06f, 1.2f * grip);
            wheel->mLongitudinalFriction.AddPoint(0.2f, 1.0f * grip);
            wheel->mLateralFriction.Clear();
            wheel->mLateralFriction.AddPoint(0.0f, 0.0f);
            wheel->mLateralFriction.AddPoint(3.0f, 1.2f * grip);
            wheel->mLateralFriction.AddPoint(20.0f, 1.0f * grip);
            settings.mWheels.push_back(wheel);
        }
        auto* controller = new JPH::WheeledVehicleControllerSettings;
        controller->mEngine.mMaxTorque = Clamp(v.engine_torque, 1.0f, 100000.0f);
        controller->mEngine.mMaxRPM = Clamp(v.max_rpm, 1500.0f, 20000.0f);
        controller->mEngine.mMinRPM = std::min(controller->mEngine.mMinRPM, controller->mEngine.mMaxRPM * 0.5f);
        controller->mTransmission.mShiftUpRPM = controller->mEngine.mMaxRPM * 0.7f;
        controller->mTransmission.mShiftDownRPM = controller->mEngine.mMaxRPM * 0.35f;
        const bool front = v.drive != DriveType::RearWheel, rear = v.drive != DriveType::FrontWheel;
        controller->mDifferentials.resize(front && rear ? 2 : 1);
        const auto set_axle = [&](usize d, int left, int right, f32 share) {
            controller->mDifferentials[d].mLeftWheel = left;
            controller->mDifferentials[d].mRightWheel = right;
            controller->mDifferentials[d].mEngineTorqueRatio = share;
        };
        if (front && rear) {
            set_axle(0, 0, 1, 0.5f);
            set_axle(1, 2, 3, 0.5f);
        } else if (front) {
            set_axle(0, 0, 1, 1.0f);
        } else {
            set_axle(0, 2, 3, 1.0f);
        }
        settings.mController = controller;

        Impl::Record record;
        record.entity = e;
        record.body = id;
        record.signature = signature;
        record.constraint = new JPH::VehicleConstraint(body, settings);
        // Reference counted: the constraint keeps it alive.
        record.constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(scene_.Physics().ObjectLayerFor(true, 0)));
        system.AddConstraint(record.constraint);
        system.AddStepListener(record.constraint);
        impl_->records.emplace(Key(e), std::move(record));
    }

    // 4. Drive them and report how they went.
    for (auto& [key, r] : impl_->records) {
        (void)key;
        const VehicleInput* in = world_.GetComponent<VehicleInput>(r.entity);
        VehicleState* state = world_.GetComponent<VehicleState>(r.entity);
        auto* controller = static_cast<JPH::WheeledVehicleController*>(r.constraint->GetController());
        f32 throttle = 0, steer = 0, brake = 0, hand = 0;
        if (in != nullptr) {
            throttle = Finite(in->throttle) ? Clamp(in->throttle, -1.0f, 1.0f) : 0.0f;
            steer = Finite(in->steer) ? Clamp(in->steer, -1.0f, 1.0f) : 0.0f;
            brake = Finite(in->brake) ? Clamp(in->brake, 0.0f, 1.0f) : 0.0f;
            hand = Finite(in->handbrake) ? Clamp(in->handbrake, 0.0f, 1.0f) : 0.0f;
        }
        controller->SetDriverInput(throttle, steer, brake, hand);
        if (throttle != 0.0f || steer != 0.0f || brake != 0.0f || hand != 0.0f) bodies.ActivateBody(r.body);
        if (state != nullptr) {
            const JPH::Vec3 forward = bodies.GetRotation(r.body) * r.constraint->GetLocalForward();
            const JPH::Vec3 v = bodies.GetLinearVelocity(r.body);
            state->speed = v.Dot(forward);
            state->rpm = controller->GetEngine().GetCurrentRPM();
            state->gear = controller->GetTransmission().GetCurrentGear();
            state->steer_angle = r.constraint->GetWheel(0)->GetSteerAngle();
            u32 touching = 0;
            for (u32 i = 0; i < kWheelCount; ++i) touching += r.constraint->GetWheel(i)->HasContact() ? 1u : 0u;
            state->wheels_on_ground = touching;
            state->airborne = touching == 0;
        }
    }
    return impl_->records.size();
}

bool VehicleSystem::Drive(Entity entity, f32 throttle, f32 steer, f32 brake, f32 handbrake) {
    if (!world_.IsAlive(entity) || !world_.HasComponent<Vehicle>(entity)) return false;
    if (!Finite(throttle) || !Finite(steer) || !Finite(brake) || !Finite(handbrake)) return false;
    if (!world_.HasComponent<VehicleInput>(entity)) world_.AddComponent<VehicleInput>(entity);
    VehicleInput* in = world_.GetComponent<VehicleInput>(entity);
    in->throttle = Clamp(throttle, -1.0f, 1.0f);
    in->steer = Clamp(steer, -1.0f, 1.0f);
    in->brake = Clamp(brake, 0.0f, 1.0f);
    in->handbrake = Clamp(handbrake, 0.0f, 1.0f);
    return true;
}

bool VehicleSystem::Wheel(Entity entity, u32 index, WheelInfo& out) const {
    if (index >= kWheelCount) return false;
    const auto it = impl_->records.find(Key(entity));
    if (it == impl_->records.end() || !world_.IsAlive(entity)) return false;
    const JPH::VehicleConstraint& c = *it->second.constraint;
    const JPH::RMat44 m = c.GetWheelWorldTransform(index, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
    const JPH::RVec3 p = m.GetTranslation();
    const JPH::Quat q = m.GetQuaternion();
    const JPH::Wheel* wheel = c.GetWheel(index);
    out.position = Vec3(static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()), static_cast<f32>(p.GetZ()));
    out.rotation = Quaternion(q.GetX(), q.GetY(), q.GetZ(), q.GetW());
    out.steer_angle = wheel->GetSteerAngle();
    out.spin_angle = wheel->GetRotationAngle();
    out.suspension = wheel->GetSuspensionLength();
    out.contact = wheel->HasContact();
    return true;
}

const VehicleState* VehicleSystem::StateOf(Entity entity) const {
    if (!world_.IsAlive(entity) || impl_->records.count(Key(entity)) == 0) return nullptr;
    return world_.GetComponent<VehicleState>(entity);
}

bool VehicleSystem::Flip(Entity entity) {
    const auto it = impl_->records.find(Key(entity));
    if (it == impl_->records.end() || !world_.IsAlive(entity)) return false;
    JPH::BodyInterface& bodies = scene_.Physics().BodyInterface();
    const JPH::BodyID id = it->second.body;
    const JPH::Vec3 forward = bodies.GetRotation(id) * it->second.constraint->GetLocalForward();
    JPH::Vec3 flat(forward.GetX(), 0.0f, forward.GetZ());
    if (flat.LengthSq() < 1e-6f) flat = JPH::Vec3(0, 0, 1); // nose straight up or down: any heading will do
    flat = flat.Normalized();
    const f32 yaw = std::atan2(flat.GetX(), flat.GetZ());
    const JPH::Quat upright = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw);
    bodies.SetRotation(id, upright, JPH::EActivation::Activate);
    bodies.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
    if (Transform* t = world_.GetComponent<Transform>(entity)) {
        t->rotation = Quaternion(upright.GetX(), upright.GetY(), upright.GetZ(), upright.GetW()); // so the scene doesn't take the next Step for a teleport back
    }
    return true;
}

} // namespace aether::vehicles
