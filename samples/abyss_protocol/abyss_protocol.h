#pragma once

#include <aether/ecs/world.h>
#include <aether/math/vec3.h>
#include <vector>
#include <string>

namespace abyss {

struct AcousticEvent {
    aether::Vec3 position;
    float intensity;      // 0..1
    float radius;
    float remaining_time;
};

struct AcousticSignature {
    float noise_level = 0.0f;   // current continuous noise 0..1
    float detection_radius = 12.0f;
};

struct PressureState {
    float depth = 0.0f;
    float hull_integrity = 1.0f;
    float max_safe_depth = 200.0f;
};

struct CreatureAI {
    aether::Vec3 position;
    aether::Vec3 home;
    float awareness = 0.0f;     // 0..1, rises with noise/light
    float investigation_timer = 0.0f;
    bool is_alerted = false;
};

struct Submarine {
    aether::Vec3 position{0.0f, -15.0f, 0.0f};
    aether::Vec3 velocity{0.0f, 0.0f, 0.0f};
    bool sonar_active = false;
    bool lights_on = true;
    float battery = 1.0f;
    float oxygen = 1.0f;
};

class Game {
public:
    Game();
    void Step(float dt);
    void SetSonar(bool active);
    void SetLights(bool on);
    void ApplyThrust(const aether::Vec3& thrust);

    const Submarine& GetSub() const { return sub_; }
    const CreatureAI& GetCreature() const { return creature_; }
    const std::vector<AcousticEvent>& GetEvents() const { return events_; }
    std::string StatusText() const;

private:
    void UpdateAcoustics(float dt);
    void UpdateCreature(float dt);
    void UpdatePressure(float dt);

    aether::World world_;
    Submarine sub_;
    CreatureAI creature_;
    std::vector<AcousticEvent> events_;
};

}  // namespace abyss
