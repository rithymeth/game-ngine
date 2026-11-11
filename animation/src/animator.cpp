#include "aether/animation/animator.h"

#include "aether/ecs/component.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_set>

namespace aether {

void Animator::SetFloatParameter(const std::string& name, f32 value) { commands.push_back({AnimatorCommand::Kind::SetFloat, name, value}); }
void Animator::SetBoolParameter(const std::string& name, bool value) {
    commands.push_back({AnimatorCommand::Kind::SetBool, name, value ? 1.0f : 0.0f});
}
void Animator::SetIntParameter(const std::string& name, i32 value) {
    commands.push_back({AnimatorCommand::Kind::SetInt, name, static_cast<f32>(value)});
}
void Animator::SetTrigger(const std::string& name) { commands.push_back({AnimatorCommand::Kind::SetTrigger, name, 0.0f}); }
void Animator::PlayMontage(const std::string& montage, f32 rate) { commands.push_back({AnimatorCommand::Kind::PlayMontage, montage, rate}); }
void Animator::StopMontage() { commands.push_back({AnimatorCommand::Kind::StopMontage, {}, 0.0f}); }
void Animator::JumpToSection(const std::string& section) { commands.push_back({AnimatorCommand::Kind::JumpToSection, section, 0.0f}); }

void RegisterAnimationComponents() {
    (void)GetComponentId<Animator>();
    (void)GetComponentId<AnimationLookAt>();
    (void)GetComponentId<AnimationTwoBoneIK>();
    (void)GetComponentId<AnimationFootPlacement>();
}

} // namespace aether

