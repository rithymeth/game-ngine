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
    relay_ = {};
    haven_ = {};
    cache_ = {};
    events_.clear();
    creatures_.clear();
    mission_ = Mission::Outbound;
    time_ = 0.0f;
    fail_.clear();
    bearing_valid_ = false;
    contact_ = 0.0f;
    ping_cooldown_ = 0.0f;
    impact_cooldown_ = 0.0f;
    noise_ = 0.0f;
    leak_ = 0.0f;
    CreatureAI outbound;
    outbound.position = {25.0f, -20.0f, 10.0f};
    outbound.home = outbound.position;
    outbound.patrol_radius = 8.0f;
    outbound.phase = 0.4f;
    CreatureAI inbound;
    inbound.position = {22.0f, -26.0f, -14.0f};
    inbound.home = inbound.position;
    inbound.patrol_radius = 6.0f;
    inbound.phase = 2.2f;
    creatures_.push_back(outbound);
    creatures_.push_back(inbound);
    rocks_ = {
        {{16.0f, -18.0f, -2.0f}, 3.2f},
        {{30.0f, -24.0f, -6.0f}, 4.0f},
        {{42.0f, -29.0f, 4.0f}, 2.8f},
    };
}

void Game::Ping() {
    if (mission_ == Mission::Won || mission_ == Mission::Lost) return;
    if (ping_cooldown_ > 0.0f) return;
    if (sub_.battery < 0.08f) return;
    ping_cooldown_ = 1.6f;
    sub_.battery -= 0.08f;
    sub_.sonar_active = true;
    contact_ = 3.2f;
    aether::Vec3 goal = Objective();
    aether::Vec3 dir{goal.x - sub_.position.x, goal.y - sub_.position.y, goal.z - sub_.position.z};
    float len = Distance(sub_.position, goal);
    if (len > 0.5f) {
        bearing_ = {dir.x / len, dir.y / len, dir.z / len};
        bearing_valid_ = true;
    }
}

void Game::SetLights(bool on) {
    if (on && sub_.battery <= 0.02f) on = false;
    sub_.lights_on = on;
}

void Game::SetSonar(bool active) { sub_.sonar_active = active; }

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
    float speed = std::sqrt(sub_.velocity.x * sub_.velocity.x + sub_.velocity.y * sub_.velocity.y +
                            sub_.velocity.z * sub_.velocity.z);
    // Full thrust is loud. A creep (Shift) stays under the creatures' hearing.
    noise_ = std::min(0.9f, speed * 0.09f);
    if (sub_.lights_on) noise_ += 0.08f;
    if (sub_.sonar_active) {
        noise_ = std::max(noise_, 0.75f);
        events_.push_back({sub_.position, 0.7f, 22.0f, 2.0f});
    }
    (void)dt;

    for (auto it = events_.begin(); it != events_.end();) {
        it->remaining_time -= dt;
        if (it->remaining_time <= 0.0f) {
            it = events_.erase(it);
        } else {
            ++it;
        }
    }
}

void Game::UpdateCreature(CreatureAI& creature, float dt) {
    float dist = Distance(creature.position, sub_.position);
    float noise_influence = 0.0f;
    float hear_radius = 8.0f + noise_ * 20.0f;
    if (dist < hear_radius) noise_influence += noise_ * (1.0f - dist / hear_radius);
    if (sub_.lights_on && dist < 18.0f) noise_influence += 0.22f;
    if (sub_.sonar_active) noise_influence += 0.55f;

    for (const auto& ev : events_) {
        float ev_dist = Distance(creature.position, ev.position);
        if (ev_dist < ev.radius) {
            noise_influence += ev.intensity * (1.0f - ev_dist / ev.radius);
        }
    }

    creature.awareness = std::min(1.0f, creature.awareness + noise_influence * dt * 2.0f);
    creature.awareness = std::max(0.0f, creature.awareness - dt * 0.15f);

    if (creature.awareness > 0.45f) {
        creature.is_alerted = true;
        creature.investigation_timer = 4.0f;
        aether::Vec3 dir = {sub_.position.x - creature.position.x,
                            sub_.position.y - creature.position.y,
                            sub_.position.z - creature.position.z};
        float len = Distance(creature.position, sub_.position);
        if (len > 1.0f) {
            dir.x /= len;
            dir.y /= len;
            dir.z /= len;
            creature.heading = std::atan2(dir.x, dir.z);
            creature.position.x += dir.x * 3.0f * dt;
            creature.position.y += dir.y * 3.0f * dt;
            creature.position.z += dir.z * 3.0f * dt;
        }
    } else {
        creature.is_alerted = false;
        creature.phase += dt * 0.45f;
        aether::Vec3 goal{creature.home.x + std::cos(creature.phase) * creature.patrol_radius, creature.home.y,
                          creature.home.z + std::sin(creature.phase) * creature.patrol_radius};
        aether::Vec3 home_dir{goal.x - creature.position.x, goal.y - creature.position.y, goal.z - creature.position.z};
        float hlen = std::sqrt(home_dir.x * home_dir.x + home_dir.y * home_dir.y + home_dir.z * home_dir.z);
        if (hlen > 0.4f) {
            creature.heading = std::atan2(home_dir.x, home_dir.z);
            creature.position.x += home_dir.x / hlen * 1.6f * dt;
            creature.position.y += home_dir.y / hlen * 1.6f * dt;
            creature.position.z += home_dir.z / hlen * 1.6f * dt;
        }
    }

    if (creature.investigation_timer > 0.0f) {
        creature.investigation_timer -= dt;
    }
}

