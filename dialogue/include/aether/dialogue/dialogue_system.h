#pragma once

#include "aether/dialogue/dialogue_def.h"
#include "aether/ecs/entity.h"
#include "aether/ecs/world.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::dlg {

enum class Result : u8 { Ok, DeadEntity, UnknownDialogue, NotActive, NeedsChoice, BadChoice };
// "ok", "dead_entity", "unknown_dialogue", "not_active" (no conversation is running), "needs_choice" (the node offers choices: use Choose),
// "bad_choice" (the node has no such choice)
const char* ResultName(Result result);

// What an entity is hearing now: the node it is at.
struct Line {
    std::string dialogue;
    std::string node;
    std::string speaker;
    std::string text;
    std::string event; // the node's free word, empty if none
    std::vector<std::string> choices; // empty: Advance continues (or ends, if `last`)
    bool last = false;                // no next node and no choices: Advance ends the conversation
};

// Runs conversations: one per entity (the listener, usually the player), moved by Advance and Choose. Deterministic and event-driven (no
// Update). Starting a conversation while one runs replaces it. What a line causes is the game's business: read Line::event.
class DialogueSystem {
public:
    DialogueSystem(World& world, const DialogueLibrary& dialogues);
    ~DialogueSystem();
    DialogueSystem(const DialogueSystem&) = delete;
    DialogueSystem& operator=(const DialogueSystem&) = delete;

    static DialogueSystem* Active();
    void MakeActive();

    Result Start(Entity listener, const std::string& dialogue);
    bool IsActive(Entity listener) const;
    std::optional<Line> Current(Entity listener) const;
    // Moves to the next node (Ok also when this was the last line: the conversation is over then). NeedsChoice if the node offers choices.
    Result Advance(Entity listener);
    // Takes choice `index` (0 is the first) of the node. A choice with no next node ends the conversation.
    Result Choose(Entity listener, i32 index);
    void End(Entity listener);

    const DialogueLibrary& Dialogues() const { return dialogues_; }

private:
    struct Session {
        std::string dialogue;
        std::string node;
    };
    const Session* Find(Entity listener) const;
    void Go(Entity listener, Session& session, const std::string& next);

    World& world_;
    const DialogueLibrary& dialogues_;
    std::unordered_map<Entity, Session> sessions_;
};

} // namespace aether::dlg