namespace aether::anim {

namespace {

bool SameFootSettings(const FootPlacementSettings& a, const FootPlacementSettings& b) {
    if (a.pelvis != b.pelvis || a.max_step_up != b.max_step_up || a.max_step_down != b.max_step_down ||
        a.max_foot_angle != b.max_foot_angle || a.smoothing != b.smoothing || a.legs.size() != b.legs.size()) return false;
    for (usize i = 0; i < a.legs.size(); ++i) {
        const LegSetup& x = a.legs[i];
        const LegSetup& y = b.legs[i];
        if (x.hip != y.hip || x.knee != y.knee || x.ankle != y.ankle || x.foot_height != y.foot_height) return false;
    }
    return true;
}

} // namespace

struct AnimationSystem::Runtime {
    Entity entity;
    std::string graph_name, skeleton_name;
    const AnimGraph* graph = nullptr;
    const Skeleton* skeleton = nullptr;
    std::unique_ptr<AnimGraphInstance> instance;
    Pose pose;
    std::vector<Mat4> model, skin;
    f32 dt = 0.0f;
    bool root_motion = false;
    LookAt look_at;
    bool apply_look_at = false;
    std::string look_at_problem;
    std::vector<TwoBoneIK> two_bone_ik;
    std::vector<std::string> two_bone_problems;
    FootPlacementSettings foot_settings;
    std::unique_ptr<FootPlacer> foot_placer;
    GroundTrace foot_trace;
    bool apply_foot_placement = false;
    std::string foot_problem;
    std::unordered_set<std::string> command_problems;
    std::string montage_slot = "Default"; // of the last montage it played
    u64 seen = 0;
};

AnimationSystem::AnimationSystem(World& world, AnimationLibrary library) : world_(world), library_(std::move(library)) {
    RegisterAnimationComponents();
}

AnimationSystem::~AnimationSystem() = default;

const char* AnimationSystem::BlueprintEventName(AnimEventType type) {
    switch (type) {
    case AnimEventType::Notify: return "Event.OnAnimNotify";
    case AnimEventType::NotifyBegin: return "Event.OnAnimNotifyBegin";
    case AnimEventType::NotifyEnd: return "Event.OnAnimNotifyEnd";
    case AnimEventType::StateChanged: return "Event.OnAnimStateChanged";
    case AnimEventType::MontageStarted: return "Event.OnMontageStarted";
    case AnimEventType::MontageSectionChanged: return "Event.OnMontageSectionChanged";
    case AnimEventType::MontageBlendingOut: return "Event.OnMontageBlendingOut";
    case AnimEventType::MontageEnded: return "Event.OnMontageEnded";
    }
    return "";
}

void AnimationSystem::Update(f32 dt, JobSystem* jobs) {
    const u64 now = ++generation_;
    events_.clear();

    // Find the Animators, (re)building runtimes whose assets changed, and apply their commands.
    std::vector<Entity> entities;
    world_.ForEachArchetype([&](Archetype& archetype) {
        if (!archetype.Mask().test(GetComponentId<Animator>())) return;
        for (usize c = 0; c < archetype.ChunkCount(); ++c) {
            Entity* e = archetype.EntityArray(c);
            entities.insert(entities.end(), e, e + archetype.ChunkEntityCount(c));
        }
    });
    std::sort(entities.begin(), entities.end(), [](Entity a, Entity b) { return a.index < b.index; });
    std::vector<Runtime*> active;
    for (Entity e : entities) {
        Animator& a = *world_.GetComponent<Animator>(e);
        auto& slot = runtimes_[e.index];
        if (!slot || slot->entity != e || slot->graph_name != a.graph || slot->skeleton_name != a.skeleton) {
            slot = std::make_unique<Runtime>();
            slot->entity = e;
            slot->graph_name = a.graph;
            slot->skeleton_name = a.skeleton;
            slot->graph = library_.graph ? library_.graph(a.graph) : nullptr;
            slot->skeleton = library_.skeleton ? library_.skeleton(a.skeleton) : nullptr;
            const std::string who = "entity " + std::to_string(e.index) + ": ";
            if (slot->graph == nullptr) problems_.push_back(who + "no animation graph named '" + a.graph + "'");
            if (slot->skeleton == nullptr) problems_.push_back(who + "no skeleton named '" + a.skeleton + "'");
            if (slot->graph != nullptr) {
                for (const AnimDiagnostic& d : ValidateAnimGraph(*slot->graph, &library_.assets)) {
                    if (d.error) problems_.push_back(who + d.code + ": " + d.message);
                }
            }
            if (slot->graph != nullptr && slot->skeleton != nullptr) {
                slot->instance = std::make_unique<AnimGraphInstance>(*slot->graph, *slot->skeleton, library_.assets);
            }
        }
        Runtime& r = *slot;
        r.seen = now;
        r.dt = a.paused ? 0.0f : dt * std::max(a.speed, 0.0f);
        r.root_motion = a.root_motion;
        r.apply_look_at = false;
        r.two_bone_ik.clear();
        r.apply_foot_placement = false;
        if (const AnimationLookAt* constraint = world_.GetComponent<AnimationLookAt>(e);
            constraint != nullptr && constraint->enabled && constraint->weight > 0.0f && r.skeleton != nullptr) {
            const i32 bone = r.skeleton->Find(constraint->bone);
            if (bone < 0) {
                const std::string problem = "entity " + std::to_string(e.index) + ": no look-at bone named '" + constraint->bone + "'";
                if (r.look_at_problem != problem) {
                    problems_.push_back(problem);
                    r.look_at_problem = problem;
                }
            } else {
                r.look_at_problem.clear();
                r.look_at.bone = bone;
                r.look_at.aim_axis = constraint->aim_axis;
                r.look_at.max_angle = constraint->max_angle;
                r.look_at.weight = constraint->weight;
                r.look_at.target = constraint->target;
                if (const Transform* transform = world_.GetComponent<Transform>(e); transform != nullptr) {
                    r.look_at.target = Rotate(Conjugate(transform->rotation), constraint->target - transform->position);
                }
                r.apply_look_at = true;
            }
        }
        if (const AnimationTwoBoneIK* constraints = world_.GetComponent<AnimationTwoBoneIK>(e);
            constraints != nullptr && constraints->enabled && r.skeleton != nullptr) {
            const Transform* transform = world_.GetComponent<Transform>(e);
            for (const AnimationTwoBoneIKChain& chain : constraints->chains) {
                const i32 root = r.skeleton->Find(chain.root);
                const i32 mid = r.skeleton->Find(chain.mid);
                const i32 end = r.skeleton->Find(chain.end);
                const bool is_chain = root >= 0 && mid >= 0 && end >= 0 &&
                                      r.skeleton->bones[static_cast<usize>(mid)].parent == root &&
                                      r.skeleton->bones[static_cast<usize>(end)].parent == mid;
                if (!is_chain) {
                    const std::string problem = "entity " + std::to_string(e.index) + ": invalid two-bone IK chain '" +
                                                chain.root + "' -> '" + chain.mid + "' -> '" + chain.end + "'";
                    if (std::find(r.two_bone_problems.begin(), r.two_bone_problems.end(), problem) == r.two_bone_problems.end()) {
                        problems_.push_back(problem);
                        r.two_bone_problems.push_back(problem);
                    }
                    continue;
                }
                TwoBoneIK ik;
                ik.root = root;
                ik.mid = mid;
                ik.end = end;
                ik.target = chain.target;
                ik.pole = chain.pole;
                ik.use_pole = chain.use_pole;
                ik.weight = chain.weight;
                if (transform != nullptr) {
                    const Quaternion inverse = Conjugate(transform->rotation);
                    ik.target = Rotate(inverse, chain.target - transform->position);
                    ik.pole = Rotate(inverse, chain.pole - transform->position);
                }
                r.two_bone_ik.push_back(ik);
            }
        }
        if (const AnimationFootPlacement* placement = world_.GetComponent<AnimationFootPlacement>(e);
            placement != nullptr && placement->enabled && r.skeleton != nullptr) {
            FootPlacementSettings settings;
            settings.pelvis = r.skeleton->Find(placement->pelvis);
            settings.max_step_up = placement->max_step_up;
            settings.max_step_down = placement->max_step_down;
            settings.max_foot_angle = placement->max_foot_angle;
            settings.smoothing = placement->smoothing;
            bool valid = settings.pelvis >= 0;
            for (const AnimationFootPlacementLeg& leg : placement->legs) {
                LegSetup resolved;
                resolved.hip = r.skeleton->Find(leg.hip);
                resolved.knee = r.skeleton->Find(leg.knee);
                resolved.ankle = r.skeleton->Find(leg.ankle);
                resolved.foot_height = leg.foot_height;
                valid = valid && resolved.hip >= 0 && resolved.knee >= 0 && resolved.ankle >= 0;
                settings.legs.push_back(resolved);
            }
            const std::string problem = "entity " + std::to_string(e.index) + ": invalid foot-placement bones";
            if (!valid || settings.legs.empty()) {
                if (r.foot_problem != problem) problems_.push_back(problem);
                r.foot_problem = problem;
            } else if (!library_.ground_trace) {
                const std::string no_trace = "entity " + std::to_string(e.index) + ": foot placement needs an AnimationLibrary ground trace";
                if (r.foot_problem != no_trace) problems_.push_back(no_trace);
                r.foot_problem = no_trace;
            } else {
                r.foot_problem.clear();
                if (!r.foot_placer || !SameFootSettings(r.foot_settings, settings)) {
                    r.foot_settings = settings;
                    r.foot_placer = std::make_unique<FootPlacer>(settings);
                }
                const Transform* transform = world_.GetComponent<Transform>(e);
                const Transform local = transform != nullptr ? *transform : Transform{};
                r.foot_trace = [this, entity = e, local, has_transform = transform != nullptr](const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal) {
                    Vec3 query_from = from, query_to = to, world_hit, world_normal;
                    if (has_transform) {
                        query_from = local.position + Rotate(local.rotation, from);
                        query_to = local.position + Rotate(local.rotation, to);
                    }
                    if (!library_.ground_trace(entity, query_from, query_to, world_hit, world_normal)) return false;
                    if (has_transform) {
                        const Quaternion inverse = Conjugate(local.rotation);
                        hit = Rotate(inverse, world_hit - local.position);
                        normal = Rotate(inverse, world_normal);
                    } else {
                        hit = world_hit;
                        normal = world_normal;
                    }
                    return true;
                };
                r.apply_foot_placement = true;
            }
        }
        if (r.instance) {
            r.instance->SetRootMotion(a.root_motion);
            for (const AnimatorCommand& c : a.commands) {
                const auto report_command_problem = [&](const std::string& message) {
                    const std::string problem = "entity " + std::to_string(e.index) + ": " + message;
                    if (r.command_problems.insert(problem).second) problems_.push_back(problem);
                };
                bool accepted = true;
                const char* expected = "parameter";
                switch (c.kind) {
                case AnimatorCommand::Kind::SetFloat:
                    expected = "float or int";
                    accepted = r.instance->SetFloat(c.name, c.value);
                    break;
                case AnimatorCommand::Kind::SetBool:
                    expected = "bool";
                    accepted = r.instance->SetBool(c.name, c.value != 0.0f);
                    break;
                case AnimatorCommand::Kind::SetInt:
                    expected = "int";
                    accepted = r.instance->SetInt(c.name, static_cast<i32>(std::lround(c.value)));
                    break;
                case AnimatorCommand::Kind::SetTrigger:
                    expected = "trigger";
                    accepted = r.instance->SetTrigger(c.name);
                    break;
                case AnimatorCommand::Kind::PlayMontage: {
                    const Montage* m = library_.montage ? library_.montage(c.name) : nullptr;
                    if (m == nullptr || !r.instance->PlayMontage(*m, c.value > 0.0f ? c.value : 1.0f)) {
                        report_command_problem("can't play the montage '" + c.name + "'");
                    } else {
                        r.montage_slot = m->slot;
                    }
                    break;
                }
                case AnimatorCommand::Kind::StopMontage: r.instance->StopMontage(r.montage_slot); break;
                case AnimatorCommand::Kind::JumpToSection:
                    if (!r.instance->JumpToSection(c.name, r.montage_slot)) {
                        report_command_problem("can't jump to section '" + c.name + "' in montage slot '" + r.montage_slot + "'");
                    }
                    break;
                }
                if (!accepted && c.kind <= AnimatorCommand::Kind::SetTrigger) {
                    report_command_problem("animation parameter '" + c.name + "' is missing or is not a " + expected);
                }
            }
            active.push_back(&r);
        }
        a.commands.clear();
    }
    // Entities gone (or without an Animator any more): drop their runtimes.
    for (auto it = runtimes_.begin(); it != runtimes_.end();) it = it->second->seen == now ? std::next(it) : runtimes_.erase(it);

    // Evaluate: each character's graph is independent, so they run in parallel.
    auto evaluate = [](Runtime& r) {
        r.instance->Update(r.dt, r.pose);
        for (const TwoBoneIK& ik : r.two_bone_ik) SolveTwoBoneIK(*r.skeleton, r.pose, ik);
        if (r.apply_look_at) SolveLookAt(*r.skeleton, r.pose, r.look_at);
        LocalToModel(*r.skeleton, r.pose, r.model);
        SkinMatrices(*r.skeleton, r.model, r.skin);
    };
    if (jobs != nullptr && active.size() > 1) {
        struct Work {
            Runtime* runtime;
            void (*run)(Runtime&);
        };
        std::vector<Work> work;
        work.reserve(active.size());
        for (Runtime* r : active) work.push_back({r, +evaluate});
        std::vector<JobDecl> decls;
        for (Work& w : work) decls.push_back({[](void* d) { auto* x = static_cast<Work*>(d); x->run(*x->runtime); }, &w});
        JobCounter counter{0};
        jobs->ScheduleBatch(decls.data(), static_cast<u32>(decls.size()), counter);
        jobs->Wait(counter);
    } else {
        for (Runtime* r : active) evaluate(*r);
    }
    // Ground providers often query physics or navigation data. Keep that work
    // on this thread; graph sampling and non-ground IK above remain parallel.
    for (Runtime* r : active) {
        if (!r->apply_foot_placement) continue;
        r->foot_placer->Apply(*r->skeleton, r->pose, r->foot_trace, r->dt);
        LocalToModel(*r->skeleton, r->pose, r->model);
        SkinMatrices(*r->skeleton, r->model, r->skin);
    }
    for (Runtime* r : active) {
        for (const AnimEvent& ev : r->instance->Events()) events_.push_back({r->entity, ev});
    }
}

AnimGraphInstance* AnimationSystem::InstanceOf(Entity e) {
    auto it = runtimes_.find(e.index);
    return it != runtimes_.end() && it->second->entity == e ? it->second->instance.get() : nullptr;
}

const Pose* AnimationSystem::PoseOf(Entity e) const {
    auto it = runtimes_.find(e.index);
    return it != runtimes_.end() && it->second->entity == e && it->second->instance ? &it->second->pose : nullptr;
}

const std::vector<Mat4>* AnimationSystem::SkinMatricesOf(Entity e) const {
    auto it = runtimes_.find(e.index);
    return it != runtimes_.end() && it->second->entity == e && it->second->instance ? &it->second->skin : nullptr;
}

bool AnimationSystem::RootMotionOf(Entity e, Vec3& world_translation, f32& yaw) const {
    auto it = runtimes_.find(e.index);
    if (it == runtimes_.end() || it->second->entity != e || !it->second->instance || !it->second->root_motion) return false;
    const RootMotionDelta& d = it->second->instance->RootMotion();
    const Transform* t = world_.GetComponent<Transform>(e);
    world_translation = t != nullptr ? Rotate(t->rotation, d.translation) : d.translation;
    yaw = 2.0f * std::atan2(d.rotation.y, d.rotation.w);
    return true;
}

} // namespace aether::anim
