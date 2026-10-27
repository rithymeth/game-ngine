// Physics constraints (docs/design/PHYSICS_CONSTRAINTS.md): PhysicsJoint
// lifetime, runtime grabs, break detection and network snapshots.
#include "aether/physics/constraints.h"
#include "aether/physics/physics_scene.h"

#include "aether/reflection/serialize.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace aether {

namespace {

constexpr f32 kDegToRad = 3.14159265358979323846f / 180.0f;
constexpr f32 kRadToDeg = 180.0f / 3.14159265358979323846f;

JPH::Vec3 J(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::RVec3 JR(const Vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
Vec3 A(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
Vec3 AR(JPH::RVec3Arg v) {
    return Vec3(static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ()));
}

// A body's point in world space (`local` is in the body's space; an invalid
// body is the world, so `local` is already world space).
Vec3 WorldPoint(JPH::BodyInterface& bodies, JPH::BodyID body, const Vec3& local) {
    if (body.IsInvalid()) return local;
    JPH::RVec3 position;
    JPH::Quat rotation;
    bodies.GetPositionAndRotation(body, position, rotation);
    return AR(position + rotation * J(local));
}

// The PhysicsJoint without its live fields: a change here rebuilds the joint.
std::vector<u8> JointSettingsOf(const PhysicsJoint& joint) {
    PhysicsJoint copy = joint;
    copy.enabled = true;
    copy.locked = false;
    copy.broken = false;
    copy.motor_target = 0.0f;
    copy.angle = 0.0f;
    return reflect::SaveBinary(copy);
}

void SetMotorTarget(JPH::TwoBodyConstraint* constraint, const PhysicsJoint& joint) {
    if (joint.type != JointType::Hinge || joint.motor == JointMotor::None || constraint == nullptr) return;
    auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
    if (joint.motor == JointMotor::Velocity) {
        hinge->SetTargetAngularVelocity(joint.motor_target * kDegToRad);
    } else {
        hinge->SetTargetAngle(joint.motor_target * kDegToRad);
    }
}

// The force (N) and torque (N m) a constraint carried in the last step.
void LoadOf(const JPH::TwoBodyConstraint& c, f32 dt, f32& force, f32& torque) {
    force = torque = 0.0f;
    if (dt <= 0.0f) return;
    switch (c.GetSubType()) {
    case JPH::EConstraintSubType::Fixed: {
        const auto& f = static_cast<const JPH::FixedConstraint&>(c);
        force = f.GetTotalLambdaPosition().Length();
        torque = f.GetTotalLambdaRotation().Length();
        break;
    }
    case JPH::EConstraintSubType::Point:
        force = static_cast<const JPH::PointConstraint&>(c).GetTotalLambdaPosition().Length();
        break;
    case JPH::EConstraintSubType::Hinge: {
        const auto& h = static_cast<const JPH::HingeConstraint&>(c);
        force = h.GetTotalLambdaPosition().Length();
        const JPH::Vector<2> r = h.GetTotalLambdaRotation();
        torque = std::sqrt(r[0] * r[0] + r[1] * r[1]) + std::abs(h.GetTotalLambdaRotationLimits());
        break;
    }
    case JPH::EConstraintSubType::Distance:
        force = std::abs(static_cast<const JPH::DistanceConstraint&>(c).GetTotalLambdaPosition());
        break;
    case JPH::EConstraintSubType::SixDOF: {
        const auto& s = static_cast<const JPH::SixDOFConstraint&>(c);
        force = (s.GetTotalLambdaPosition() + s.GetTotalLambdaMotorTranslation()).Length();
        torque = (s.GetTotalLambdaRotation() + s.GetTotalLambdaMotorRotation()).Length();
        break;
    }
    default: break;
    }
    force /= dt;
    torque /= dt;
}

} // namespace

// --- Joints -----------------------------------------------------------------

Entity PhysicsScene::FindByGuid(const EntityGuid& guid) const {
    auto it = guids_.find(guid);
    if (it == guids_.end() || !world_.IsAlive(it->second)) return kNullEntity;
    return it->second;
}

usize PhysicsScene::JointCount() const {
    usize n = 0;
    for (const auto& [index, joint] : joints_) {
        (void)index;
        if (joint.constraint != nullptr) ++n;
    }
    return n;
}

void PhysicsScene::RemoveLock(JointTracked& joint) {
    if (joint.lock == nullptr) return;
    physics_.System().RemoveConstraint(joint.lock);
    joint.lock = nullptr;
}

void PhysicsScene::RemoveJointConstraints(JointTracked& joint) {
    RemoveLock(joint);
    if (joint.constraint != nullptr) {
        physics_.System().RemoveConstraint(joint.constraint);
        joint.constraint = nullptr;
    }
}

void PhysicsScene::DetachBody(JPH::BodyID body) {
    if (body.IsInvalid()) return;
    for (auto& [index, joint] : joints_) {
        (void)index;
        if (joint.body1 == body || joint.body2 == body) RemoveJointConstraints(joint);
    }
    for (auto it = grabs_.begin(); it != grabs_.end();) {
        if (it->second.body == body) {
            if (it->second.constraint != nullptr) physics_.System().RemoveConstraint(it->second.constraint);
            it = grabs_.erase(it);
        } else {
            ++it;
        }
    }
}

void PhysicsScene::RemoveAllConstraints() {
    for (auto& [index, joint] : joints_) {
        (void)index;
        RemoveJointConstraints(joint);
    }
    joints_.clear();
    for (auto& [id, grab] : grabs_) {
        (void)id;
        if (grab.constraint != nullptr) physics_.System().RemoveConstraint(grab.constraint);
    }
    grabs_.clear();
}

bool PhysicsScene::CreateJoint(JointTracked& joint, const PhysicsJoint& c) {
    JPH::BodyInterface& bodies = physics_.BodyInterface();
    const JPH::BodyID b1 = joint.body1, b2 = joint.body2;
    const Vec3 point2 = WorldPoint(bodies, b2, c.anchor);
    JPH::RVec3 position2;
    JPH::Quat rotation2;
    bodies.GetPositionAndRotation(b2, position2, rotation2);

    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    switch (c.type) {
    case JointType::Fixed: {
        auto* s = new JPH::FixedConstraintSettings();
        s->mPoint1 = s->mPoint2 = JR(point2);
        settings = s;
        break;
    }
    case JointType::Point: {
        auto* s = new JPH::PointConstraintSettings();
        s->mPoint1 = s->mPoint2 = JR(point2);
        settings = s;
        break;
    }
    case JointType::Hinge: {
        auto* s = new JPH::HingeConstraintSettings();
        JPH::Vec3 axis = J(c.axis);
        if (axis.LengthSq() < 1e-12f) axis = JPH::Vec3::sAxisY();
        axis = (rotation2 * axis.Normalized()).Normalized();
        const JPH::Vec3 normal = axis.GetNormalizedPerpendicular();
        s->mPoint1 = s->mPoint2 = JR(point2);
        s->mHingeAxis1 = s->mHingeAxis2 = axis;
        s->mNormalAxis1 = s->mNormalAxis2 = normal;
        s->mLimitsMin = std::clamp(c.min_angle, -180.0f, 0.0f) * kDegToRad;
        s->mLimitsMax = std::clamp(c.max_angle, 0.0f, 180.0f) * kDegToRad;
        if (c.motor != JointMotor::None) {
            s->mMotorSettings = JPH::MotorSettings(std::max(c.motor_frequency, 0.0f), std::max(c.motor_damping, 0.0f));
            s->mMotorSettings.SetTorqueLimit(std::max(c.motor_max_torque, 0.0f));
        }
        settings = s;
        break;
    }
    case JointType::Distance: {
        auto* s = new JPH::DistanceConstraintSettings();
        const Vec3 point1 = WorldPoint(bodies, b1, c.connected_anchor);
        s->mPoint1 = JR(point1);
        s->mPoint2 = JR(point2);
        const Vec3 d = point2 - point1;
        const f32 length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        const f32 max_d = c.max_distance < 0.0f ? length : c.max_distance;
        s->mMaxDistance = max_d;
        s->mMinDistance = std::clamp(c.min_distance, 0.0f, max_d);
        if (c.spring_frequency > 0.0f) {
            s->mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, c.spring_frequency,
                                                           std::max(c.spring_damping, 0.0f));
        }
        settings = s;
        break;
    }
    }
    joint.constraint = bodies.CreateConstraint(settings, b1, b2);
    if (joint.constraint == nullptr) return false;
    if (c.type == JointType::Hinge && c.motor != JointMotor::None) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(joint.constraint.GetPtr());
        hinge->SetMotorState(c.motor == JointMotor::Velocity ? JPH::EMotorState::Velocity : JPH::EMotorState::Position);
    }
    SetMotorTarget(joint.constraint, c);
    joint.constraint->SetEnabled(c.enabled);
    physics_.System().AddConstraint(joint.constraint);
    bodies.ActivateConstraint(joint.constraint);
    joint.enabled = c.enabled;
    joint.motor_target = c.motor_target;
    return true;
}

