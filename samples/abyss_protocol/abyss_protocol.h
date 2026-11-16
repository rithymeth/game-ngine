#pragma once

#include <aether/ecs/world.h>
#include <aether/math/vec3.h>
#include <aether/assets/gltf_loader.h>
#include <vector>
#include <string>
#include <optional>

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

struct Relay {
    aether::Vec3 position{48.0f, -32.0f, -18.0f};
    bool restored = false;
};

enum class Mission { Outbound, Return, Won, Lost };

struct Rock {
    aether::Vec3 position;
    float radius = 3.0f;
};

struct Haven {
    aether::Vec3 position{0.0f, -15.0f, 0.0f};
};

struct Submarine {
    aether::Vec3 position{0.0f, -15.0f, 0.0f};
    aether::Vec3 velocity{0.0f, 0.0f, 0.0f};
    bool sonar_active = false;
    bool lights_on = true;
    float battery = 1.0f;
    float oxygen = 1.0f;
};

// Simple summary of a loaded glTF model for the prototype.
struct ModelInfo {
    std::string path;
    size_t mesh_count = 0;
    size_t primitive_count = 0;
    size_t vertex_count = 0;
    size_t material_count = 0;
    aether::Vec3 bounds_min{0, 0, 0};
    aether::Vec3 bounds_max{0, 0, 0};
    bool loaded = false;
    std::string error;
};

class Game {
public:
    Game();
    void Step(float dt);
    void SetSonar(bool active);
    void SetLights(bool on);
    void ApplyThrust(const aether::Vec3& thrust);
    void Ping();
    void Restart();

    // Load a glTF/GLB and store a summary (does not yet upload to GPU).
    bool LoadModel(const std::string& path, ModelInfo& out_info);

    const Submarine& GetSub() const { return sub_; }
    const CreatureAI& GetCreature() const { return creature_; }
    const Relay& GetRelay() const { return relay_; }
    const Haven& GetHaven() const { return haven_; }
    const std::vector<Rock>& GetRocks() const { return rocks_; }
    bool HasContact() const { return contact_ > 0.05f; }
    float Contact() const { return contact_; }
    Mission GetMission() const { return mission_; }
    aether::Vec3 Objective() const;
    float Time() const { return time_; }
    const std::vector<AcousticEvent>& GetEvents() const { return events_; }
    std::string StatusText() const;

private:
    void UpdateAcoustics(float dt);
    void UpdateCreature(float dt);
    void UpdatePressure(float dt);
    void UpdateRelay();
    void UpdateMission();
    void ResolveRocks();

    aether::World world_;
    Submarine sub_;
    CreatureAI creature_;
    Relay relay_;
    Haven haven_;
    std::vector<Rock> rocks_;
    Mission mission_ = Mission::Outbound;
    float time_ = 0.0f;
    float contact_ = 0.0f;
    float ping_cooldown_ = 0.0f;
    std::string fail_;
    std::vector<AcousticEvent> events_;
    std::optional<aether::assets::GltfScene> loaded_scene_;
};

}  // namespace abyss
