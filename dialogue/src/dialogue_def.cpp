#include "aether/dialogue/dialogue_def.h"

#include <set>

namespace aether::dlg {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool ReadString(const Json& j, const char* key, std::string& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return Fail(error, std::string("dialogue.bad_json: '") + key + "' is a string");
    out = j[key].get<std::string>();
    return true;
}

} // namespace

const Node* DialogueDef::Find(const std::string& id) const {
    for (const Node& node : nodes) if (node.id == id) return &node;
    return nullptr;
}

std::string DialogueDef::Validate() const {
    if (name.empty()) return "dialogue.name_empty: a dialogue needs a name";
    if (nodes.empty()) return "dialogue.no_nodes: '" + name + "' has no nodes";
    if (nodes.size() > kMaxNodes) return "dialogue.too_many_nodes: '" + name + "' has more than " + std::to_string(kMaxNodes) + " nodes";
    std::set<std::string> ids;
    for (const Node& node : nodes) {
        if (node.id.empty()) return "dialogue.node_id_empty: a node of '" + name + "' has no id";
        if (!ids.insert(node.id).second) return "dialogue.duplicate_node: '" + name + "' has two nodes named '" + node.id + "'";
    }
    if (Find(start) == nullptr) return "dialogue.unknown_start: '" + name + "' starts at '" + start + "', which isn't a node";
    for (const Node& node : nodes) {
        if (node.text.empty()) return "dialogue.text_empty: node '" + node.id + "' of '" + name + "' says nothing";
        if (!node.next.empty() && !node.choices.empty()) return "dialogue.next_and_choices: node '" + node.id + "' of '" + name + "' has both a next node and choices";
        if (!node.next.empty() && Find(node.next) == nullptr) return "dialogue.unknown_next: node '" + node.id + "' of '" + name + "' leads to '" + node.next + "', which isn't a node";
        if (node.choices.size() > kMaxChoices) return "dialogue.too_many_choices: node '" + node.id + "' of '" + name + "' has more than " + std::to_string(kMaxChoices) + " choices";
        for (const Choice& choice : node.choices) {
            if (choice.text.empty()) return "dialogue.choice_text_empty: a choice of node '" + node.id + "' says nothing";
            if (!choice.next.empty() && Find(choice.next) == nullptr) return "dialogue.unknown_next: a choice of node '" + node.id + "' leads to '" + choice.next + "', which isn't a node";
        }
    }
    return {};
}

Json DialogueToJson(const DialogueDef& dialogue) {
    Json nodes = Json::array();
    for (const Node& node : dialogue.nodes) {
        Json n = {{"id", node.id}, {"text", node.text}};
        if (!node.speaker.empty()) n["speaker"] = node.speaker;
        if (!node.next.empty()) n["next"] = node.next;
        if (!node.event.empty()) n["event"] = node.event;
        if (!node.choices.empty()) {
            Json choices = Json::array();
            for (const Choice& choice : node.choices) {
                Json c = {{"text", choice.text}};
                if (!choice.next.empty()) c["next"] = choice.next;
                choices.push_back(std::move(c));
            }
            n["choices"] = std::move(choices);
        }
        nodes.push_back(std::move(n));
    }
    Json j = {{"name", dialogue.name}, {"start", dialogue.start}, {"nodes", std::move(nodes)}};
    if (!dialogue.title.empty()) j["title"] = dialogue.title;
    return j;
}

bool DialogueFromJson(const std::string& text, DialogueDef& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "dialogue.bad_json: a dialogue is a JSON object");
    DialogueDef dialogue;
    if (!ReadString(j, "name", dialogue.name, error) || !ReadString(j, "title", dialogue.title, error) || !ReadString(j, "start", dialogue.start, error)) return false;
    if (j.contains("nodes")) {
        if (!j["nodes"].is_array()) return Fail(error, "dialogue.bad_json: 'nodes' is a list");
        for (const Json& entry : j["nodes"]) {
            if (!entry.is_object()) return Fail(error, "dialogue.bad_json: every node is an object");
            Node node;
            for (const auto& [key, field] : std::initializer_list<std::pair<const char*, std::string*>>{
                     {"id", &node.id}, {"speaker", &node.speaker}, {"text", &node.text}, {"next", &node.next}, {"event", &node.event}}) {
                if (!ReadString(entry, key, *field, error)) return false;
            }
            if (entry.contains("choices")) {
                if (!entry["choices"].is_array()) return Fail(error, "dialogue.bad_json: 'choices' is a list");
                for (const Json& c : entry["choices"]) {
                    if (!c.is_object()) return Fail(error, "dialogue.bad_json: every choice is an object");
                    Choice choice;
                    if (!ReadString(c, "text", choice.text, error) || !ReadString(c, "next", choice.next, error)) return false;
                    node.choices.push_back(std::move(choice));
                }
            }
            dialogue.nodes.push_back(std::move(node));
        }
    }
    if (dialogue.start.empty() && !dialogue.nodes.empty()) dialogue.start = dialogue.nodes.front().id;
    const std::string problem = dialogue.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(dialogue);
    return true;
}

bool DialogueLibrary::Register(DialogueDef dialogue, std::string* error) {
    const std::string problem = dialogue.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = dialogue.name;
    dialogues_[name] = std::move(dialogue);
    return true;
}

const DialogueDef* DialogueLibrary::Find(const std::string& name) const {
    const auto it = dialogues_.find(name);
    return it == dialogues_.end() ? nullptr : &it->second;
}

std::vector<std::string> DialogueLibrary::Names() const {
    std::vector<std::string> names;
    names.reserve(dialogues_.size());
    for (const auto& entry : dialogues_) names.push_back(entry.first);
    return names;
}

} // namespace aether::dlg
