#include "abyss_protocol.h"
#include <cmath>
#include <sstream>
#include <limits>

namespace abyss {

namespace {
float Distance(const aether::Vec3& a, const aether::Vec3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

Game::Game() { Restart(); }

void Game::Restart() {
    sub_ = {};
    creature_ = {};
    relay_ = {};
    haven_ = {};
    events_.clear();
    mission_ = Mission::Outbound;
    time_ = 0.0f;
    fail_.clear();
    creature_.position = {25.0f, -20.0f, 10.0f};
    creature_.home = creature_.position;
}

void Game::SetSonar(bool active) { sub_.sonar_active = active; }
void Game::SetLights(bool on) { sub_.lights_on = on; }

void Game::ApplyThrust(const aether::Vec3& thrust) {
    sub_.velocity.x += thrust.x;
    sub_.velocity.y += thrust.y;
    sub_.velocity.z += thrust.z;
}

bool Game::LoadModel(const std::string& path, ModelInfo& out_info) {
    out_info = ModelInfo{};
    out_info.path = path;

    aether::assets::GltfScene scene;
    std::string error;
    if (!aether::assets::LoadGltf(path, scene, &error)) {
        out_info.error = error.empty() ? "LoadGltf failed" : error;
        return false;
    }

    out_info.loaded = true;
    out_info.mesh_count = scene.meshes.size();
    out_info.material_count = scene.materials.size();

    float min_x = std::numeric_limits<float>::max();
    float min_y = min_x, min_z = min_x;
    float max_x = -min_x, max_y = -min_x, max_z = -min_x;
    bool any_vert = false;

    for (const auto& mesh : scene.meshes) {
        for (const auto& prim : mesh.primitives) {
            out_info.primitive_count++;
            out_info.vertex_count += prim.vertices.size();
            for (const auto& v : prim.vertices) {
                any_vert = true;
                min_x = std::min(min_x, v.position[0]);
                min_y = std::min(min_y, v.position[1]);
                min_z = std::min(min_z, v.position[2]);
                max_x = std::max(max_x, v.position[0]);
                max_y = std::max(max_y, v.position[1]);
                max_z = std::max(max_z, v.position[2]);
            }
        }
    }

    if (any_vert) {
        out_info.bounds_min = {min_x, min_y, min_z};
        out_info.bounds_max = {max_x, max_y, max_z};
    }

    loaded_scene_ = std::move(scene);
    return true;
}

void Game::UpdateAcoustics(float dt) {
    float continuous = 0.05f;
    if (sub_.velocity.x * sub_.velocity.x + sub_.velocity.y * sub_.velocity.y +
            sub_.velocity.z * sub_.velocity.z > 0.1f) {
        continuous += 0.25f;
    }
    if (sub_.sonar_active) {
        continuous += 0.6f;
        events_.push_back({sub_.position, 0.85f, 40.0f, 2.0f});
    }
    if (sub_.lights_on) {
        continuous += 0.1f;
    }

    for (auto it = events_.begin(); it != events_.end();) {
        it->remaining_time -= dt;
        if (it->remaining_time <= 0.0f) {
            it = events_.erase(it);
        } else {
            ++it;
        }
    }
}

void Game::UpdateCreature(float dt) {
    float dist = Distance(creature_.position, sub_.position);
    float noise_influence = 0.0f;

    if (sub_.sonar_active) noise_influence += 0.7f;
    if (sub_.lights_on && dist < 30.0f) noise_influence += 0.3f;

    for (const auto& ev : events_) {
        float ev_dist = Distance(creature_.position, ev.position);
        if (ev_dist < ev.radius) {
            noise_influence += ev.intensity * (1.0f - ev_dist / ev.radius);
        }
    }

    creature_.awareness = std::min(1.0f, creature_.awareness + noise_influence * dt * 2.0f);
    creature_.awareness = std::max(0.0f, creature_.awareness - dt * 0.15f);

    if (creature_.awareness > 0.45f) {
        creature_.is_alerted = true;
        creature_.investigation_timer = 4.0f;
        aether::Vec3 dir = {sub_.position.x - creature_.position.x,
                            sub_.position.y - creature_.position.y,
                            sub_.position.z - creature_.position.z};
        float len = Distance(creature_.position, sub_.position);
        if (len > 1.0f) {
            dir.x /= len;
            dir.y /= len;
            dir.z /= len;
            creature_.position.x += dir.x * 3.0f * dt;
            creature_.position.y += dir.y * 3.0f * dt;
            creature_.position.z += dir.z * 3.0f * dt;
        }
    } else {
        creature_.is_alerted = false;
        aether::Vec3 home_dir = {creature_.home.x - creature_.position.x,
                                 creature_.home.y - creature_.position.y,
                                 creature_.home.z - creature_.position.z};
        float hlen = Distance(creature_.position, creature_.home);
        if (hlen > 0.5f) {
            home_dir.x /= hlen;
            home_dir.y /= hlen;
            home_dir.z /= hlen;
            creature_.position.x += home_dir.x * 1.0f * dt;
            creature_.position.y += home_dir.y * 1.0f * dt;
            creature_.position.z += home_dir.z * 1.0f * dt;
        }
    }

    if (creature_.investigation_timer > 0.0f) {
        creature_.investigation_timer -= dt;
    }
}

void Game::UpdatePressure(float dt) {
    float depth = -sub_.position.y;
    if (depth > 50.0f) {
        float excess = (depth - 50.0f) / 150.0f;
        sub_.battery -= excess * 0.05f * dt;
    }
    sub_.battery = std::max(0.0f, sub_.battery);
    sub_.oxygen = std::max(0.0f, sub_.oxygen - 0.01f * dt);
}

void Game::Step(float dt) {
    time_ += dt;
    if (mission_ == Mission::Won || mission_ == Mission::Lost) {
        sub_.velocity = {0.0f, 0.0f, 0.0f};
        return;
    }

    sub_.position.x += sub_.velocity.x * dt;
    sub_.position.y += sub_.velocity.y * dt;
    sub_.position.z += sub_.velocity.z * dt;
    sub_.velocity.x *= 0.95f;
    sub_.velocity.y *= 0.95f;
    sub_.velocity.z *= 0.95f;

    UpdateAcoustics(dt);
    UpdateCreature(dt);
    UpdatePressure(dt);
    UpdateRelay();
    UpdateMission();
}

aether::Vec3 Game::Objective() const {
    if (mission_ == Mission::Return || mission_ == Mission::Won) return haven_.position;
    return relay_.position;
}

void Game::UpdateMission() {
    float creature_dist = Distance(sub_.position, creature_.position);
    if (creature_.is_alerted && creature_dist < 4.0f) {
        mission_ = Mission::Lost;
        fail_ = "caught";
        return;
    }
    if (sub_.oxygen <= 0.0f) {
        mission_ = Mission::Lost;
        fail_ = "oxygen";
        return;
    }
    if (sub_.battery <= 0.0f) {
        mission_ = Mission::Lost;
        fail_ = "battery";
        return;
    }
    if (mission_ == Mission::Outbound && relay_.restored) mission_ = Mission::Return;
    if (mission_ == Mission::Return && Distance(sub_.position, haven_.position) < 8.0f) mission_ = Mission::Won;
}

void Game::UpdateRelay() {
    if (relay_.restored) return;
    float dist = Distance(sub_.position, relay_.position);
    // The relay only answers a quiet boat: no sonar, lights off, creature calm.
    if (dist < 6.0f && !sub_.sonar_active && !sub_.lights_on && !creature_.is_alerted) {
        relay_.restored = true;
    }
}

std::string Game::StatusText() const {
    std::ostringstream oss;
    oss << "Depth: " << (-sub_.position.y)
        << "  Battery: " << int(sub_.battery * 100) << "%"
        << "  Sonar: " << (sub_.sonar_active ? "ON" : "off")
        << "  Lights: " << (sub_.lights_on ? "ON" : "off")
        << "  Creature awareness: " << int(creature_.awareness * 100) << "%"
        << (creature_.is_alerted ? "  [ALERT]" : "");
    if (mission_ == Mission::Won) oss << "  MISSION COMPLETE  " << int(time_) << "s";
    else if (mission_ == Mission::Lost) oss << "  FAILED: " << fail_ << "  (R retry)";
    else if (mission_ == Mission::Return) oss << "  Return to Haven";
    else oss << "  Relay: silent";
    return oss.str();
}

}  // namespace abyss
