#pragma once

#include "aether/animation/anim_graph.h"
#include "aether/animation/ik.h"
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
// a retired planning doc). The methods are Blueprint nodes (Set
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

// A procedural aim pass applied after an Animator has evaluated its graph.
// `target` is in world space when the entity has a Transform (otherwise it is
// model space). This keeps authored graph poses intact while allowing a
// character, turret, or NPC to follow a gameplay target.
struct AnimationLookAt {
    std::string bone;
    Vec3 target{0, 0, 0};
    Vec3 aim_axis{0, 0, 1}; // local bone axis that faces the target
    f32 max_angle = 1.2f;   // radians from the graph pose
    f32 weight = 1.0f;
    bool enabled = true;
};

// One arm or leg in a procedural IK pass. Targets and poles are world space
// when the owning entity has a Transform (otherwise they are model space).
struct AnimationTwoBoneIKChain {
    std::string root;
    std::string mid;
    std::string end;
    Vec3 target{0, 0, 0};
    Vec3 pole{0, 0, 0};
    bool use_pole = true;
    f32 weight = 1.0f;
};

// A post-graph collection of arm/leg reach constraints. One component can
// drive both sides of a character or several limbs on a procedural NPC.
struct AnimationTwoBoneIK {
    std::vector<AnimationTwoBoneIKChain> chains;
    bool enabled = true;
};

// A leg used by the post-graph ground-placement pass.
struct AnimationFootPlacementLeg {
    std::string hip;
    std::string knee;
    std::string ankle;
    f32 foot_height = 0.1f;
};

// Keeps animated feet on uneven terrain. Ground queries come from the
// AnimationLibrary, which lets physics, navigation, or a procedural world
// provide terrain without coupling animation to one of those modules.
struct AnimationFootPlacement {
    std::string pelvis;
    std::vector<AnimationFootPlacementLeg> legs;
    f32 max_step_up = 0.5f;
    f32 max_step_down = 0.5f;
    f32 max_foot_angle = 0.8f;
    f32 smoothing = 0.0f;
    bool enabled = true;
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
    // A world-space terrain trace for a foot-placement component. It must be
    // safe to call from the animation system's update thread.
    std::function<bool(Entity, const Vec3& from, const Vec3& to, Vec3& hit, Vec3& normal)> ground_trace;
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

AETHER_REFLECT(aether::AnimationLookAt, 1,
    AETHER_FIELD(bone, Field_EditAnywhere, {.tooltip = "Bone that turns toward the target"}),
    AETHER_FIELD(target, Field_EditAnywhere, {.tooltip = "World-space target", .units = "m"}),
    AETHER_FIELD(aim_axis, Field_EditAnywhere, {.tooltip = "Local bone axis that faces the target"}),
    AETHER_FIELD(max_angle, Field_EditAnywhere, {.range_min = 0.0, .range_max = 3.141592653589793, .units = "rad"}),
    AETHER_FIELD(weight, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(enabled, Field_EditAnywhere)
)

AETHER_REFLECT(aether::AnimationTwoBoneIKChain, 1,
    AETHER_FIELD(root, Field_EditAnywhere, {.tooltip = "Root bone, such as shoulder or hip"}),
    AETHER_FIELD(mid, Field_EditAnywhere, {.tooltip = "Middle bone, such as elbow or knee"}),
    AETHER_FIELD(end, Field_EditAnywhere, {.tooltip = "End bone, such as hand or ankle"}),
    AETHER_FIELD(target, Field_EditAnywhere, {.tooltip = "World-space reach target", .units = "m"}),
    AETHER_FIELD(pole, Field_EditAnywhere, {.tooltip = "World-space bend direction", .units = "m"}),
    AETHER_FIELD(use_pole, Field_EditAnywhere),
    AETHER_FIELD(weight, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0})
)

AETHER_REFLECT(aether::AnimationTwoBoneIK, 1,
    AETHER_FIELD(chains, Field_EditAnywhere, {.tooltip = "Arm and leg reach constraints"}),
    AETHER_FIELD(enabled, Field_EditAnywhere)
)

AETHER_REFLECT(aether::AnimationFootPlacementLeg, 1,
    AETHER_FIELD(hip, Field_EditAnywhere, {.tooltip = "Hip bone"}),
    AETHER_FIELD(knee, Field_EditAnywhere, {.tooltip = "Knee bone"}),
    AETHER_FIELD(ankle, Field_EditAnywhere, {.tooltip = "Ankle bone"}),
    AETHER_FIELD(foot_height, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0, .units = "m"})
)

AETHER_REFLECT(aether::AnimationFootPlacement, 1,
    AETHER_FIELD(pelvis, Field_EditAnywhere, {.tooltip = "Pelvis bone"}),
    AETHER_FIELD(legs, Field_EditAnywhere, {.tooltip = "Legs to place on the ground"}),
    AETHER_FIELD(max_step_up, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0, .units = "m"}),
    AETHER_FIELD(max_step_down, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0, .units = "m"}),
    AETHER_FIELD(max_foot_angle, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.5707963267948966, .units = "rad"}),
    AETHER_FIELD(smoothing, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0, .units = "s"}),
    AETHER_FIELD(enabled, Field_EditAnywhere)
)
