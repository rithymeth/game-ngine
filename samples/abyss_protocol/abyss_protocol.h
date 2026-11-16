#pragma once

#include <aether/ecs/world.h>
#include <aether/math/vec3.h>
#include <cstdint>

namespace abyss {

struct AcousticSignature {
    float noise_level = 0.0f;  // 0..1 current emitted noise
    float detection_radius = 10.0f;
};

struct PressureState {
    float depth = 0.0f;
    float hull_integrity = 1.0f;
};

struct Submarine {
    aether::Vec3 position{0, 0, 0};
    aether::Vec3 velocity{0, 0, 0};
    bool sonar_active = false;
    bool lights_on = true;
};

class Game {
public:
    Game();
    void Step(float dt);
    const Submarine& GetSub() const { return sub_; }

private:
    aether::World world_;
    Submarine sub_;
    // Placeholder entities for creature, relay, etc.
};

}  // namespace abyss
