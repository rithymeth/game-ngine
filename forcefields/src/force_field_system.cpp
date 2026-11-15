#include "aether/forcefields/force_field_system.h"

#include "aether/physics/components.h"
#include "aether/scene/components.h"

#include <Jolt/Physics/Collision/TransformedShape.h>

#include <algorithm>
#include <cmath>

namespace aether::ff {

namespace {

ForceFieldSystem* g_active = nullptr;

f32 Clamp(f32 v, f32 lo, f32 hi) { return std::clamp(v, lo, hi); }
bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// How far a radial or vortex push has faded at `distance` from the middle: 1 at the middle, 0 at the edge, falling as (1 - d/edge)^falloff.
f32 Fade(f32 distance, f32 edge, f32 falloff) {
    if (!(falloff > 0.0f) || !(edge > 0.0f)) return 1.0f;
    return std::pow(Clamp(1.0f - distance / edge, 0.0f, 1.0f), falloff);
}

f32 Edge(const ForceField& f) {
    if (f.shape == FieldShape::Sphere) return std::max(f.radius, ForceField::kMinSize);
    return std::max({f.half_extents.x, f.half_extents.y, f.half_extents.z, ForceField::kMinSize});
}

} // namespace

Vec3 Rotated(const Quaternion& q, const Vec3& v) {
    const Vec3 u(q.x, q.y, q.z);
    return v + u.Cross(u.Cross(v) + v * q.w) * 2.0f;
}

f32 FieldSurfaceHeight(const ForceField& field, const Vec3& centre) {
    const f32 half = field.shape == FieldShape::Sphere ? field.radius : field.half_extents.y;
    return centre.y + Clamp(half, ForceField::kMinSize, ForceField::kMaxSize);
}

Vec3 FieldPush(const ForceField& field, const Vec3& centre, const Quaternion& rotation, const Vec3& position, const Vec3& velocity, const Vec3& gravity, f32 submersion) {
    const f32 strength = std::isfinite(field.strength) ? field.strength : 0.0f;
    switch (field.mode) {
        case FieldMode::Directional: {
            if (!Finite(field.direction) || field.direction.LengthSq() < 1e-12f) return Vec3(0, 0, 0);
            return field.direction.Normalized() * strength;
        }
        case FieldMode::Radial: {
            const Vec3 d = position - centre;
            const f32 distance = d.Length();
            if (distance < 1e-4f) return Vec3(0, 0, 0);
            return d * (-strength * Fade(distance, Edge(field), field.falloff) / distance); // positive pulls toward the centre
        }
        case FieldMode::Vortex: {
            const Vec3 axis = Rotated(rotation, Vec3(0, 1, 0));
            const Vec3 d = position - centre;
            const Vec3 r = d - axis * d.Dot(axis);
            const f32 distance = r.Length();
            if (distance < 1e-4f) return Vec3(0, 0, 0);
            return axis.Cross(r) * (strength * Fade(distance, Edge(field), field.falloff) / distance);
        }
        case FieldMode::Buoyancy: {
            const f32 s = Clamp(submersion, 0.0f, 1.0f);
            const f32 drag = std::isfinite(field.fluid_drag) ? std::max(field.fluid_drag, 0.0f) : 0.0f;
            return gravity * (-strength * s) - velocity * (drag * s); // against gravity, and against the motion
        }
        case FieldMode::Drag:
            return velocity * (-std::max(strength, 0.0f));
    }
    return Vec3(0, 0, 0);
}

ForceFieldSystem::ForceFieldSystem(World& world, PhysicsScene& scene) : world_(world), scene_(scene) {
    RegisterForceFieldComponents();
    MakeActive();
}
ForceFieldSystem::~ForceFieldSystem() {
    if (g_active == this) g_active = nullptr;
}
ForceFieldSystem* ForceFieldSystem::Active() { return g_active; }
void ForceFieldSystem::MakeActive() { g_active = this; }

std::vector<Entity> ForceFieldSystem::Fields() const {
    std::vector<Entity> out;
    const ComponentId id = GetComponentId<ForceField>();
    world_.ForEachArchetype([&](const Archetype& a) {
        if (!a.Mask().test(id)) return;
        for (usize c = 0; c < a.ChunkCount(); ++c) {
            const Entity* chunk = a.EntityArray(c);
            out.insert(out.end(), chunk, chunk + a.ChunkEntityCount(c));
        }
    });
    std::sort(out.begin(), out.end(), [](Entity x, Entity y) { return x.index != y.index ? x.index < y.index : x.generation < y.generation; });
    return out;
}

usize ForceFieldSystem::Apply() {
    usize pushes = 0;
    JPH::BodyInterface& bodies = scene_.Physics().BodyInterface();
    const Vec3 gravity = scene_.Physics().GetGravity();
    for (const Entity field_entity : Fields()) {
        if (!world_.IsAlive(field_entity)) continue;
        const ForceField* field = world_.GetComponent<ForceField>(field_entity);
        const Transform* at = world_.GetComponent<Transform>(field_entity);
        if (field == nullptr || at == nullptr || !field->enabled || !Finite(at->position)) continue;
        const ForceField f = *field; // a copy: nothing below changes entities, but a copy costs nothing and cannot dangle
        const Vec3 centre = at->position;
        const Quaternion rotation = at->rotation;
        std::vector<Entity> inside;
        if (f.shape == FieldShape::Sphere) {
            const f32 radius = std::isfinite(f.radius) ? Clamp(f.radius, ForceField::kMinSize, ForceField::kMaxSize) : ForceField::kMinSize;
            inside = scene_.OverlapSphere(centre, radius, kAllLayers, field_entity, false);
        } else {
            const Vec3 half(Clamp(f.half_extents.x, ForceField::kMinSize, ForceField::kMaxSize), Clamp(f.half_extents.y, ForceField::kMinSize, ForceField::kMaxSize),
                            Clamp(f.half_extents.z, ForceField::kMinSize, ForceField::kMaxSize));
            inside = scene_.OverlapOrientedBox(centre, half, rotation, kAllLayers, field_entity, false);
        }
        const f32 surface = FieldSurfaceHeight(f, centre);
        for (const Entity e : inside) {
            const JPH::BodyID id = scene_.BodyOf(e);
            if (id.IsInvalid() || bodies.GetMotionType(id) != JPH::EMotionType::Dynamic) continue;
            const RigidBody* rb = world_.GetComponent<RigidBody>(e);
            const f32 mass = rb != nullptr && rb->mass > 0.0f ? rb->mass : 1.0f;
            const JPH::RVec3 com = bodies.GetCenterOfMassPosition(id);
            const JPH::Vec3 lv = bodies.GetLinearVelocity(id);
            const Vec3 position(static_cast<f32>(com.GetX()), static_cast<f32>(com.GetY()), static_cast<f32>(com.GetZ()));
            const Vec3 velocity(lv.GetX(), lv.GetY(), lv.GetZ());
            f32 submersion = 0.0f;
            if (f.mode == FieldMode::Buoyancy) {
                const JPH::AABox box = bodies.GetTransformedShape(id).GetWorldSpaceBounds();
                const f32 bottom = box.mMin.GetY(), top = box.mMax.GetY();
                submersion = Clamp((surface - bottom) / std::max(top - bottom, 1e-3f), 0.0f, 1.0f);
            }
            Vec3 push = FieldPush(f, centre, rotation, position, velocity, gravity, submersion);
            // An acceleration becomes a force with the mass; Buoyancy and Drag are accelerations always, the others by their switch.
            const bool acceleration = f.mode == FieldMode::Buoyancy || f.mode == FieldMode::Drag || f.mass_independent;
            if (acceleration) push = push * mass;
            if (!Finite(push) || push.LengthSq() == 0.0f) continue;
            bodies.AddForce(id, JPH::Vec3(push.x, push.y, push.z));
            ++pushes;
        }
    }
    return pushes;
}

usize ForceFieldSystem::Explode(const Vec3& centre, f32 radius, f32 strength, f32 upward) {
    if (!Finite(centre) || !(radius > 0.0f) || !std::isfinite(radius) || !std::isfinite(strength) || !std::isfinite(upward)) return 0;
    const f32 lift = Clamp(upward, 0.0f, 1.0f);
    JPH::BodyInterface& bodies = scene_.Physics().BodyInterface();
    usize pushed = 0;
    for (const Entity e : scene_.OverlapSphere(centre, Clamp(radius, ForceField::kMinSize, ForceField::kMaxSize), kAllLayers, kNullEntity, false)) {
        const JPH::BodyID id = scene_.BodyOf(e);
        if (id.IsInvalid() || bodies.GetMotionType(id) != JPH::EMotionType::Dynamic) continue;
        const RigidBody* rb = world_.GetComponent<RigidBody>(e);
        const f32 mass = rb != nullptr && rb->mass > 0.0f ? rb->mass : 1.0f;
        const JPH::RVec3 com = bodies.GetCenterOfMassPosition(id);
        const Vec3 d = Vec3(static_cast<f32>(com.GetX()), static_cast<f32>(com.GetY()), static_cast<f32>(com.GetZ())) - centre;
        const f32 distance = d.Length();
        const f32 fade = Clamp(1.0f - distance / radius, 0.0f, 1.0f);
        if (fade <= 0.0f) continue; // the body only touches the edge from outside its centre: nothing left of the blast there
        Vec3 away = distance > 1e-4f ? d * (1.0f / distance) : Vec3(0, 1, 0); // dead centre: straight up
        Vec3 dir = away + Vec3(0, lift, 0);
        if (dir.LengthSq() < 1e-8f) continue;
        dir = dir.Normalized();
        const Vec3 impulse = dir * (strength * fade * mass);
        bodies.AddImpulse(id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
        ++pushed;
    }
    return pushed;
}

} // namespace aether::ff
