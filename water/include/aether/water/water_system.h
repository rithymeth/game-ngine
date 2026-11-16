#pragma once

#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/physics/physics_scene.h"
#include "aether/water/water.h"

#include <memory>

namespace aether::water {

// What the water is at one point: the surface height (world Y), the current there (m/s) and the water's density.
struct SurfaceSample {
    f32 height = 0.0f;
    Vec3 current{0.0f, 0.0f, 0.0f};
    f32 density = 1000.0f;
};

// The wave height above the still level, from a surface's own settings and a time, without any physics. Exposed so the maths can be tested on its own.
f32 WaveHeight(const WaterSurface& surface, f32 x, f32 z, f32 time);

// What the last step found of one floater.
struct FloaterState {
    f32 submersion = 0.0f;       // 0 to 1: how much of the hull is under the water
    f32 surface_height = 0.0f;   // the water's height under the hull's middle (world Y); 0 when there is no water there
    bool in_water = false;       // any of it under
};

// Floats every enabled Floater and drives every Boat, once per physics step: call Apply just before PhysicsScene::Step (Jolt adds up the forces until the step that
// uses them). The surface's waves are a function of the system's own clock, which Apply advances by the step, so a run is repeatable. Deterministic: surfaces and
// floaters go in entity order, columns in order.
class WaterSystem {
public:
    WaterSystem(World& world, PhysicsScene& scene);
    ~WaterSystem();
    WaterSystem(const WaterSystem&) = delete;
    WaterSystem& operator=(const WaterSystem&) = delete;

    static WaterSystem* Active();
    void MakeActive();

    // Advances the clock by `dt` and pushes every floater. Returns how many floaters were in the water.
    usize Apply(f32 dt);

    // The water at a point, from the surfaces as they are now. False when no surface covers it (then `out` is not set).
    bool SurfaceAt(f32 x, f32 z, SurfaceSample& out) const;
    // Just the height: the lowest representable number where there is no water.
    f32 HeightAt(f32 x, f32 z) const;

    // What the last Apply found for a floater; a zero state for an entity that is not one.
    FloaterState StateOf(Entity floater) const;

    // A boat's controls, kept until changed: throttle -1 (reverse) to 1, steer -1 (left) to 1 (right). False for an entity that is not an enabled boat, or a number
    // that is not finite.
    bool Drive(Entity boat, f32 throttle, f32 steer);

    // The system's clock (s), which sets the waves' phase; it starts at 0.
    f32 Time() const;
    void SetTime(f32 time);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    World& world_;
    PhysicsScene& scene_;
};

} // namespace aether::water