void PhysicsScene::SyncJoints() {
    // Every PhysicsJoint, by entity index.
    std::vector<Entity> entities;
    const ComponentId joint_id = GetComponentId<PhysicsJoint>();
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(joint_id)) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    if (entities.empty() && joints_.empty()) return;
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });

    guids_.clear();
    const ComponentId id_component = GetComponentId<IdComponent>();
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(id_component)) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            for (usize i = 0; i < archetype.ChunkEntityCount(c); ++i) {
                if (const IdComponent* id = world_.GetComponent<IdComponent>(e[i])) guids_.emplace(id->guid, e[i]);
            }
        }
    });

    // Joints whose component or entity is gone.
    for (auto it = joints_.begin(); it != joints_.end();) {
        const Entity e = it->second.entity;
        if (!world_.IsAlive(e) || world_.GetComponent<PhysicsJoint>(e) == nullptr) {
            RemoveJointConstraints(it->second);
            it = joints_.erase(it);
        } else {
            ++it;
        }
    }

    JPH::BodyInterface& bodies = physics_.BodyInterface();
    for (Entity e : entities) {
        PhysicsJoint& c = *world_.GetComponent<PhysicsJoint>(e);
        auto [it, inserted] = joints_.try_emplace(e.index);
        JointTracked& joint = it->second;
        if (!inserted && joint.entity != e) RemoveJointConstraints(joint); // the index was reused
        joint.entity = e;

        // The bodies it should join now.
        const JPH::BodyID body2 = BodyOf(e);
        Entity other = kNullEntity;
        JPH::BodyID body1;
        bool ready = !body2.IsInvalid();
        if (!c.connected.IsNull()) {
            other = FindByGuid(c.connected);
            body1 = other.IsNull() || other == e ? JPH::BodyID() : BodyOf(other);
            if (body1.IsInvalid()) ready = false;
        }
        if (ready) {
            const bool moving2 = bodies.GetMotionType(body2) != JPH::EMotionType::Static;
            const bool moving1 = !body1.IsInvalid() && bodies.GetMotionType(body1) != JPH::EMotionType::Static;
            ready = moving1 || moving2;
        }
        std::vector<u8> settings = JointSettingsOf(c);
        const bool changed = settings != joint.settings || body1 != joint.body1 || body2 != joint.body2 || other != joint.other;
        if (changed || !ready || c.broken) RemoveJointConstraints(joint);
        joint.settings = std::move(settings);
        joint.body1 = ready ? body1 : JPH::BodyID();
        joint.body2 = ready ? body2 : JPH::BodyID();
        joint.other = other;
        if (!ready || c.broken) continue;

        if (joint.constraint == nullptr && !CreateJoint(joint, c)) continue;
        bool wake = false;
        if (joint.enabled != c.enabled) {
            joint.constraint->SetEnabled(c.enabled);
            if (joint.lock != nullptr) joint.lock->SetEnabled(c.enabled);
            joint.enabled = c.enabled;
            wake = true;
        }
        if (joint.motor_target != c.motor_target) {
            SetMotorTarget(joint.constraint, c);
            joint.motor_target = c.motor_target;
            wake = true;
        }
        if (c.locked && joint.lock == nullptr) {
            JPH::FixedConstraintSettings lock;
            lock.mAutoDetectPoint = true;
            joint.lock = bodies.CreateConstraint(&lock, joint.body1, joint.body2);
            if (joint.lock != nullptr) {
                joint.lock->SetEnabled(c.enabled);
                physics_.System().AddConstraint(joint.lock);
                wake = true;
            }
        } else if (!c.locked && joint.lock != nullptr) {
            RemoveLock(joint);
            wake = true;
        }
        if (wake) bodies.ActivateConstraint(joint.constraint);
    }
}

