#include "aether/animation/animator.h"

#include "aether/ecs/component.h"
#include "aether/scene/components.h"

#include <algorithm>
#include <cmath>

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

void RegisterAnimationComponents() { (void)GetComponentId<Animator>(); }

} // namespace aether

namespace aether::anim {

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
        if (r.instance) {
            r.instance->SetRootMotion(a.root_motion);
            for (const AnimatorCommand& c : a.commands) {
                switch (c.kind) {
                case AnimatorCommand::Kind::SetFloat: r.instance->SetFloat(c.name, c.value); break;
                case AnimatorCommand::Kind::SetBool: r.instance->SetBool(c.name, c.value != 0.0f); break;
                case AnimatorCommand::Kind::SetInt: r.instance->SetInt(c.name, static_cast<i32>(std::lround(c.value))); break;
                case AnimatorCommand::Kind::SetTrigger: r.instance->SetTrigger(c.name); break;
                case AnimatorCommand::Kind::PlayMontage: {
                    const Montage* m = library_.montage ? library_.montage(c.name) : nullptr;
                    if (m == nullptr || !r.instance->PlayMontage(*m, c.value > 0.0f ? c.value : 1.0f)) {
                        problems_.push_back("entity " + std::to_string(e.index) + ": can't play the montage '" + c.name + "'");
                    } else {
                        r.montage_slot = m->slot;
                    }
                    break;
                }
                case AnimatorCommand::Kind::StopMontage: r.instance->StopMontage(r.montage_slot); break;
                case AnimatorCommand::Kind::JumpToSection: r.instance->JumpToSection(c.name, r.montage_slot); break;
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
