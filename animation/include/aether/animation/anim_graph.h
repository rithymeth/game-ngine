#pragma once

#include "aether/animation/blend_space.h"
#include "aether/animation/montage.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aether::anim {

// Animation graphs (Phase 16 step 3, docs/design/PHASE_SPECS.md §16.4): a
// DAG of pose nodes ending in the graph's output, driven by typed
// variables, with state machines whose states are pose subgraphs.

enum class VarType : u8 { Bool, Int, Float, Trigger };
const char* VarTypeName(VarType type);

struct AnimVariable {
    std::string name;
    VarType type = VarType::Float;
    f32 default_value = 0.0f;
};

enum class AnimNodeKind : u8 {
    Clip,        // asset, loop, rate
    BlendSpace,  // asset, variable (x), variable_y (y, 2D)
    Blend,       // inputs [a, b], variable: the float alpha (0 = a, 1 = b)
    BlendByBool, // inputs [false, true], variable, blend_time
    BlendByInt,  // inputs [0, 1, ...], variable, blend_time
    Layered,     // inputs [base, layer], bone + depth: the layer applies from that bone down; weight
    Additive,    // inputs [base, additive]: the additive input's difference from the rest pose, times weight (or `variable`)
    StateMachine, // machine
    Slot          // inputs [source], slot: montages playing in that slot blend in over the source
};
const char* AnimNodeKindName(AnimNodeKind kind);

struct AnimNode {
    u32 id = 0;
    AnimNodeKind kind = AnimNodeKind::Clip;
    std::string asset;      // Clip, BlendSpace
    std::string machine;    // StateMachine
    std::string variable;   // see AnimNodeKind
    std::string variable_y; // BlendSpace (2D)
    std::vector<u32> inputs;
    bool loop = true;
    f32 rate = 1.0f;
    f32 blend_time = 0.2f; // BlendByBool/Int crossfades
    f32 weight = 1.0f;     // Layered, Additive
    std::string bone;      // Layered
    u32 depth = 0;         // Layered: blend depth
    std::string slot;      // Slot
};

enum class CompareOp : u8 { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual, IsTrue, IsFalse, Triggered };
struct Condition {
    std::string variable;
    CompareOp op = CompareOp::IsTrue;
    f32 value = 0.0f;
};

constexpr i32 kAnyState = -1;
struct Transition {
    i32 from = kAnyState; // a state index, or any state
    u32 to = 0;
    std::vector<Condition> conditions; // all must hold
    // Also required when set: the state's animation is within blend_time of its end.
    bool when_finished = false;
    f32 blend_time = 0.2f;
    i32 priority = 0; // higher first; ties in list order
};

struct AnimState {
    std::string name;
    u32 pose = 0;         // the pose node it plays (a StateMachine node makes it a sub-machine)
    bool conduit = false; // no pose: passed straight through to one of its exits
};

struct StateMachine {
    std::string name;
    u32 entry = 0;
    std::vector<AnimState> states;
    std::vector<Transition> transitions;
    i32 FindState(const std::string& name) const;
};

struct AnimGraph {
    std::vector<AnimVariable> variables;
    std::vector<AnimNode> nodes;
    std::vector<StateMachine> machines;
    u32 output = 0; // the node whose pose is the result

    const AnimNode* Find(u32 id) const;
    const StateMachine* FindMachine(const std::string& name) const;
    const AnimVariable* FindVariable(const std::string& name) const;
};

// What the graph's clip and blend space names refer to.
struct AnimAssets {
    std::function<const AnimationClip*(const std::string&)> clip;
    std::function<const BlendSpace*(const std::string&)> blend_space;
};

// AG001 no output node; AG002 a node's inputs are wrong (missing node, wrong
// count); AG003 the pose nodes loop; AG004 an unknown variable; AG005 a
// variable of the wrong type for its use; AG006 a state machine problem (no
// states, a bad entry, a transition to a state that doesn't exist, an
// unknown machine); AG007 a conduit as the entry or with no way out; AG008
// a state machine inside itself (or a Slot node without a slot name: AG002); AG009 (warning) a state no transition
// reaches; AG010 a clip or blend space the assets don't have; AG011 two
// variables or states with one name.
struct AnimDiagnostic {
    std::string code;
    bool error = true;
    u32 node = 0;
    std::string machine;
    std::string message;
};
std::vector<AnimDiagnostic> ValidateAnimGraph(const AnimGraph& graph, const AnimAssets* assets = nullptr);

// --- Runtime -------------------------------------------------------------------------------
struct StateChange {
    std::string machine, from, to;
};