void PhysicsScene::CheckBreaks(f32 dt) {
    breaks_.clear();
    JPH::BodyInterface& bodies = physics_.BodyInterface();
    for (auto& [index, joint] : joints_) {
        (void)index;
        PhysicsJoint* c = world_.IsAlive(joint.entity) ? world_.GetComponent<PhysicsJoint>(joint.entity) : nullptr;
        if (c == nullptr || joint.constraint == nullptr) continue;
        if (joint.lock != nullptr && c->lock_break_force > 0.0f && c->enabled) {
            f32 force, torque;
            LoadOf(*joint.lock, dt, force, torque);
            const f32 load = std::max(force, torque);
            if (load > c->lock_break_force) {
                RemoveLock(joint);
                c->locked = false;
                breaks_.push_back({ConstraintBreakKind::Lock, joint.entity, joint.other, kInvalidGrab, load});
            }
        }
        if (c->enabled && (c->break_force > 0.0f || c->break_torque > 0.0f)) {
            f32 force, torque;
            LoadOf(*joint.constraint, dt, force, torque);
            if (joint.lock != nullptr) { // a locked joint carries the lock's load too
                f32 lf, lt;
                LoadOf(*joint.lock, dt, lf, lt);
                force += lf;
                torque += lt;
            }
            const bool by_force = c->break_force > 0.0f && force > c->break_force;
            const bool by_torque = c->break_torque > 0.0f && torque > c->break_torque;
            if (by_force || by_torque) {
                RemoveJointConstraints(joint);
                c->broken = true;
                breaks_.push_back({ConstraintBreakKind::Joint, joint.entity, joint.other, kInvalidGrab, by_force ? force : torque});
                continue;
            }
        }
        // Read back the angle (hinge) or separation (distance).
        if (c->type == JointType::Hinge) {
            c->angle = static_cast<JPH::HingeConstraint*>(joint.constraint.GetPtr())->GetCurrentAngle() * kRadToDeg;
        } else if (c->type == JointType::Distance) {
            const Vec3 d = WorldPoint(bodies, joint.body2, c->anchor) - WorldPoint(bodies, joint.body1, c->connected_anchor);
            c->angle = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        }
    }
    for (auto it = grabs_.begin(); it != grabs_.end();) {
        GrabTracked& grab = it->second;
        f32 force, torque;
        LoadOf(*grab.constraint, dt, force, torque);
        bool broke = grab.settings.break_force > 0.0f && force >= grab.settings.break_force;
        if (!broke && grab.settings.break_distance > 0.0f) {
            const Vec3 d = WorldPoint(bodies, grab.body, grab.local_point) - grab.target;
            broke = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) > grab.settings.break_distance;
        }
        if (broke) {
            physics_.System().RemoveConstraint(grab.constraint);
            breaks_.push_back({ConstraintBreakKind::Grab, grab.object, grab.holder, it->first, force});
            it = grabs_.erase(it);
        } else {
            ++it;
        }
    }
    for (const ConstraintBreakEvent& event : breaks_) {
        if (break_handler_) break_handler_(event);
    }
}

