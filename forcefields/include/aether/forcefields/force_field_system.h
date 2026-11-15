#pragma once

#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/forcefields/force_field.h"
#include "aether/physics/physics_scene.h"

#include <vector>

namespace aether::ff {

// What one field does to a body, worked out without any physics: the acceleration (m/s^2) at `position` for a body moving at `velocity`, where `centre` and
// `rotation` are the field's. `submersion` (0..1) is how much of the body is below the top of the volume (Buoyancy only, found from the body's bounds).
// Directional, Radial and Vortex give the raw push (acceleration or force, by `mass_independent`); the caller turns an acceleration into a force with the
// body's mass. Exposed so the maths can be tested on its own.
Vec3 FieldPush(const ForceField& field, const Vec3& centre, const Quaternion& rotation, const Vec3& position, const Vec3& velocity, const Vec3& gravity, f32 submersion);
// Where the top of the volume is (Buoyancy's surface), in world height.
f32 FieldSurfaceHeight(const ForceField& field, const Vec3& centre);
// A vector rotated by a quaternion.
Vec3 Rotated(const Quaternion& q, const Vec3& v);

// Pushes the dynamic bodies inside every enabled ForceField, once per physics step (call Apply just before PhysicsScene::Step: Jolt adds up the
// forces until the step that uses them). Deterministic: fields go in entity order and each field's bodies in the overlap query's order. Also explodes.
class ForceFieldSystem {
public:
    ForceFieldSystem(World& world, PhysicsScene& scene);
    ~ForceFieldSystem();
    ForceFieldSystem(const ForceFieldSystem&) = delete;
    ForceFieldSystem& operator=(const ForceFieldSystem&) = delete;

    static ForceFieldSystem* Active();
    void MakeActive();

    // Adds every field's force to the bodies in it. Returns how many pushes it made (a body in two fields counts twice).
    usize Apply();

    // An explosion: every dynamic body whose collider touches the sphere gets a change of velocity of `strength` m/s, away from `centre` (plus `upward`
    // of that straight up, 0 to 1, as a lift), fading linearly to nothing at `radius`. The same for a heavy and a light body. Returns how many it
    // pushed; 0 for a radius that is not positive or a strength that is not a number.
    usize Explode(const Vec3& centre, f32 radius, f32 strength, f32 upward = 0.0f);

private:
    std::vector<Entity> Fields() const;

    World& world_;
    PhysicsScene& scene_;
};

} // namespace aether::ff
