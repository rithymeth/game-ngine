#pragma once

// The systems an agent can run while the editor is in play mode: 3D physics
// (with character movement), 2D physics (with platformer controllers and
// camera follow) and previous-transform recording, on the engine's system
// scheduler and fixed-timestep frame loop.
//
// A Simulation lives from Play to Stop. Systems that need project assets or a
// window (audio, particles, sequences, UI, scripting) are not part of it; they
// need a full Game.

#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/scene/scheduler.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace aether::mcp {

class Simulation {
public:
    explicit Simulation(World& world, f32 fixed_hz = 60.0f);
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    // Runs one frame of `dt` seconds: PreUpdate, 0-N fixed steps, Update, LateUpdate, PreRender.
    FrameContext Step(f32 dt);

    struct SystemInfo {
        std::string name;
        SystemPhase phase;
        bool enabled;
    };
    // Every system in execution order, phase by phase.
    std::vector<SystemInfo> Systems();
    // False if there is no such system.
    bool SetEnabled(const std::string& name, bool enabled);

    // Input for the 2D platformer controllers (move -1..1, jump held).
    void SetPlatformerInput(f32 move, bool jump) { move_ = move; jump_ = jump; }

    // Frames and fixed steps run so far.
    u64 Frames() const { return last_.frame + (frames_run_ > 0 ? 1 : 0); }
    const FrameContext& Last() const { return last_; }
    bool HasPhysics3D() const;

private:
    struct Impl;
    World& world_;
    std::unique_ptr<Impl> impl_;
    f32 move_ = 0.0f;
    bool jump_ = false;
    FrameContext last_;
    u64 frames_run_ = 0;
};

} // namespace aether::mcp