// --- Grabs ------------------------------------------------------------------

GrabId PhysicsScene::Grab(Entity object, Entity holder, const Vec3& world_point, const GrabSettings& settings) {
    const JPH::BodyID body = BodyOf(object);
    if (body.IsInvalid()) return kInvalidGrab;
    JPH::BodyInterface& bodies = physics_.BodyInterface();
    if (bodies.GetMotionType(body) != JPH::EMotionType::Dynamic) return kInvalidGrab;

    JPH::SixDOFConstraintSettings s;
    s.mPosition1 = s.mPosition2 = JR(world_point);
    for (int axis = JPH::SixDOFConstraintSettings::TranslationX; axis <= JPH::SixDOFConstraintSettings::TranslationZ; ++axis) {
        JPH::MotorSettings& motor = s.mMotorSettings[axis];
        motor = JPH::MotorSettings(std::max(settings.frequency, 0.01f), std::max(settings.damping, 0.0f));
        motor.SetForceLimit(std::max(settings.max_force, 0.0f));
    }
    JPH::Ref<JPH::TwoBodyConstraint> created = bodies.CreateConstraint(&s, JPH::BodyID(), body);
    if (created == nullptr) return kInvalidGrab;
    JPH::Ref<JPH::SixDOFConstraint> constraint = static_cast<JPH::SixDOFConstraint*>(created.GetPtr());
    for (int axis = JPH::SixDOFConstraintSettings::TranslationX; axis <= JPH::SixDOFConstraintSettings::TranslationZ; ++axis) {
        constraint->SetMotorState(static_cast<JPH::SixDOFConstraintSettings::EAxis>(axis), JPH::EMotorState::Position);
    }
    constraint->SetTargetPositionCS(JPH::Vec3::sZero());
    physics_.System().AddConstraint(constraint);
    bodies.ActivateBody(body);

    JPH::RVec3 position;
    JPH::Quat rotation;
    bodies.GetPositionAndRotation(body, position, rotation);
    GrabTracked grab;
    grab.object = object;
    grab.holder = holder;
    grab.body = body;
    grab.constraint = constraint;
    grab.origin = world_point;
    grab.target = world_point;
    grab.local_point = A(rotation.Conjugated() * JPH::Vec3(JR(world_point) - position));
    grab.settings = settings;
    const GrabId id = next_grab_++;
    grabs_.emplace(id, std::move(grab));
    return id;
}

