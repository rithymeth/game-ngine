#pragma once

#include "aether/ai/bt_runtime.h"
#include "aether/reflection/reflection.h"

#include <memory>
#include <unordered_map>

namespace aether {

struct BehaviorTreeCommand {
    enum class Kind : u8 { Start, Stop, Restart };
    Kind kind = Kind::Start;
};

// Runs a Behavior Tree (.abt) on its entity (Phase 20 step 4,
// docs/design/PHASE_SPECS.md §20.4), with the tree's blackboard. The
// methods are Blueprint nodes and Luau methods: Start, Stop and Restart
// are applied on the next update; the blackboard methods act at once
// (set before the tree starts, values that fit its keys are kept).
struct BehaviorTreeComponent {
    std::string tree;
    bool auto_start = true;
    f32 tick_interval = 0.0f; // seconds between ticks (0: every update)

    // Runtime; not saved.
    bool running = false;
    std::string active; // the deepest running node's label
    std::shared_ptr<ai::Blackboard> blackboard;
    std::vector<BehaviorTreeCommand> commands;

    ai::Blackboard& Board();
    void Start();
    void Stop();
    void Restart();
    bool IsRunning() const { return running; }
    std::string GetActiveNode() const { return active; }
    // False when the key isn't the tree's, or is another type.
    bool SetBool(const std::string& key, bool value);
    bool SetInt(const std::string& key, i32 value);
    bool SetFloat(const std::string& key, f32 value);
    bool SetString(const std::string& key, const std::string& value);
    bool SetVector(const std::string& key, const Vec3& value);
    bool SetEntity(const std::string& key, const Entity& value);
    bool ClearValue(const std::string& key);
    bool IsValueSet(const std::string& key) const;
    bool GetBool(const std::string& key) const;
    i32 GetInt(const std::string& key) const;
    f32 GetFloat(const std::string& key) const;
    std::string GetString(const std::string& key) const;
    Vec3 GetVector(const std::string& key) const;
    Entity GetEntity(const std::string& key) const;
};

// Blueprint function library for Blueprint tasks.
struct BehaviorTrees {
    // Ends the entity's running Run Blueprint task.
    static bool FinishTask(const Entity& entity, bool success);
};

void RegisterBehaviorTreeComponents();

} // namespace aether

namespace aether::ai {

// Runs a world's BehaviorTreeComponents: an instance per component, ticked
// each Update, with Blueprint and Luau tasks and services going through `hooks`.
class BehaviorTreeWorld {
public:
    using AssetLookup = std::function<const BehaviorTreeAsset*(const std::string& tree)>;
    BehaviorTreeWorld(World& world, AssetLookup assets);
    ~BehaviorTreeWorld();
    BehaviorTreeWorld(const BehaviorTreeWorld&) = delete;
    BehaviorTreeWorld& operator=(const BehaviorTreeWorld&) = delete;

    void Update(f32 dt);
    BtHooks hooks;

    // Ends the entity's waiting Blueprint task (Behavior Trees > Finish Task).
    bool FinishTask(Entity e, bool success, const std::string& event = {});
    const BehaviorTreeInstance* InstanceOf(Entity e) const;
    const std::vector<std::string>& Problems() const { return problems_; }

    static BehaviorTreeWorld* Active();
    void MakeActive();

private:
    struct Live {
        Entity entity;
        std::string tree;
        const BehaviorTreeAsset* asset = nullptr;
        std::unique_ptr<BehaviorTreeInstance> instance;
        f32 accumulated = 0.0f;
    };
    void Report(const std::string& key, const std::string& message);
    void StopLive(Live& live, BehaviorTreeComponent& c);

    World& world_;
    AssetLookup assets_;
    std::unordered_map<u32, Live> live_; // by entity index
    std::unordered_map<std::string, bool> reported_;
    std::vector<std::string> problems_;
};

} // namespace aether::ai

AETHER_REFLECT(aether::BehaviorTreeComponent, 1,
    AETHER_FIELD(tree, Field_EditAnywhere, {.tooltip = "The Behavior Tree to run (.abt)"}),
    AETHER_FIELD(auto_start, Field_EditAnywhere, {.tooltip = "Start when the entity appears"}),
    AETHER_FIELD(tick_interval, Field_EditAnywhere, {.tooltip = "Seconds between ticks (0: every frame)", .range_min = 0.0, .range_max = 10.0, .units = "s"}),
    AETHER_FIELD(running, Field_ReadOnly | Field_Transient),
    AETHER_FIELD(active, Field_ReadOnly | Field_Transient),
    AETHER_METHOD(Start, Fn_BlueprintCallable),
    AETHER_METHOD(Stop, Fn_BlueprintCallable),
    AETHER_METHOD(Restart, Fn_BlueprintCallable),
    AETHER_METHOD(IsRunning, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(GetActiveNode, Fn_BlueprintCallable | Fn_Pure),
    AETHER_METHOD(SetBool, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetInt, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetFloat, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetString, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetVector, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(SetEntity, Fn_BlueprintCallable, {"key", "value"}),
    AETHER_METHOD(ClearValue, Fn_BlueprintCallable, {"key"}),
    AETHER_METHOD(IsValueSet, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(GetBool, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(GetInt, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(GetFloat, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(GetString, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(GetVector, Fn_BlueprintCallable | Fn_Pure, {"key"}),
    AETHER_METHOD(GetEntity, Fn_BlueprintCallable | Fn_Pure, {"key"})
)

AETHER_REFLECT(aether::BehaviorTrees, 1,
    AETHER_METHOD(FinishTask, Fn_BlueprintCallable, {"entity", "success"})
)
