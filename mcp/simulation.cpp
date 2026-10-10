#include "simulation.h"

#include "aether/sprite2d/physics2d.h"
#include "aether/sprite2d/platformer.h"

#if AETHER_MCP_PHYSICS
#include "aether/physics/character.h"
#include "aether/physics/physics_scene.h"
#include "aether/physics/physics_world.h"
#endif

namespace aether::mcp {

struct Simulation::Impl {
    SystemScheduler scheduler;
    std::unique_ptr<FrameLoop> loop;
    std::set<std::string> disabled;
    std::vector<std::pair<std::string, SystemPhase>> names;
    JobSystem jobs{1};
#if AETHER_MCP_PHYSICS
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<PhysicsScene> scene;
    std::unique_ptr<CharacterSystem> characters;
#endif
    std::unique_ptr<sprite2d::Physics2D> physics2d;
};

Simulation::Simulation(World& world, f32 fixed_hz) : world_(world), impl_(std::make_unique<Impl>()) {
    Impl& im = *impl_;
    auto add = [&im](SystemDesc desc) {
        // Disabled systems are skipped by name, so the toggle needs no rebuild.
        auto run = std::move(desc.run);
        desc.run = [&im, name = desc.name, run = std::move(run)](World& w, const FrameContext& f) {
            if (im.disabled.count(name) == 0) run(w, f);
        };
        desc.main_thread_only = true;
        im.names.emplace_back(desc.name, desc.phase);
        im.scheduler.Add(std::move(desc));
    };

    SystemDesc previous = RecordPreviousTransformsSystem();
    previous.name = "Engine.PreviousTransforms";
    add(std::move(previous));

    std::vector<std::string> physics_systems;
#if AETHER_MCP_PHYSICS
    im.physics = std::make_unique<PhysicsWorld>(im.jobs);
    im.scene = std::make_unique<PhysicsScene>(world, *im.physics);
    im.characters = std::make_unique<CharacterSystem>(world, *im.physics, im.scene.get());
    im.scene->Sync();
    SystemDesc physics;
    physics.name = "Physics3D";
    physics.phase = SystemPhase::FixedUpdate;
    physics.after = {"Engine.PreviousTransforms"};
    physics.run = [&im](World&, const FrameContext& frame) {
        im.characters->Step(frame.fixed_dt);
        im.scene->Step(frame.fixed_dt);
    };
    add(std::move(physics));
    physics_systems.push_back("Physics3D");
#endif

    im.physics2d = std::make_unique<sprite2d::Physics2D>(world);
    SystemDesc physics2d;
    physics2d.name = "Physics2D";
    physics2d.phase = SystemPhase::FixedUpdate;
    physics2d.after = physics_systems;
    physics2d.run = [&im, this](World& w, const FrameContext& frame) {
        w.ForEach<sprite2d::PlatformerController2D>([&](sprite2d::PlatformerController2D& pc) {
            pc.input_move = move_;
            pc.input_jump = jump_;
        });
        sprite2d::UpdatePlatformers(w, *im.physics2d, frame.fixed_dt);
        im.physics2d->Step(frame.fixed_dt);
        sprite2d::UpdateCameraFollow2D(w, frame.fixed_dt);
    };
    add(std::move(physics2d));

    std::string error;
    im.scheduler.Build(&error);
    im.loop = std::make_unique<FrameLoop>(im.scheduler, FixedTimestep(1.0f / fixed_hz));
}

Simulation::~Simulation() = default;

FrameContext Simulation::Step(f32 dt) {
    // The world is the editor's own; entities added since the last frame get bodies first.
#if AETHER_MCP_PHYSICS
    impl_->scene->Sync();
#endif
    last_ = impl_->loop->Tick(world_, dt);
    ++frames_run_;
    return last_;
}

std::vector<Simulation::SystemInfo> Simulation::Systems() {
    std::vector<SystemInfo> out;
    for (usize p = 0; p < kSystemPhaseCount; ++p) {
        const SystemPhase phase = static_cast<SystemPhase>(p);
        for (const std::string& name : impl_->scheduler.Order(phase)) {
            out.push_back({name, phase, impl_->disabled.count(name) == 0});
        }
    }
    return out;
}

bool Simulation::SetEnabled(const std::string& name, bool enabled) {
    for (const auto& n : impl_->names) {
        if (n.first == name) {
            if (enabled) impl_->disabled.erase(name);
            else impl_->disabled.insert(name);
            return true;
        }
    }
    return false;
}

bool Simulation::HasPhysics3D() const {
#if AETHER_MCP_PHYSICS
    return true;
#else
    return false;
#endif
}

} // namespace aether::mcp