bool PhysicsScene::SetGrabTarget(GrabId id, const Vec3& target) {
    auto it = grabs_.find(id);
    if (it == grabs_.end()) return false;
    it->second.target = target;
    it->second.constraint->SetTargetPositionCS(J(target - it->second.origin));
    physics_.BodyInterface().ActivateBody(it->second.body);
    return true;
}

bool PhysicsScene::Release(GrabId id) {
    auto it = grabs_.find(id);
    if (it == grabs_.end()) return false;
    physics_.System().RemoveConstraint(it->second.constraint);
    physics_.BodyInterface().ActivateBody(it->second.body);
    grabs_.erase(it);
    return true;
}

usize PhysicsScene::ReleaseHolder(Entity holder) {
    std::vector<GrabId> ids;
    for (const auto& [id, grab] : grabs_) {
        if (grab.holder == holder) ids.push_back(id);
    }
    for (GrabId id : ids) Release(id);
    return ids.size();
}

usize PhysicsScene::GrabCount(Entity object) const {
    usize n = 0;
    for (const auto& [id, grab] : grabs_) {
        (void)id;
        if (grab.object == object) ++n;
    }
    return n;
}

bool PhysicsScene::IsGrabActive(GrabId id) const { return grabs_.count(id) != 0; }

// --- Snapshots --------------------------------------------------------------

