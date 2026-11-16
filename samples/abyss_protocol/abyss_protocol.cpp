#include "abyss_protocol.h"

namespace abyss {

Game::Game() {
    // TODO: Create entities for Haven-01 hub, research relay, one sound-sensitive creature.
    // Use ECS components for AcousticSignature, PressureState, etc.
    sub_.position = {0.0f, -10.0f, 0.0f};
}

void Game::Step(float dt) {
    // Simple placeholder movement / acoustic update.
    // Later: job-system friendly systems for acoustic propagation, AI reaction, pressure damage.
    if (sub_.sonar_active) {
        // Emit noise pulse
    }
    (void)dt;
}

}  // namespace abyss