// What happened during an Update, for gameplay and Blueprints (the
// Animator's events, step 4).
enum class AnimEventType : u8 {
    Notify,                // name: the notify
    NotifyBegin,           // a window notify starting
    NotifyEnd,             // and ending
    StateChanged,          // name: the new state, detail: the old one, machine
    MontageStarted,        // name: the montage
    MontageSectionChanged, // detail: the section
    MontageBlendingOut,
    MontageEnded, // interrupted: stopped or replaced rather than finished
};
const char* AnimEventTypeName(AnimEventType type);
struct AnimEvent {
    AnimEventType type = AnimEventType::Notify;
    std::string name;
    std::string detail;
    std::string machine;
    f32 weight = 1.0f; // notifies: how much the animation that fired it counted
    bool interrupted = false;
};

// One character's graph: its variable values and every node's playback state.
class AnimGraphInstance {
public:
    AnimGraphInstance(const AnimGraph& graph, const Skeleton& skeleton, AnimAssets assets);
    ~AnimGraphInstance();

    bool SetBool(const std::string& name, bool value);
    bool SetInt(const std::string& name, i32 value);
    bool SetFloat(const std::string& name, f32 value);
    // Triggers last until the end of the next Update, or until a transition uses one.
    bool SetTrigger(const std::string& name);
    f32 Get(const std::string& name) const;

    // Advances by `dt` and writes the output pose. The state changes it
    // made are in Changes() until the next Update.
    void Update(f32 dt, Pose& out);
    const std::vector<StateChange>& Changes() const { return changes_; }

    // The state a machine node is in ("" if the node isn't a machine), and
    // whether it's blending into it.
    std::string CurrentState(u32 machine_node) const;
    bool InTransition(u32 machine_node) const;

    // Everything that happened during the last Update (and calls since it,
    // like PlayMontage's start): notifies, state changes, montage events.
    const std::vector<AnimEvent>& Events() const { return events_; }

    // --- Montages ---------------------------------------------------------------
    // Plays a montage in its slot (replacing the one there, which ends
    // interrupted), from a section or the start. False if its clip isn't known.
    bool PlayMontage(const Montage& montage, f32 rate = 1.0f, const std::string& section = {});
    // Blends the slot's montage out (over `blend_out`, or its own when < 0).
    bool StopMontage(const std::string& slot = "Default", f32 blend_out = -1.0f);
    bool JumpToSection(const std::string& section, const std::string& slot = "Default");
    bool IsPlayingMontage(const std::string& slot = "Default") const;
    std::string CurrentSection(const std::string& slot = "Default") const;
    f32 MontageWeight(const std::string& slot = "Default") const;

    // --- Root motion ------------------------------------------------------------
    // With root motion on, clips and montages play in place and their
    // motion, weighted like their poses, is collected for the character.
    void SetRootMotion(bool enabled, const RootMotionSettings& settings = {});
    const RootMotionDelta& RootMotion() const { return root_motion_; } // the last Update's

private:
    struct NodeState;
    struct MachineState;
    struct MontageRun;
    const Pose& Evaluate(u32 node, f32 dt, f32 weight);
    void EvaluateMachine(const AnimNode& node, f32 dt, f32 weight, Pose& out);
    void UpdateMontages(f32 dt);
    void Emit(AnimEvent event);
    void Collect(); // notifies and root motion from this frame's nodes
    void Reset(u32 node);
    f32 RemainingTime(u32 node) const;
    bool Passes(const Transition& t, const StateMachine& m, const MachineState& ms) const;
    bool FindTransition(const StateMachine& m, MachineState& ms, i32& target, f32& blend, std::vector<std::string>& used);

    const AnimGraph& graph_;
    const Skeleton& skeleton_;
    AnimAssets assets_;
    std::map<std::string, f32> values_;
    std::map<u32, std::unique_ptr<NodeState>> nodes_;
    std::map<u32, std::unique_ptr<MachineState>> machines_;
    u64 frame_ = 0;
    std::vector<StateChange> changes_;
    std::map<std::string, std::unique_ptr<MontageRun>> montages_; // by slot
    std::vector<AnimEvent> events_, pending_;
    bool in_update_ = false;
    bool root_motion_enabled_ = false;
    RootMotionSettings root_settings_;
    RootMotionDelta root_motion_;
    f32 root_yaw_ = 0.0f;
};

// .aanim files.
nlohmann::json AnimGraphToJson(const AnimGraph& graph);
bool AnimGraphFromJson(const nlohmann::json& json, AnimGraph& out, std::string* error = nullptr);

} // namespace aether::anim