ConstraintSnapshot PhysicsScene::CaptureConstraintState() const {
    ConstraintSnapshot out;
    std::vector<Entity> moved;
    auto guid_of = [&](Entity e, EntityGuid& guid) {
        if (e.IsNull() || !world_.IsAlive(e)) return false;
        const IdComponent* id = world_.GetComponent<IdComponent>(e);
        if (id == nullptr || id->guid.IsNull()) return false;
        guid = id->guid;
        return true;
    };
    for (const auto& [index, joint] : joints_) {
        (void)index;
        const PhysicsJoint* c = world_.IsAlive(joint.entity) ? world_.GetComponent<PhysicsJoint>(joint.entity) : nullptr;
        EntityGuid guid;
        if (c == nullptr || !guid_of(joint.entity, guid)) continue;
        out.joints.push_back({guid, c->enabled, c->locked, c->broken, c->angle, c->motor_target});
        moved.push_back(joint.entity);
        if (!joint.other.IsNull()) moved.push_back(joint.other);
    }
    for (const auto& [id, grab] : grabs_) {
        (void)id;
        moved.push_back(grab.object);
    }
    std::sort(out.joints.begin(), out.joints.end(), [](const auto& a, const auto& b) { return a.entity < b.entity; });

    JPH::BodyInterface& bodies = physics_.BodyInterface();
    for (Entity e : moved) {
        EntityGuid guid;
        const JPH::BodyID body = BodyOf(e);
        if (body.IsInvalid() || !guid_of(e, guid)) continue;
        if (std::any_of(out.bodies.begin(), out.bodies.end(), [&](const BodyStateRecord& r) { return r.entity == guid; })) continue;
        BodyStateRecord r;
        r.entity = guid;
        JPH::RVec3 position;
        JPH::Quat rotation;
        bodies.GetPositionAndRotation(body, position, rotation);
        r.position = AR(position);
        r.rotation = Quaternion(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW());
        r.linear_velocity = A(bodies.GetLinearVelocity(body));
        r.angular_velocity = A(bodies.GetAngularVelocity(body));
        out.bodies.push_back(r);
    }
    std::sort(out.bodies.begin(), out.bodies.end(), [](const auto& a, const auto& b) { return a.entity < b.entity; });
    return out;
}

void PhysicsScene::ApplyConstraintState(const ConstraintSnapshot& snapshot) {
    GuidIndex index;
    index.Rebuild(world_);
    for (const JointStateRecord& r : snapshot.joints) {
        const Entity e = index.Find(world_, r.entity);
        PhysicsJoint* c = e.IsNull() ? nullptr : world_.GetComponent<PhysicsJoint>(e);
        if (c == nullptr) continue;
        c->enabled = r.enabled;
        c->locked = r.locked;
        c->broken = r.broken;
        c->angle = r.angle;
        c->motor_target = r.motor_target;
    }
    JPH::BodyInterface& bodies = physics_.BodyInterface();
    for (const BodyStateRecord& r : snapshot.bodies) {
        const Entity e = index.Find(world_, r.entity);
        if (e.IsNull()) continue;
        auto it = bodies_.find(EntityKey(e));
        if (it == bodies_.end() || it->second.body.IsInvalid()) continue;
        const JPH::Quat rotation = JPH::Quat(r.rotation.x, r.rotation.y, r.rotation.z, r.rotation.w).Normalized();
        bodies.SetPositionRotationAndVelocity(it->second.body, JR(r.position), rotation, J(r.linear_velocity), J(r.angular_velocity));
        if (it->second.motion == BodyMotion::Dynamic) bodies.ActivateBody(it->second.body);
        if (Transform* t = world_.GetComponent<Transform>(e)) {
            t->position = r.position;
            t->rotation = Quaternion(rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW());
            it->second.position = t->position;
            it->second.rotation = t->rotation;
        }
    }
}

