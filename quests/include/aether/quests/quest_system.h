#pragma once

#include "aether/ecs/world.h"
#include "aether/gameplay/effect_system.h"
#include "aether/quests/quest_def.h"
#include "aether/quests/quest_log.h"

#include <string>
#include <vector>

namespace aether::quest {

// Registers the quest log component with the ECS (idempotent).
void RegisterQuestComponents();

enum class Result : u8 { Ok, DeadEntity, UnknownQuest, AlreadyActive, AlreadyCompleted, PrereqUnmet, NotActive, UnknownObjective };
// "prereq_unmet", "not_active", ...
const char* ResultName(Result result);

// Something happened in an entity's quests (§30.10).
struct QuestEvent {
    enum class Kind : u8 { Started, ObjectiveProgress, ObjectiveCompleted, Completed, Failed, Abandoned };
    Entity entity;
    std::string quest;
    std::string objective; // for the objective events
    i32 progress = 0;
    i32 required = 0;
    Kind kind = Kind::Started;
    std::vector<RewardItem> reward_items; // Completed: for the host to hand out (the quest kit doesn't know about inventories)
};

// Runs entities' quests. A quest is started (its prerequisites completed), its objectives are advanced
// by Progress (named) or Notify (by kind and target: what the host forwards, e.g. an item picked up
// is Notify(entity, Count, "Wolf Pelt", 1)), and when every non-optional objective is done the quest
// completes: reward effects are applied through the EffectSystem and the Completed event carries
// the reward items. Count and tag objectives clamp at `required`; a negative amount takes progress
// back while the objective is not yet done; a flag is done at once. Deterministic and event-driven (no Update).
// The Blueprint library acts on the active system.
class QuestSystem {
public:
    // `effects` may be null: reward effects are then skipped.
    QuestSystem(World& world, gas::EffectSystem* effects, const QuestLibrary& quests);
    ~QuestSystem();
    QuestSystem(const QuestSystem&) = delete;
    QuestSystem& operator=(const QuestSystem&) = delete;

    static QuestSystem* Active();
    void MakeActive();

    // A completed quest can't be started again; an abandoned or failed one can (it starts over).
    Result Start(Entity entity, const std::string& quest);
    Result Abandon(Entity entity, const std::string& quest);
    Result Fail(Entity entity, const std::string& quest);
    // Advances one objective by `amount` (a flag objective is set by any positive amount).
    Result Progress(Entity entity, const std::string& quest, const std::string& objective, i32 amount = 1);
    // Advances every unfinished objective of the entity's active quests whose kind and target match; how many moved.
    i32 Notify(Entity entity, Objective::Kind kind, const std::string& target, i32 amount = 1);

    // Inactive for a quest the entity hasn't started (or doesn't have).
    i32 State(Entity entity, const std::string& quest) const; // a QuestStatus
    i32 GetProgress(Entity entity, const std::string& quest, const std::string& objective) const;
    bool IsActive(Entity entity, const std::string& quest) const { return State(entity, quest) == kActive; }
    bool IsCompleted(Entity entity, const std::string& quest) const { return State(entity, quest) == kCompleted; }
    // The quest's title (translated when it has a key) and an objective's text.
    std::string Title(const std::string& quest) const;
    std::string ObjectiveText(const std::string& quest, const std::string& objective) const;

    const std::vector<QuestEvent>& Events() const { return events_; }
    void ClearEvents() { events_.clear(); }

private:
    QuestLog* LogOf(Entity entity) const;
    void Queue(Entity entity, const std::string& quest, QuestEvent::Kind kind, const std::string& objective = {}, i32 progress = 0, i32 required = 0);
    void CheckCompletion(Entity entity, const QuestDef& def);
    bool Advance(Entity entity, const QuestDef& def, QuestState& state, const Objective& o, i32 amount);

    World& world_;
    gas::EffectSystem* effects_;
    const QuestLibrary& quests_;
    std::vector<QuestEvent> events_;
};

} // namespace aether::quest
