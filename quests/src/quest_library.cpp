#include "aether/quests/quest_library.h"

#include "aether/quests/quest_system.h"

namespace aether::quest {

bool Quests::StartQuest(const Entity& owner, const std::string& quest) {
    QuestSystem* s = QuestSystem::Active();
    return s && s->Start(owner, quest) == Result::Ok;
}
bool Quests::AbandonQuest(const Entity& owner, const std::string& quest) {
    QuestSystem* s = QuestSystem::Active();
    return s && s->Abandon(owner, quest) == Result::Ok;
}
bool Quests::FailQuest(const Entity& owner, const std::string& quest) {
    QuestSystem* s = QuestSystem::Active();
    return s && s->Fail(owner, quest) == Result::Ok;
}
bool Quests::ProgressQuest(const Entity& owner, const std::string& quest, const std::string& objective, i32 amount) {
    QuestSystem* s = QuestSystem::Active();
    return s && s->Progress(owner, quest, objective, amount) == Result::Ok;
}
i32 Quests::NotifyQuests(const Entity& owner, const std::string& kind, const std::string& target, i32 amount) {
    QuestSystem* s = QuestSystem::Active();
    if (s == nullptr) return 0;
    Objective::Kind k;
    if (kind == "count") k = Objective::Kind::Count;
    else if (kind == "tag") k = Objective::Kind::Tag;
    else if (kind == "flag") k = Objective::Kind::Flag;
    else return 0;
    return s->Notify(owner, k, target, amount);
}
bool Quests::IsQuestActive(const Entity& owner, const std::string& quest) {
    const QuestSystem* s = QuestSystem::Active();
    return s && s->IsActive(owner, quest);
}
bool Quests::IsQuestCompleted(const Entity& owner, const std::string& quest) {
    const QuestSystem* s = QuestSystem::Active();
    return s && s->IsCompleted(owner, quest);
}
std::string Quests::GetQuestState(const Entity& owner, const std::string& quest) {
    const QuestSystem* s = QuestSystem::Active();
    switch (s ? s->State(owner, quest) : static_cast<i32>(kInactive)) {
    case kActive: return "active";
    case kCompleted: return "completed";
    case kFailed: return "failed";
    default: return "inactive";
    }
}
i32 Quests::GetQuestProgress(const Entity& owner, const std::string& quest, const std::string& objective) {
    const QuestSystem* s = QuestSystem::Active();
    return s ? s->GetProgress(owner, quest, objective) : 0;
}
std::string Quests::GetQuestTitle(const std::string& quest) {
    const QuestSystem* s = QuestSystem::Active();
    return s ? s->Title(quest) : std::string();
}

} // namespace aether::quest