namespace {

constexpr u8 kMagic[4] = {'A', 'E', 'C', 'S'};
constexpr u16 kSnapshotVersion = 1;
constexpr usize kJointRecordSize = 16 + 1 + 4 + 4;
constexpr usize kBodyRecordSize = 16 + 12 + 16 + 12 + 12;
constexpr u32 kMaxRecords = 1u << 20;

void Put(std::vector<u8>& out, const void* data, usize size) {
    // Little-endian hosts only (every supported target); copy byte by byte otherwise.
    const u8* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}
template <typename T>
void PutValue(std::vector<u8>& out, T value) {
    u8 bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    if constexpr (std::endian::native == std::endian::big) std::reverse(bytes, bytes + sizeof(T));
    Put(out, bytes, sizeof(T));
}
void PutVec(std::vector<u8>& out, const Vec3& v) {
    PutValue(out, v.x);
    PutValue(out, v.y);
    PutValue(out, v.z);
}

struct Reader {
    std::span<const u8> bytes;
    usize at = 0;
    template <typename T>
    bool Get(T& value) {
        if (bytes.size() - at < sizeof(T)) return false;
        u8 b[sizeof(T)];
        std::memcpy(b, bytes.data() + at, sizeof(T));
        if constexpr (std::endian::native == std::endian::big) std::reverse(b, b + sizeof(T));
        std::memcpy(&value, b, sizeof(T));
        at += sizeof(T);
        if constexpr (std::is_floating_point_v<T>) return std::isfinite(value);
        return true;
    }
    bool GetVec(Vec3& v) { return Get(v.x) && Get(v.y) && Get(v.z); }
};

} // namespace

std::vector<u8> SerializeConstraintSnapshot(const ConstraintSnapshot& snapshot) {
    std::vector<u8> out;
    out.reserve(4 + 2 + 8 + snapshot.joints.size() * kJointRecordSize + snapshot.bodies.size() * kBodyRecordSize);
    Put(out, kMagic, 4);
    PutValue(out, kSnapshotVersion);
    PutValue(out, static_cast<u32>(snapshot.joints.size()));
    for (const JointStateRecord& r : snapshot.joints) {
        PutValue(out, r.entity.hi);
        PutValue(out, r.entity.lo);
        PutValue(out, static_cast<u8>((r.enabled ? 1 : 0) | (r.locked ? 2 : 0) | (r.broken ? 4 : 0)));
        PutValue(out, r.angle);
        PutValue(out, r.motor_target);
    }
    PutValue(out, static_cast<u32>(snapshot.bodies.size()));
    for (const BodyStateRecord& r : snapshot.bodies) {
        PutValue(out, r.entity.hi);
        PutValue(out, r.entity.lo);
        PutVec(out, r.position);
        PutValue(out, r.rotation.x);
        PutValue(out, r.rotation.y);
        PutValue(out, r.rotation.z);
        PutValue(out, r.rotation.w);
        PutVec(out, r.linear_velocity);
        PutVec(out, r.angular_velocity);
    }
    return out;
}

bool DeserializeConstraintSnapshot(std::span<const u8> bytes, ConstraintSnapshot& out) {
    Reader in{bytes};
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kMagic, 4) != 0) return false;
    in.at = 4;
    u16 version = 0;
    if (!in.Get(version) || version != kSnapshotVersion) return false;
    ConstraintSnapshot result;
    u32 joints = 0;
    if (!in.Get(joints) || joints > kMaxRecords || (bytes.size() - in.at) / kJointRecordSize < joints) return false;
    result.joints.resize(joints);
    for (JointStateRecord& r : result.joints) {
        u8 flags = 0;
        if (!in.Get(r.entity.hi) || !in.Get(r.entity.lo) || !in.Get(flags) || flags > 7 || !in.Get(r.angle) ||
            !in.Get(r.motor_target)) {
            return false;
        }
        r.enabled = (flags & 1) != 0;
        r.locked = (flags & 2) != 0;
        r.broken = (flags & 4) != 0;
    }
    u32 bodies = 0;
    if (!in.Get(bodies) || bodies > kMaxRecords || (bytes.size() - in.at) != usize(bodies) * kBodyRecordSize) return false;
    result.bodies.resize(bodies);
    for (BodyStateRecord& r : result.bodies) {
        if (!in.Get(r.entity.hi) || !in.Get(r.entity.lo) || !in.GetVec(r.position) || !in.Get(r.rotation.x) ||
            !in.Get(r.rotation.y) || !in.Get(r.rotation.z) || !in.Get(r.rotation.w) || !in.GetVec(r.linear_velocity) ||
            !in.GetVec(r.angular_velocity)) {
            return false;
        }
    }
    if (in.at != bytes.size()) return false;
    out = std::move(result);
    return true;
}

} // namespace aether
