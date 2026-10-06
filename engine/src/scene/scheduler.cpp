#include "aether/scene/scheduler.h"

#include "aether/core/log.h"
#include "aether/core/profile.h"

#include <algorithm>
#include <unordered_map>

namespace aether {

const char* SystemPhaseName(SystemPhase phase) {
    switch (phase) {
    case SystemPhase::PreUpdate: return "PreUpdate";
    case SystemPhase::FixedUpdate: return "FixedUpdate";
    case SystemPhase::Update: return "Update";
    case SystemPhase::LateUpdate: return "LateUpdate";
    case SystemPhase::PreRender: return "PreRender";
    }
    return "?";
}

namespace {

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool Conflicts(const SystemDesc& a, const SystemDesc& b) {
    return (a.writes & (b.reads | b.writes)).any() || (b.writes & a.reads).any();
}

} // namespace

bool SystemScheduler::Add(SystemDesc system, std::string* error) {
    if (system.name.empty() || !system.run) {
        SetError(error, "A system needs a name and a run function");
        return false;
    }
    for (const SystemDesc& existing : systems_) {
        if (existing.name == system.name) {
            SetError(error, "There's already a system named \"" + system.name + "\"");
            return false;
        }
    }
    systems_.push_back(std::move(system));
    built_ = false;
    return true;
}

bool SystemScheduler::Build(std::string* error) {
    std::unordered_map<std::string, u32> by_name;
    for (u32 i = 0; i < systems_.size(); ++i) {
        by_name[systems_[i].name] = i;
    }
    for (usize p = 0; p < kSystemPhaseCount; ++p) {
        const SystemPhase phase = static_cast<SystemPhase>(p);
        std::vector<u32> members;
        for (u32 i = 0; i < systems_.size(); ++i) {
            if (systems_[i].phase == phase) {
                members.push_back(i);
            }
        }
        // Explicit edges.
        std::unordered_map<u32, std::vector<u32>> must_follow; // system -> systems it runs after
        for (u32 i : members) {
            for (const std::string& name : systems_[i].after) {
                auto it = by_name.find(name);
                if (it == by_name.end()) {
                    SetError(error, "System \"" + systems_[i].name + "\" runs after \"" + name + "\", which doesn't exist");
                    return false;
                }
                if (systems_[it->second].phase != phase) {
                    SetError(error, "System \"" + systems_[i].name + "\" (" + SystemPhaseName(phase) + ") runs after \"" +
                                        name + "\", which is in " + SystemPhaseName(systems_[it->second].phase));
                    return false;
                }
                must_follow[i].push_back(it->second);
            }
        }
        // Base order: topological by explicit edges, registration order first.
        std::vector<u32> base;
        std::vector<bool> placed(systems_.size(), false);
        while (base.size() < members.size()) {
            bool progressed = false;
            for (u32 i : members) {
                if (placed[i]) {
                    continue;
                }
                const auto& deps = must_follow[i];
                if (std::all_of(deps.begin(), deps.end(), [&](u32 d) { return placed[d]; })) {
                    base.push_back(i);
                    placed[i] = true;
                    progressed = true;
                    break; // restart: keep registration order as the tie-break
                }
            }
            if (!progressed) {
                std::string names;
                for (u32 i : members) {
                    if (!placed[i]) {
                        names += (names.empty() ? "" : ", ") + systems_[i].name;
                    }
                }
                SetError(error, std::string("Systems in ") + SystemPhaseName(phase) + " form a cycle through their \"after\" lists: " + names);
                return false;
            }
        }
        // Levels: explicit edges plus conflicts directed by the base order.
        std::unordered_map<u32, u32> level_of;
        std::vector<std::vector<u32>> levels;
        for (usize k = 0; k < base.size(); ++k) {
            const u32 i = base[k];
            u32 level = 0;
            for (usize j = 0; j < k; ++j) {
                const u32 earlier = base[j];
                const auto& deps = must_follow[i];
                const bool explicit_edge = std::find(deps.begin(), deps.end(), earlier) != deps.end();
                if (explicit_edge || Conflicts(systems_[earlier], systems_[i])) {
                    level = std::max(level, level_of[earlier] + 1);
                }
            }
            level_of[i] = level;
            if (levels.size() <= level) {
                levels.resize(level + 1);
            }
            levels[level].push_back(i);
        }
        std::vector<u32> order;
        for (const auto& level : levels) {
            order.insert(order.end(), level.begin(), level.end());
        }
        graphs_[p] = {std::move(order), std::move(levels)};
    }
    built_ = true;
    return true;
}

std::vector<std::string> SystemScheduler::Order(SystemPhase phase) {
    if (!built_) {
        Build();
    }
    std::vector<std::string> names;
    for (u32 i : graphs_[static_cast<usize>(phase)].order) {
        names.push_back(systems_[i].name);
    }
    return names;
}

std::vector<std::vector<std::string>> SystemScheduler::Levels(SystemPhase phase) {
    if (!built_) {
        Build();
    }
    std::vector<std::vector<std::string>> result;
    for (const auto& level : graphs_[static_cast<usize>(phase)].levels) {
        result.emplace_back();
        for (u32 i : level) {
            result.back().push_back(systems_[i].name);
        }
    }
    return result;
}

namespace {

struct SystemJob {
    const SystemDesc* system;
    World* world;
    const FrameContext* frame;
};

void RunSystemJob(void* data) {
    const SystemJob* job = static_cast<const SystemJob*>(data);
    AETHER_ZONE_DYNAMIC(job->system->name.c_str());
    job->system->run(*job->world, *job->frame);
}

} // namespace

void SystemScheduler::RunPhase(World& world, SystemPhase phase, const FrameContext& frame, JobSystem* jobs) {
    if (!built_) {
        std::string error;
        const bool ok = Build(&error);
        if (!ok) {
            AETHER_LOG_ERROR("Scheduler", "%s", error.c_str());
        }
        AETHER_ASSERT(ok);
    }
    const PhaseGraph& graph = graphs_[static_cast<usize>(phase)];
    if (jobs == nullptr) {
        for (u32 i : graph.order) {
            AETHER_ZONE_DYNAMIC(systems_[i].name.c_str());
            systems_[i].run(world, frame);
        }
        return;
    }
    std::vector<SystemJob> batch;
    std::vector<JobDecl> decls;
    for (const auto& level : graph.levels) {
        batch.clear();
        decls.clear();
        for (u32 i : level) {
            if (!systems_[i].main_thread_only) {
                batch.push_back({&systems_[i], &world, &frame});
            }
        }
        for (SystemJob& job : batch) {
            decls.push_back({&RunSystemJob, &job});
        }
        JobCounter counter{0};
        if (!decls.empty()) {
            jobs->ScheduleBatch(decls.data(), static_cast<u32>(decls.size()), counter);
        }
        for (u32 i : level) { // the calling thread's share, meanwhile
            if (systems_[i].main_thread_only) {
                AETHER_ZONE_DYNAMIC(systems_[i].name.c_str());
                systems_[i].run(world, frame);
            }
        }
        jobs->Wait(counter);
    }
}

u32 FixedTimestep::Advance(f32 dt) {
    if (dt > 0.0f) {
        accumulator_ += dt;
    }
    u32 steps = 0;
    while (accumulator_ >= fixed_dt_ && steps < max_steps_) {
        accumulator_ -= fixed_dt_;
        ++steps;
    }
    if (accumulator_ >= fixed_dt_) {
        // Too far behind: keep only the fraction of a step.
        const f64 keep = std::fmod(accumulator_, static_cast<f64>(fixed_dt_));
        dropped_ += accumulator_ - keep;
        accumulator_ = keep;
    }
    return steps;
}

FrameContext FrameLoop::Tick(World& world, f32 dt, JobSystem* jobs) {
    FrameContext frame;
    frame.dt = dt;
    frame.fixed_dt = timestep_.FixedDt();
    frame.frame = frame_++;
    time_ += dt > 0.0f ? dt : 0.0f;
    frame.time = time_;
    frame.fixed_step = fixed_steps_;

    scheduler_.RunPhase(world, SystemPhase::PreUpdate, frame, jobs);
    const u32 steps = timestep_.Advance(dt);
    for (u32 s = 0; s < steps; ++s) {
        FrameContext fixed = frame;
        fixed.dt = timestep_.FixedDt();
        fixed.fixed_step = ++fixed_steps_;
        scheduler_.RunPhase(world, SystemPhase::FixedUpdate, fixed, jobs);
    }
    frame.fixed_step = fixed_steps_;
    scheduler_.RunPhase(world, SystemPhase::Update, frame, jobs);
    scheduler_.RunPhase(world, SystemPhase::LateUpdate, frame, jobs);
    frame.alpha = timestep_.Alpha();
    scheduler_.RunPhase(world, SystemPhase::PreRender, frame, jobs);
    return frame;
}

void RecordPreviousTransforms(World& world) {
    world.ForEach<Transform, PreviousTransform>([](const Transform& t, PreviousTransform& previous) {
        previous.position = t.position;
        previous.rotation = t.rotation;
    });
}

SystemDesc RecordPreviousTransformsSystem() {
    SystemDesc system;
    system.name = "RecordPreviousTransforms";
    system.phase = SystemPhase::FixedUpdate;
    system.reads = Access<Transform>();
    system.writes = Access<PreviousTransform>();
    system.run = [](World& world, const FrameContext&) { RecordPreviousTransforms(world); };
    return system;
}

Quaternion NlerpRotation(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 sign = dot < 0.0f ? -1.0f : 1.0f; // take the short way round
    return Quaternion(a.x + (sign * b.x - a.x) * t, a.y + (sign * b.y - a.y) * t, a.z + (sign * b.z - a.z) * t,
                      a.w + (sign * b.w - a.w) * t)
        .Normalized();
}

Transform InterpolateTransform(const PreviousTransform& previous, const Transform& current, f32 alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    Transform result;
    result.position = previous.position + (current.position - previous.position) * alpha;
    result.rotation = NlerpRotation(previous.rotation, current.rotation, alpha);
    return result;
}

} // namespace aether
