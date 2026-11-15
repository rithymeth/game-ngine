#pragma once

#include "aether/core/base.h"
#include "aether/reflection/serialize.h"

#include <map>
#include <string>
#include <vector>

namespace aether::dlg {

// One answer the player can give. `next` is the node it leads to; empty ends the conversation.
struct Choice {
    std::string text;
    std::string next;
    bool operator==(const Choice&) const = default;
};

// One step of a conversation: someone says `text`. It then leads to `next`, or offers `choices` (not both); with neither it is the last line.
// `event` is a free word a script can read when the node is reached ("give_key", "start_quest"): what the line causes is the game's business.
struct Node {
    std::string id;
    std::string speaker;
    std::string text;
    std::string next;
    std::string event;
    std::vector<Choice> choices;
    bool operator==(const Node&) const = default;
};

struct DialogueDef {
    static constexpr usize kMaxNodes = 1000;
    static constexpr usize kMaxChoices = 16;

    std::string name;
    std::string title; // shown text (empty: the name)
    std::string start; // the first node (empty in a file: the first node listed)
    std::vector<Node> nodes;

    const Node* Find(const std::string& id) const;
    // Empty string if valid, else a named error ("dialogue.name_empty", ...).
    std::string Validate() const;
    bool operator==(const DialogueDef&) const = default;
};

reflect::Json DialogueToJson(const DialogueDef& dialogue);
// False (with `error`, "dialogue.<code>: message") for bad JSON or an invalid conversation.
bool DialogueFromJson(const std::string& text, DialogueDef& out, std::string* error = nullptr);

// Conversations by name: filled from cooked `.adialogue` assets by the player, or by code.
class DialogueLibrary {
public:
    bool Register(DialogueDef dialogue, std::string* error = nullptr);
    const DialogueDef* Find(const std::string& name) const;
    void Clear() { dialogues_.clear(); }
    usize Size() const { return dialogues_.size(); }
    std::vector<std::string> Names() const; // sorted

private:
    std::map<std::string, DialogueDef> dialogues_;
};

} // namespace aether::dlg
