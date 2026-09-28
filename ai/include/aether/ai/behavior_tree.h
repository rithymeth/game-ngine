#pragma once

#include "aether/ai/blackboard.h"

#include <string>
#include <vector>

namespace aether::ai {

// Behavior Tree assets (Phase 20 step 4, docs/design/PHASE_SPECS.md §20.4):
// composites choose among children, decorators gate them (and abort them
// when a watched blackboard key changes), services run while a branch is
// active, and tasks do the work. `.abt` files are JSON.

enum class BtStatus : u8 { Running, Success, Failure };

enum class BtNodeType : u8 {
    // Composites.
    Selector, // tries children in order until one succeeds
    Sequence, // runs children in order until one fails
    Parallel, // runs every child at once (see succeed_on_one)
    // Tasks.
    Wait,            // seconds (+- deviation)
    MoveTo,          // the NavAgent to the key's Vector or Entity, within acceptance
    SetBlackboard,   // key = value
    ClearBlackboard, // key
    RunBlueprint,    // dispatches Event.Custom:<event>; it ends with Behavior Trees > Finish Task
    RunLuau,         // calls <event>(entity, dt, first) each tick: "running", "success" or "failure"
    Log,             // event is the text
    Succeed,
    Fail,
};
const char* BtNodeTypeName(BtNodeType type);
bool IsComposite(BtNodeType type);

enum class BtDecoratorType : u8 {
    BlackboardCondition, // key op value
    Cooldown,            // can't run again for seconds after it ends
    Loop,                // runs again on success, count times (0: forever)
    TimeLimit,           // fails after seconds
    Inverter,
    ForceSuccess,
    ForceFailure,
};
// Which running branches a condition aborts when its result changes:
// Self (the branch it's on, when it turns false), LowerPriority (a
// selector's later siblings, when it turns true), or both.
enum class BtAbort : u8 { None, Self, LowerPriority, Both };
enum class BtCompare : u8 { IsSet, IsNotSet, Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

struct BtDecorator {
    BtDecoratorType type = BtDecoratorType::BlackboardCondition;
    std::string key;
    BtCompare op = BtCompare::IsSet;
    BlackboardValue value;
    BtAbort abort = BtAbort::None;
    f32 seconds = 1.0f;
    i32 count = 0;
};

enum class BtServiceType : u8 {
    Blueprint,  // dispatches Event.Custom:<event> every interval
    Luau,       // calls <event>(entity, dt) every interval
    DistanceTo, // out_key (Float) = the distance to key's Vector or Entity
};

struct BtService {
    BtServiceType type = BtServiceType::Blueprint;
    f32 interval = 0.5f;
    std::string event;
    std::string key, out_key;
};

struct BtNode {
    BtNodeType type = BtNodeType::Sequence;
    std::string name; // shown in the editor and debugger (the type when empty)
    std::vector<BtDecorator> decorators;
    std::vector<BtService> services;
    std::vector<BtNode> children;
    // Tasks' settings.
    f32 seconds = 1.0f, deviation = 0.0f; // Wait
    std::string key;                      // MoveTo, SetBlackboard, ClearBlackboard
    f32 acceptance = 0.5f;                // MoveTo
    BlackboardValue value;                // SetBlackboard
    std::string event;                    // RunBlueprint, RunLuau, Log
    bool succeed_on_one = false;          // Parallel: succeeds when one child does (else when all do)

    std::string Label() const { return name.empty() ? BtNodeTypeName(type) : name; }
};

struct BehaviorTreeAsset {
    std::string name;
    BlackboardAsset blackboard;
    BtNode root;
};

nlohmann::json SaveBehaviorTree(const BehaviorTreeAsset& tree);
bool LoadBehaviorTree(const nlohmann::json& json, BehaviorTreeAsset& out, std::string* error = nullptr);

struct BtProblem {
    std::string code; // BT001...
    std::string path; // "root/Sequence[1]/Wait"
    std::string message;
    bool warning = false;
};
// BT001 a composite without children; BT002 a task with children; BT003 an
// unknown blackboard key; BT004 a value or comparison that doesn't fit the
// key's type; BT005 a Parallel with fewer than two children (warning);
// BT006 a LowerPriority abort outside a Selector (warning); BT007 a
// negative time, or an interval that isn't positive; BT008 a Blueprint or
// Luau task or service without an event; BT009 a MoveTo or DistanceTo key
// that isn't a Vector or Entity; BT010 duplicate blackboard keys.
std::vector<BtProblem> ValidateBehaviorTree(const BehaviorTreeAsset& tree);

} // namespace aether::ai
