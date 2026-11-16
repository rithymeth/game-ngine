#pragma once

#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/physics/physics_scene.h"
#include "aether/vehicles/vehicle.h"

#include <memory>

namespace aether::vehicles {

// One wheel's pose and state, for drawing it: `position` and `rotation` are the wheel's in the world for a wheel mesh whose axle runs along its local X axis
// (steering and spin included), `suspension` is the spring's current length (m).
struct WheelInfo {
    Vec3 position{0, 0, 0};
    Quaternion rotation = Quaternion::Identity();
    f32 steer_angle = 0.0f;  // radians
    f32 spin_angle = 0.0f;   // radians, how far the wheel has turned
    f32 suspension = 0.0f;
    bool contact = false;
};

constexpr u32 kWheelCount = 4;

// Builds a Jolt vehicle constraint for every enabled Vehicle (on a dynamic body with a box collider), feeds it the entity's VehicleInput and writes its
// VehicleState, once per physics step: call Apply just before PhysicsScene::Step. A vehicle whose entity is gone, whose component is disabled or whose
// settings (or chassis) were edited is taken apart first and built again, so no constraint ever outlives the body it belongs to.
class VehicleSystem {
public:
    VehicleSystem(World& world, PhysicsScene& scene);
    ~VehicleSystem();
    VehicleSystem(const VehicleSystem&) = delete;
    VehicleSystem& operator=(const VehicleSystem&) = delete;

    static VehicleSystem* Active();
    void MakeActive();

    // Updates the vehicles. Returns how many are driving (constraints alive).
    usize Apply();

    // Sets a vehicle's input (clamped to its ranges). False for an entity that has no Vehicle or a value that is not a number.
    bool Drive(Entity entity, f32 throttle, f32 steer, f32 brake = 0.0f, f32 handbrake = 0.0f);
    // A wheel's pose, for drawing; false if the vehicle isn't built yet or `index` is not 0..3.
    bool Wheel(Entity entity, u32 index, WheelInfo& out) const;
    // Stands the vehicle back on its wheels where it is (same heading, no spin, no motion), e.g. after a roll. False for an entity that is not a built vehicle.
    bool Flip(Entity entity);

    // The vehicle's state as of the last Apply; null for an entity that is not a built vehicle.
    const VehicleState* StateOf(Entity entity) const;

    usize VehicleCount() const; // vehicles with a live constraint

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    World& world_;
    PhysicsScene& scene_;
};

} // namespace aether::vehicles
