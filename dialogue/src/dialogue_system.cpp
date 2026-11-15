#include "aether/dialogue/dialogue_system.h"

namespace aether::dlg {

namespace {
DialogueSystem* g_active = nullptr;
} // namespace

const char* ResultName(Result result) {
    switch (result) {
        case Result::Ok: return "ok";
        case Result::DeadEntity: return "dead_entity";
        case Result::UnknownDialogue: return "unknown_dialogue";
        case Result::NotActive: return "not_active";
        case Result::NeedsChoice: return "needs_choice";
        case Result::BadChoice: return "bad_choice";
    }
    return "unknown";
}

DialogueSystem::DialogueSystem(World& world, const DialogueLibrary& dialogues) : world_(world), dialogues_(dialogues) { MakeActive(); }
DialogueSystem::~DialogueSystem() {
    if (g_active == this) g_active = nullptr;
}
DialogueSystem* DialogueSystem::Active() { return g_active; }
void DialogueSystem::MakeActive() { g_active = this; }

const DialogueSystem::Session* DialogueSystem::Find(Entity listener) const {
    if (!world_.IsAlive(listener)) return nullptr;
    const auto it = sessions_.find(listener);
    return it == sessions_.end() ? nullptr : &it->second;
}

Result DialogueSystem::Start(Entity listener, const std::string& dialogue) {
    if (!world_.IsAlive(listener)) return Result::DeadEntity;
    const DialogueDef* def = dialogues_.Find(dialogue);
    if (def == nullptr) return Result::UnknownDialogue;
    sessions_[listener] = Session{dialogue, def->start};
    return Result::Ok;
}

bool DialogueSystem::IsActive(Entity listener) const { return Find(listener) != nullptr; }

std::optional<Line> DialogueSystem::Current(Entity listener) const {
    const Session* session = Find(listener);
    if (session == nullptr) return std::nullopt;
    const DialogueDef* def = dialogues_.Find(session->dialogue);
    const Node* node = def ? def->Find(session->node) : nullptr;
    if (node == nullptr) return std::nullopt;
    Line line{session->dialogue, node->id, node->speaker, node->text, node->event, {}, node->next.empty() && node->choices.empty()};
    for (const Choice& choice : node->choices) line.choices.push_back(choice.text);
    return line;
}

void DialogueSystem::Go(Entity listener, Session& session, const std::string& next) {
    if (next.empty()) sessions_.erase(listener);
    else session.node = next;
}

Result DialogueSystem::Advance(Entity listener) {
    if (!world_.IsAlive(listener)) return Result::DeadEntity;
    const auto it = sessions_.find(listener);
    if (it == sessions_.end()) return Result::NotActive;
    const DialogueDef* def = dialogues_.Find(it->second.dialogue);
    const Node* node = def ? def->Find(it->second.node) : nullptr;
    if (node == nullptr) { sessions_.erase(it); return Result::NotActive; }
    if (!node->choices.empty()) return Result::NeedsChoice;
    const std::string next = node->next; // Go may erase the session (and with it the node's owner is untouched, the library's)
    Go(listener, it->second, next);
    return Result::Ok;
}

Result DialogueSystem::Choose(Entity listener, i32 index) {
    if (!world_.IsAlive(listener)) return Result::DeadEntity;
    const auto it = sessions_.find(listener);
    if (it == sessions_.end()) return Result::NotActive;
    const DialogueDef* def = dialogues_.Find(it->second.dialogue);
    const Node* node = def ? def->Find(it->second.node) : nullptr;
    if (node == nullptr) { sessions_.erase(it); return Result::NotActive; }
    if (index < 0 || static_cast<usize>(index) >= node->choices.size()) return Result::BadChoice;
    const std::string next = node->choices[static_cast<usize>(index)].next;
    Go(listener, it->second, next);
    return Result::Ok;
}

void DialogueSystem::End(Entity listener) { sessions_.erase(listener); }

} // namespace aether::dlg
