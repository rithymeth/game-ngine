#pragma once

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/scene/components.h"

namespace aether::player {

// Camera shake with the "trauma" model: an impact adds trauma (0 to 1), the camera shakes by trauma squared (small hits are small, big ones violent), and
// trauma drains away over time. The shake is a smooth deterministic wobble, not random noise, so it is the same on every run and in every test.
//
// It does not touch what a script sets: the game applies it to the active camera's Transform after the frame's scripts have run (so the renderer, the
// shadows and the listener all see the shaken camera) and takes it off again before the next frame's scripts, which therefore always start from
// the camera they put there themselves.
class CameraShake {
public:
    struct Settings {
        f32 max_offset = 0.30f;        // metres, at full shake, in the camera's own axes
        f32 max_angle_degrees = 3.0f;  // pitch, yaw and roll, at full shake
        f32 frequency = 17.0f;         // how fast it wobbles
        f32 decay = 1.2f;              // trauma lost per second
    };

    void SetSettings(const Settings& settings) { settings_ = settings; }
    const Settings& GetSettings() const { return settings_; }

    // Adds trauma, kept within [0, 1]. A negative or non-finite amount is ignored.
    void AddTrauma(f32 amount);
    f32 Trauma() const { return trauma_; }

    // This frame's offset and angles at the current trauma (the pure function the game applies), for tests and for hosts that move the camera themselves.
    struct Pose {
        Vec3 offset;
        Vec3 angles_degrees; // pitch (x), yaw (y), roll (z)
    };
    Pose Evaluate() const;

    // Moves `camera`'s Transform by the shake, remembering where it was, then lets the trauma decay by dt. Does nothing without trauma or a camera.
    void Apply(World& world, Entity camera, f32 dt);
    // Puts the camera's Transform back as it was before Apply. Safe to call at any time, and when nothing was applied.
    void Restore(World& world);
    // Forgets what was applied without writing it back (the world it belonged to is gone). Trauma is cleared too.
    void Reset();

private:
    Settings settings_;
    f32 trauma_ = 0.0f;
    f64 time_ = 0.0;
    Entity applied_ = kNullEntity;
    Transform original_;
};

} // namespace aether::player