void Game::UpdatePressure(float dt) {
    float depth = -sub_.position.y;
    if (depth > 50.0f) {
        float excess = (depth - 50.0f) / 150.0f;
        sub_.battery -= excess * 0.05f * dt;
    }
    if (sub_.lights_on) sub_.battery -= 0.03f * dt;
    sub_.battery = std::max(0.0f, sub_.battery);
    if (sub_.battery <= 0.0f) sub_.lights_on = false;
    sub_.oxygen = std::max(0.0f, sub_.oxygen - (0.01f + leak_) * dt);
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
    float horiz = std::sqrt(sub_.velocity.x * sub_.velocity.x + sub_.velocity.z * sub_.velocity.z);
    if (horiz > 0.35f) sub_.heading = std::atan2(sub_.velocity.x, sub_.velocity.z);
    ApplyCurrent(dt);
    ResolveRocks();

    UpdateAcoustics(dt);
    for (CreatureAI& creature : creatures_) UpdateCreature(creature, dt);
    UpdatePressure(dt);
    UpdateCache();
    UpdateRelay();
    UpdateMission();
    sub_.sonar_active = false;
    contact_ = std::max(0.0f, contact_ - dt);
    ping_cooldown_ = std::max(0.0f, ping_cooldown_ - dt);
    impact_cooldown_ = std::max(0.0f, impact_cooldown_ - dt);
}

bool Game::InCurrent() const {
    return sub_.position.x > 18.0f && sub_.position.x < 46.0f && sub_.position.z > -16.0f && sub_.position.z < 6.0f;
}

void Game::ApplyCurrent(float dt) {
    if (!InCurrent()) return;
    // Pushes you off a straight bearing, toward +z.
    sub_.position.z += 2.6f * dt;
    sub_.position.x += 0.5f * dt;
}

void Game::UpdateCache() {
    if (cache_.taken) return;
    if (Distance(sub_.position, cache_.position) > 4.0f) return;
    cache_.taken = true;
    sub_.battery = std::min(1.0f, sub_.battery + 0.5f);
    sub_.hull = std::min(1.0f, sub_.hull + 0.25f);
    leak_ = 0.0f;
    events_.push_back({cache_.position, 0.45f, 12.0f, 1.2f});
}

void Game::ResolveRocks() {
    constexpr float kHull = 1.3f;
    for (const Rock& rock : rocks_) {
        float dx = sub_.position.x - rock.position.x;
        float dy = sub_.position.y - rock.position.y;
        float dz = sub_.position.z - rock.position.z;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float min_d = kHull + rock.radius;
        if (dist < 1e-4f) {
            sub_.position.x += min_d;
            continue;
        }
        if (dist < min_d) {
            float push = (min_d - dist) / dist;
            sub_.position.x += dx * push;
            sub_.position.y += dy * push;
            sub_.position.z += dz * push;
            sub_.velocity.x *= 0.35f;
            sub_.velocity.y *= 0.35f;
            sub_.velocity.z *= 0.35f;
            if (impact_cooldown_ <= 0.0f) {
                impact_cooldown_ = 0.7f;
                sub_.hull = std::max(0.0f, sub_.hull - 0.18f);
                leak_ = std::min(0.05f, leak_ + 0.012f);
                events_.push_back({sub_.position, 0.95f, 18.0f, 1.4f});
            }
        }
    }
}

aether::Vec3 Game::Objective() const {
    if (mission_ == Mission::Return || mission_ == Mission::Won) return haven_.position;
    return relay_.position;
}

void Game::UpdateMission() {
    for (const CreatureAI& creature : creatures_) {
        if (creature.is_alerted && Distance(sub_.position, creature.position) < 4.0f) {
            mission_ = Mission::Lost;
            fail_ = "caught";
            return;
        }
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
    if (sub_.hull <= 0.0f) {
        mission_ = Mission::Lost;
        fail_ = "hull";
        return;
    }
    if (mission_ == Mission::Outbound && relay_.restored) {
        mission_ = Mission::Return;
        bearing_valid_ = false;
    }
    if (mission_ == Mission::Return && Distance(sub_.position, haven_.position) < 8.0f) mission_ = Mission::Won;
}

void Game::UpdateRelay() {
    if (relay_.restored) return;
    bool alert = false;
    for (const CreatureAI& creature : creatures_) alert = alert || creature.is_alerted;
    float dist = Distance(sub_.position, relay_.position);
    if (dist < 6.0f && !sub_.sonar_active && !sub_.lights_on && !alert) relay_.restored = true;
}

std::string Game::StatusText() const {
    float awareness = 0.0f;
    bool alert = false;
    for (const CreatureAI& creature : creatures_) {
        awareness = std::max(awareness, creature.awareness);
        alert = alert || creature.is_alerted;
    }
    std::ostringstream oss;
    oss << "Depth: " << (-sub_.position.y)
        << "  Battery: " << int(sub_.battery * 100) << "%"
        << "  Hull: " << int(sub_.hull * 100) << "%"
        << "  Lights: " << (sub_.lights_on ? "ON" : "off")
        << "  Contact: " << int(contact_ * 10) / 10.0f << "s"
        << "  Creature awareness: " << int(awareness * 100) << "%"
        << (alert ? "  [ALERT]" : "")
        << (InCurrent() ? "  CURRENT" : "")
        << (leak_ > 0.0f ? "  LEAK" : "")
        << (!cache_.taken ? "  Cache: live" : "");
    if (mission_ == Mission::Won) oss << "  MISSION COMPLETE  " << int(time_) << "s";
    else if (mission_ == Mission::Lost) oss << "  FAILED: " << fail_ << "  (R retry)";
    else if (mission_ == Mission::Return) oss << "  Return to Haven";
    else oss << "  Relay: silent";
    return oss.str();
}

}  // namespace abyss
