#pragma once

#include "aether/animation/anim_graph.h"
#include "aether/ecs/world.h"
#include "aether/job/job_system.h"
#include "aether/reflection/reflection.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether {

// A request made from gameplay or Blueprints, applied at the start of the
// next AnimationSystem::Update.
struct AnimatorCommand {
    enum class Kind : u8 { SetFloat, SetBool, SetInt, SetTrigger, PlayMontage, StopMontage, JumpToSection };
    Kind kind = Kind::SetFloat;
    std::string name;
    f32 value = 0.0f;
};

// Plays an animation graph on an entity (Phase 16 step 4,
// docs/design/PHASE_SPECS.md §16.5). The methods are Blueprint nodes (Set
// Float Parameter, Play Montage, ...); they queue commands the animation
// system applies before it evaluates.
struct Animator {
    std::string graph;    // the animation graph asset
    std::string skeleton; // the skeleton asset
    bool root_motion = false; // play in place and move the character by the animation
    f32 speed = 1.0f;
    bool paused = false;
    std::vector<AnimatorCommand> commands; // not saved

    void SetFloatParameter(const std::string& name, f32 value);
    void SetBoolParameter(const std::string& name, bool value);
    void SetIntParameter(const std::string& name, i32 value);
    void SetTrigger(const std::string& name);
    void PlayMontage(const std::string& montage, f32 rate);
    void StopMontage();
    void JumpToSection(const std::string& section);
};

// Registers Animator with the ECS (idempotent), so scenes load it.
void RegisterAnimationComponents();

} // namespace aether

namespace aether::anim {

// Where the Animator's asset names lead.
struct AnimationLibrary {
    std::function<const AnimGraph*(const std::string&)> graph;
    std::function<const Skeleton*(const std::string&)> skeleton;
    std::function<const Montage*(const std::string&)> montage;
    AnimAssets assets; // clips and blend spaces
};

struct EntityAnimEvent {
    Entity entity;
    AnimEvent event;
};

// Runs every Animator: applies its commands, advances its graph (in
// parallel jobs when given a JobSystem), and keeps each entity's pose, skin
// matrices, root motion and events from the last Update.
class AnimationSystem {
public:
    AnimationSystem(World& world, AnimationLibrary library);
    ~AnimationSystem();

    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    void Update(f32 dt, JobSystem* jobs = nullptr);

    usize Count() const { return runtimes_.size(); }
    AnimGraphInstance* InstanceOf(Entity entity);
    const Pose* PoseOf(Entity entity) const;
    const std::vector<Mat4>* SkinMatricesOf(Entity entity) const;
    // The last Update's root motion in world space (the animation's motion
    // turned by the entity's rotation) and its turn about +Y, for
    // CharacterMovement::AddRootMotion. False without root motion.
    bool RootMotionOf(Entity entity, Vec3& world_translation, f32& yaw) const;
    // Every Animator's events from the last Update, in entity order.
    const std::vector<EntityAnimEvent>& Events() const { return events_; }
    // Missing assets and invalid graphs, reported once per entity.
    const std::vector<std::string>& Problems() const { return problems_; }

    // The Blueprint event an animation event is dispatched as, and its
    // arguments in the event's pin order: Notify/Begin/End (name);
    // StateChanged (machine, from, to); MontageStarted (montage);
    // MontageSectionChanged (montage, section); MontageBlendingOut and
    // MontageEnded (montage, interrupted).
    static const char* BlueprintEventName(AnimEventType type);

private:
    struct Runtime;
    World& world_;
    AnimationLibrary library_;
    std::map<u32, std::unique_ptr<Runtime>> runtimes_; // by entity index
    std::vector<EntityAnimEvent> events_;
    std::vector<std::string> problems_;
    u64 generation_ = 0;
};

} // namespace aether::anim

AETHER_REFLECT(aether::Animator, 1,
    AETHER_FIELD(graph, Field_EditAnywhere, {.tooltip = "The animation graph (.aanim)"}),
    AETHER_FIELD(skeleton, Field_EditAnywhere, {.tooltip = "The skeleton the graph animates"}),
    AETHER_FIELD(root_motion, Field_EditAnywhere, {.tooltip = "Move the character by the animation's root motion"}),
    AETHER_FIELD(speed, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(paused, Field_EditAnywhere),
    AETHER_METHOD(SetFloatParameter, Fn_BlueprintCallable, {"name", "value"}),
    AETHER_METHOD(SetBoolParameter, Fn_BlueprintCallable, {"name", "value"}),
    AETHER_METHOD(SetIntParameter, Fn_BlueprintCallable, {"name", "value"}),
    AETHER_METHOD(SetTrigger, Fn_BlueprintCallable, {"name"}),
    AETHER_METHOD(PlayMontage, Fn_BlueprintCallable, {"montage", "rate"}),
    AETHER_METHOD(StopMontage, Fn_BlueprintCallable),
    AETHER_METHOD(JumpToSection, Fn_BlueprintCallable, {"section"})
)
