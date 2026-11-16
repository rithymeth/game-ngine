#pragma once

#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"
#include "aether/math/math.h"

#include <memory>

namespace aether::fire {

// Advances every enabled Flammable of a world: heats the ones near a burning one, lights what has been heated enough, burns what is alight and puts out what is wet.
// Each step does it in the same order whatever the world's layout (entities by index; heat given out is worked out first and then taken, so which one goes first
// does not matter), so a run repeats exactly. The flames are a child entity with a ParticleSystem and a PointLight that follows the burning one and is cleaned up
// a few seconds after it goes out. One per world; scripts reach the newest through Active().
class FireSystem {
public:
    explicit FireSystem(World& world);
    ~FireSystem();
    FireSystem(const FireSystem&) = delete;
    FireSystem& operator=(const FireSystem&) = delete;

    static FireSystem* Active();
    void MakeActive();

    // One step of `dt` seconds. Returns how many things are burning after it.
    usize Advance(f32 dt);

    // Sets it alight now (its heat is taken as enough). False for an entity that is not an enabled Flammable, one that is burnt out or already burning, or one too wet.
    bool Ignite(Entity entity);
    // Puts it out: it stops burning and its heat is gone (what is left of its fuel is kept, so it can catch again). False if it was not burning.
    bool Extinguish(Entity entity);
    // Sets alight every enabled, dry Flammable within `radius` of `centre` (a fire bomb, a lightning strike). Returns how many caught.
    usize IgniteNear(const Vec3& centre, f32 radius);

    bool IsBurning(Entity entity) const;
    bool IsBurnt(Entity entity) const;
    f32 Heat(Entity entity) const;     // 0 up to its ignition heat; 0 for an entity that is not a Flammable
    f32 FuelLeft(Entity entity) const; // seconds; 0 for an entity that is not a Flammable, or has burnt out
    usize BurningCount() const;
    // The flame entity that follows a burning (or just extinguished) one; null if none.
    Entity FlameOf(Entity entity) const;

    // Sets how wet one Flammable is (clamped to 0..1), or every one. False for an entity that is not a Flammable, or a number that is not finite; WetAll returns how many it set.
    bool SetWetness(Entity entity, f32 wetness);
    usize WetAll(f32 wetness);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    World& world_;
};

} // namespace aether::fire
