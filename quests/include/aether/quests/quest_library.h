#pragma once

#include "aether/core/base.h"
#include "aether/ecs/entity.h"
#include "aether/reflection/reflection.h"

#include <string>

// Blueprint function library for quests (Phase 30 step 7c, §30.10), acting on
// QuestSystem::Active(); without one everything returns false / 0 / "". The reply of the
// functions that start or change a quest is true if it worked. `Event.OnQuestStarted` (quest),
// `Event.OnQuestProgress` (quest, objective, progress, required), `Event.OnQuestCompleted` (quest)
// and `Event.OnQuestFailed` (quest) are what an entity's Blueprint hears. Kinds are "count",
// "tag" and "flag".

namespace aether::quest {

struct Quests {
    static bool StartQuest(const Entity& owner, const std::string& quest);
    static bool AbandonQuest(const Entity& owner, const std::string& quest);
    static bool FailQuest(const Entity& owner, const std::string& quest);
    static bool ProgressQuest(const Entity& owner, const std::string& quest, const std::string& objective, i32 amount);
    // Advances the owner's active quests' objectives of that kind and target; how many moved.
    static i32 NotifyQuests(const Entity& owner, const std::string& kind, const std::string& target, i32 amount);
    static bool IsQuestActive(const Entity& owner, const std::string& quest);
    static bool IsQuestCompleted(const Entity& owner, const std::string& quest);
    // "inactive", "active", "completed" or "failed".
    static std::string GetQuestState(const Entity& owner, const std::string& quest);
    static i32 GetQuestProgress(const Entity& owner, const std::string& quest, const std::string& objective);
    static std::string GetQuestTitle(const std::string& quest);
};

} // namespace aether::quest

AETHER_REFLECT(aether::quest::Quests, 1,
    AETHER_METHOD(StartQuest, Fn_BlueprintCallable, {"owner", "quest"}),
    AETHER_METHOD(AbandonQuest, Fn_BlueprintCallable, {"owner", "quest"}),
    AETHER_METHOD(FailQuest, Fn_BlueprintCallable, {"owner", "quest"}),
    AETHER_METHOD(ProgressQuest, Fn_BlueprintCallable, {"owner", "quest", "objective", "amount"}),
    AETHER_METHOD(NotifyQuests, Fn_BlueprintCallable, {"owner", "kind", "target", "amount"}),
    AETHER_METHOD(IsQuestActive, Fn_BlueprintCallable | Fn_Pure, {"owner", "quest"}),
    AETHER_METHOD(IsQuestCompleted, Fn_BlueprintCallable | Fn_Pure, {"owner", "quest"}),
    AETHER_METHOD(GetQuestState, Fn_BlueprintCallable | Fn_Pure, {"owner", "quest"}),
    AETHER_METHOD(GetQuestProgress, Fn_BlueprintCallable | Fn_Pure, {"owner", "quest", "objective"}),
    AETHER_METHOD(GetQuestTitle, Fn_BlueprintCallable | Fn_Pure, {"quest"}))
