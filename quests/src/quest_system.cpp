#include "aether/quests/quest_system.h"

#include "aether/ecs/component.h"
#include "aether/loc/localize.h"
#include "aether/quests/quest_library.h" // registers the Blueprint library wherever the system is linked

#include <algorithm>

namespace aether::quest {

namespace {
QuestSystem* g_active = nullptr;
}

const char* ResultName(Result r) {
    switch (r) {
    case Result::Ok: return "ok";
    case Result::DeadEntity: return "dead_entity";
    case Result::UnknownQuest: return "unknown_quest";
    case Result::AlreadyActive: return "already_active";
    case Result::AlreadyCompleted: return "already_completed";
    case Result::PrereqUnmet: return "prereq_unmet";
    case Result::NotActive: return "not_active";
    case Result::UnknownObjective: return "unknown_objective";
    }
    return "ok";
}

QuestState* QuestLog::Find(const std::string& name) {
    const auto it = std::lower_bound(quests.begin(), quests.end(), name, [](const QuestState& q, const std::string& n) { return q.name < n; });
    return it != quests.end() && it->name == name ? &*it : nullptr;
}
const QuestState* QuestLog::Find(const std::string& name) const { return const_cast<QuestLog*>(this)->Find(name); }

void RegisterQuestComponents() {
    gas::RegisterGameplayComponents();
    (void)GetComponentId<QuestLog>();
}

QuestSystem::QuestSystem(World& world, gas::EffectSystem* effects, const QuestLibrary& quests) : world_(world), effects_(effects), quests_(quests) {
    RegisterQuestComponents();
    MakeActive();
}

QuestSystem::~QuestSystem() {
    if (g_active == this) g_active = nullptr;
}

QuestSystem* QuestSystem::Active() { return g_active; }
void QuestSystem::MakeActive() { g_active = this; }

QuestLog* QuestSystem::LogOf(Entity entity) const {
    if (entity.IsNull() || !world_.IsAlive(entity) || !world_.HasComponent<QuestLog>(entity)) return nullptr;
    return world_.GetComponent<QuestLog>(entity);
}

void QuestSystem::Queue(Entity entity, const std::string& quest, QuestEvent::Kind kind, const std::string& objective, i32 progress, i32 required) {
    QuestEvent e;
    e.entity = entity;
    e.quest = quest;
    e.objective = objective;
    e.progress = progress;
    e.required = required;
    e.kind = kind;
    events_.push_back(std::move(e));
}

i32 QuestSystem::State(Entity entity, const std::string& quest) const {
    const QuestLog* log = LogOf(entity);
    const QuestState* s = log ? log->Find(quest) : nullptr;
    return s ? s->state : kInactive;
}

i32 QuestSystem::GetProgress(Entity entity, const std::string& quest, const std::string& objective) const {
    const QuestLog* log = LogOf(entity);
    const QuestState* s = log ? log->Find(quest) : nullptr;
    if (s == nullptr) return 0;
    for (const ObjectiveState& o : s->objectives) {
        if (o.id == objective) return o.progress;
    }
    return 0;
}

std::string QuestSystem::Title(const std::string& quest) const {
    const QuestDef* def = quests_.Find(quest);
    if (def == nullptr) return {};
    const std::string fallback = def->title.empty() ? def->name : def->title;
    return def->title_key.empty() ? fallback : loc::Localize::GetText(def->title_key, fallback);
}

std::string QuestSystem::ObjectiveText(const std::string& quest, const std::string& objective) const {
    const QuestDef* def = quests_.Find(quest);
    if (def == nullptr) return {};
    for (const Objective& o : def->objectives) {
        if (o.id == objective) return o.text_key.empty() ? o.text : loc::Localize::GetText(o.text_key, o.text);
    }
    return {};
}

Result QuestSystem::Start(Entity entity, const std::string& quest) {
    if (entity.IsNull() || !world_.IsAlive(entity)) return Result::DeadEntity;
    const QuestDef* def = quests_.Find(quest);
    if (def == nullptr) return Result::UnknownQuest;
    if (State(entity, quest) == kActive) return Result::AlreadyActive;
    if (State(entity, quest) == kCompleted) return Result::AlreadyCompleted;
    for (const std::string& p : def->prerequisites) {
        if (State(entity, p) != kCompleted) return Result::PrereqUnmet;
    }
    if (!world_.HasComponent<QuestLog>(entity)) world_.AddComponent<QuestLog>(entity, QuestLog{});
    QuestLog* log = LogOf(entity);
    QuestState* s = log->Find(quest);
    if (s == nullptr) {
        QuestState fresh;
        fresh.name = quest;
        const auto it = std::lower_bound(log->quests.begin(), log->quests.end(), quest, [](const QuestState& q, const std::string& n) { return q.name < n; });
        s = &*log->quests.insert(it, std::move(fresh));
    }
    s->state = kActive;
    s->objectives.clear();
    for (const Objective& o : def->objectives) s->objectives.push_back({o.id, 0, false});
    Queue(entity, quest, QuestEvent::Kind::Started);
    if (def->objectives.empty()) CheckCompletion(entity, *def); // nothing to do: done at once
    return Result::Ok;
}

Result QuestSystem::Abandon(Entity entity, const std::string& quest) {
    QuestLog* log = LogOf(entity);
    QuestState* s = log ? log->Find(quest) : nullptr;
    if (s == nullptr || s->state != kActive) return Result::NotActive;
    s->state = kInactive;
    s->objectives.clear();
    Queue(entity, quest, QuestEvent::Kind::Abandoned);
    return Result::Ok;
}

Result QuestSystem::Fail(Entity entity, const std::string& quest) {
    QuestLog* log = LogOf(entity);
    QuestState* s = log ? log->Find(quest) : nullptr;
    if (s == nullptr || s->state != kActive) return Result::NotActive;
    s->state = kFailed;
    Queue(entity, quest, QuestEvent::Kind::Failed);
    return Result::Ok;
}

// Returns true if the objective's progress changed.
bool QuestSystem::Advance(Entity entity, const QuestDef& /*def*/, QuestState& state, const Objective& o, i32 amount) {
    ObjectiveState* os = nullptr;
    for (ObjectiveState& x : state.objectives) {
        if (x.id == o.id) os = &x;
    }
    if (os == nullptr || amount == 0) return false;
    if (os->done && amount < 0) return false; // done stays done
    const i32 before = os->progress;
    os->progress = amount > 0 && o.kind == Objective::Kind::Flag ? o.required : std::clamp(os->progress + amount, 0, o.required);
    if (os->progress == before) return false;
    Queue(entity, state.name, QuestEvent::Kind::ObjectiveProgress, o.id, os->progress, o.required);
    if (os->progress >= o.required && !os->done) {
        os->done = true;
        Queue(entity, state.name, QuestEvent::Kind::ObjectiveCompleted, o.id, os->progress, o.required);
    }
    return true;
}

void QuestSystem::CheckCompletion(Entity entity, const QuestDef& def) {
    QuestLog* log = LogOf(entity);
    QuestState* s = log ? log->Find(def.name) : nullptr;
    if (s == nullptr || s->state != kActive) return;
    for (const Objective& o : def.objectives) {
        if (o.optional) continue;
        const auto it = std::find_if(s->objectives.begin(), s->objectives.end(), [&](const ObjectiveState& x) { return x.id == o.id; });
        if (it == s->objectives.end() || !it->done) return;
    }
    s->state = kCompleted;
    QuestEvent e;
    e.entity = entity;
    e.quest = def.name;
    e.kind = QuestEvent::Kind::Completed;
    e.reward_items = def.reward_items;
    events_.push_back(std::move(e));
    // Effects can add components to the entity, which moves its log: nothing of it is used after this.
    if (effects_ != nullptr) {
        for (const std::string& name : def.reward_effects) effects_->Apply(entity, name);
    }
}

Result QuestSystem::Progress(Entity entity, const std::string& quest, const std::string& objective, i32 amount) {
    if (entity.IsNull() || !world_.IsAlive(entity)) return Result::DeadEntity;
    const QuestDef* def = quests_.Find(quest);
    if (def == nullptr) return Result::UnknownQuest;
    QuestLog* log = LogOf(entity);
    QuestState* s = log ? log->Find(quest) : nullptr;
    if (s == nullptr || s->state != kActive) return Result::NotActive;
    const auto it = std::find_if(def->objectives.begin(), def->objectives.end(), [&](const Objective& o) { return o.id == objective; });
    if (it == def->objectives.end()) return Result::UnknownObjective;
    if (Advance(entity, *def, *s, *it, amount)) CheckCompletion(entity, *def);
    return Result::Ok;
}

i32 QuestSystem::Notify(Entity entity, Objective::Kind kind, const std::string& target, i32 amount) {
    QuestLog* log = LogOf(entity);
    if (log == nullptr || amount == 0) return 0;
    i32 moved = 0;
    // The log can't move while only quest state changes (rewards are applied last, after completion), so go by name.
    std::vector<std::string> names;
    for (const QuestState& s : log->quests) {
        if (s.state == kActive) names.push_back(s.name);
    }
    for (const std::string& name : names) {
        const QuestDef* def = quests_.Find(name);
        if (def == nullptr) continue;
        QuestLog* l = LogOf(entity);
        QuestState* s = l ? l->Find(name) : nullptr;
        if (s == nullptr) continue;
        bool any = false;
        for (const Objective& o : def->objectives) {
            if (o.kind != kind || o.target != target) continue;
            const auto os = std::find_if(s->objectives.begin(), s->objectives.end(), [&](const ObjectiveState& x) { return x.id == o.id; });
            if (os == s->objectives.end() || os->done) continue;
            if (Advance(entity, *def, *s, o, amount)) {
                ++moved;
                any = true;
            }
        }
        if (any) CheckCompletion(entity, *def);
    }
    return moved;
}

} // namespace aether::quest
