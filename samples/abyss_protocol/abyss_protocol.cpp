#include "abyss_protocol.h"
#include <cmath>
#include <sstream>

namespace abyss {

namespace {
float Distance(const aether::Vec3& a, const aether::Vec3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

Game::Game() {
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

void Game::UpdateAcoustics(float dt) {
    // Continuous noise from engines / thrusters
    float continuous = 0.05f;
    if (sub_.velocity.x * sub_.velocity.x + sub_.velocity.y * sub_.velocity.y +
            sub_.velocity.z * sub_.velocity.z > 0.1f) {
        continuous += 0.25f;
    }
    if (sub_.sonar_active) {
        continuous += 0.6f;
        // Emit a sonar pulse event
        events_.push_back({sub_.position, 0.85f, 40.0f, 2.0f});
    }
    if (sub_.lights_on) {
        continuous += 0.1f;  // lights are quieter but still detectable
    }

    // Decay old events
    for (auto it = events_.begin(); it != events_.end();) {
        it->remaining_time -= dt;
        if (it->remaining_time <= 0.0f) {
            it = events_.erase(it);
        } else {
            ++it;
        }
    }

    // Simple signature for the sub
    AcousticSignature sig;
    sig.noise_level = std::min(1.0f, continuous);
    (void)sig;
}

void Game::UpdateCreature(float dt) {
    float dist = Distance(creature_.position, sub_.position);
    float noise_influence = 0.0f;

    // React to continuous noise and events
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
        // Simple move toward the sub
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
        // Drift back toward home
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
    // Depth is negative Y for underwater
    float depth = -sub_.position.y;
    if (depth > 50.0f) {
        float excess = (depth - 50.0f) / 150.0f;
        sub_.battery -= excess * 0.05f * dt;
        // Hull slowly degrades at extreme depth
        if (depth > 180.0f) {
            // placeholder damage
        }
    }
    sub_.battery = std::max(0.0f, sub_.battery);
    sub_.oxygen = std::max(0.0f, sub_.oxygen - 0.01f * dt);
}

void Game::Step(float dt) {
    // Integrate simple movement
    sub_.position.x += sub_.velocity.x * dt;
    sub_.position.y += sub_.velocity.y * dt;
    sub_.position.z += sub_.velocity.z * dt;
    // Drag
    sub_.velocity.x *= 0.95f;
    sub_.velocity.y *= 0.95f;
    sub_.velocity.z *= 0.95f;

    UpdateAcoustics(dt);
    UpdateCreature(dt);
    UpdatePressure(dt);
}

std::string Game::StatusText() const {
    std::ostringstream oss;
    oss << "Depth: " << (-sub_.position.y)
        << "  Battery: " << int(sub_.battery * 100) << "%"
        << "  Sonar: " << (sub_.sonar_active ? "ON" : "off")
        << "  Lights: " << (sub_.lights_on ? "ON" : "off")
        << "  Creature awareness: " << int(creature_.awareness * 100) << "%"
        << (creature_.is_alerted ? "  [ALERT]" : "");
    return oss.str();
}

}  // namespace abyss
