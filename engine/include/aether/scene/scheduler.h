#pragma once

#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/scene/components.h"

#include <functional>
#include <string>
#include <vector>

namespace aether {

// System scheduling and the fixed-timestep frame (Phase 9 step 6,
// docs/design/PHASE_SPECS.md §9.6).

enum class SystemPhase : u8 { PreUpdate, FixedUpdate, Update, LateUpdate, PreRender };
inline constexpr usize kSystemPhaseCount = 5;
const char* SystemPhaseName(SystemPhase phase);

struct FrameContext {
    f32 dt = 0.0f;       // this frame's (variable) time step, seconds
    f32 fixed_dt = 0.0f; // the fixed step
    f32 alpha = 0.0f;    // how far between the last two fixed steps render time is, [0, 1)
    u64 frame = 0;
    u64 fixed_step = 0;  // fixed steps run so far, this one included during FixedUpdate
    f64 time = 0.0;      // game time: sum of dt
};

struct SystemDesc {
    std::string name;
    SystemPhase phase = SystemPhase::Update;
    // Declared component access: systems that touch the same component, at
    // least one of them writing it, never run at the same time.
    ComponentMask reads;
    ComponentMask writes;
    std::vector<std::string> after; // systems in the same phase this one must run after, by name
    std::function<void(World&, const FrameContext&)> run;
    bool main_thread_only = false; // e.g. anything touching ImGui or the graphics device
};

template <typename... Ts>
ComponentMask Access() {
    return ComponentMaskOf<Ts...>();
}

// Orders and runs systems. Within a phase, system B runs after A if B lists
// A in `after`, or if they conflict (share a component at least one of them
// writes) and A comes first in the phase's base order: explicit `after`
// constraints first, registration order breaking ties. So only contradictory
// `after` lists can form a cycle, and Build reports those.
//
// Each phase runs as a sequence of levels: every system in a level has all
// its predecessors in earlier levels, so a level's systems can run in
// parallel. With a JobSystem they do (main_thread_only ones on the calling
// thread); without one, a phase runs in its sequential order.
class SystemScheduler {
public:
    // False (with a reason) for an empty or duplicate name or no `run`.
    bool Add(SystemDesc system, std::string* error = nullptr);
    // Builds the per-phase graphs. False on an `after` naming a system that
    // doesn't exist or is in another phase, or a cycle; the error names them.
    // Run* and Tick build automatically when needed (and assert it succeeds).
    bool Build(std::string* error = nullptr);

    // The phase's systems in a valid sequential order.
    std::vector<std::string> Order(SystemPhase phase);
    // The phase's levels (names), each runnable in parallel.
    std::vector<std::vector<std::string>> Levels(SystemPhase phase);

    void RunPhase(World& world, SystemPhase phase, const FrameContext& frame, JobSystem* jobs = nullptr);

    usize SystemCount() const { return systems_.size(); }

private:
    struct PhaseGraph {
        std::vector<u32> order;               // indices into systems_
        std::vector<std::vector<u32>> levels; // indices into systems_
    };
    std::vector<SystemDesc> systems_;
    std::vector<const char*> zone_names_; // interned in the profiler, by system
    PhaseGraph graphs_[kSystemPhaseCount];
    bool built_ = false;
};

// Fixed-timestep accumulator (§9.6): each frame's time is added up and spent
// in whole fixed steps, at most `max_steps` per frame; if more are owed (a
// long hitch), the rest is dropped rather than letting the simulation fall
// ever further behind (the "spiral of death").
class FixedTimestep {
public:
    explicit FixedTimestep(f32 fixed_dt = 1.0f / 60.0f, u32 max_steps = 5) : fixed_dt_(fixed_dt), max_steps_(max_steps) {}

    // Adds `dt` and returns how many fixed steps to run now.
    u32 Advance(f32 dt);
    // Leftover time as a fraction of a step: blend factor for rendering.
    f32 Alpha() const { return static_cast<f32>(accumulator_ / fixed_dt_); }
    f32 FixedDt() const { return fixed_dt_; }
    f64 DroppedTime() const { return dropped_; }

private:
    f32 fixed_dt_;
    u32 max_steps_;
    f64 accumulator_ = 0.0;
    f64 dropped_ = 0.0;
};

// Runs a whole frame through a scheduler: PreUpdate, FixedUpdate 0-N times,
// Update, LateUpdate, then PreRender with FrameContext::alpha set.
class FrameLoop {
public:
    FrameLoop(SystemScheduler& scheduler, FixedTimestep timestep = FixedTimestep()) : scheduler_(scheduler), timestep_(timestep) {}

    // Returns the context PreRender saw.
    FrameContext Tick(World& world, f32 dt, JobSystem* jobs = nullptr);
    const FixedTimestep& Timestep() const { return timestep_; }

private:
    SystemScheduler& scheduler_;
    FixedTimestep timestep_;
    u64 frame_ = 0;
    u64 fixed_steps_ = 0;
    f64 time_ = 0.0;
};

// Render interpolation (§9.6): entities with PreviousTransform get their
// Transform recorded before each fixed step, so the renderer can draw them
// between the last two steps with InterpolateTransform(previous, current,
// alpha) instead of snapping at the fixed rate.
struct PreviousTransform {
    Vec3 position;
    Quaternion rotation;
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Copies Transform into PreviousTransform (adding nothing: only entities that
// already have PreviousTransform are interpolated). Run it before each fixed
// step; RecordPreviousTransformsSystem() is a ready-made FixedUpdate system.
void RecordPreviousTransforms(World& world);
SystemDesc RecordPreviousTransformsSystem();

// Shortest-path normalized lerp of rotations; position lerp.
Quaternion NlerpRotation(const Quaternion& a, const Quaternion& b, f32 t);
Transform InterpolateTransform(const PreviousTransform& previous, const Transform& current, f32 alpha);

} // namespace aether

AETHER_REFLECT(aether::PreviousTransform, 2,
    AETHER_FIELD(position), AETHER_FIELD(rotation), AETHER_FIELD(scale))
