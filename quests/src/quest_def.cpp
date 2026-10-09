#include "aether/quests/quest_def.h"

#include <algorithm>
#include <set>

namespace aether::quest {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

const char* KindName(Objective::Kind k) {
    switch (k) {
    case Objective::Kind::Count: return "count";
    case Objective::Kind::Tag: return "tag";
    case Objective::Kind::Flag: return "flag";
    }
    return "count";
}

bool ReadString(const Json& j, const char* key, std::string& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return Fail(error, std::string("quest.parse: '") + key + "' is a string");
    out = j[key].get<std::string>();
    return true;
}

bool ReadStrings(const Json& j, const char* key, std::vector<std::string>& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return Fail(error, std::string("quest.parse: '") + key + "' is a list of names");
    for (const Json& v : j[key]) {
        if (!v.is_string()) return Fail(error, std::string("quest.parse: '") + key + "' is a list of names");
        out.push_back(v.get<std::string>());
    }
    return true;
}

} // namespace

std::string QuestDef::Validate() const {
    if (name.empty()) return "quest.no_name: a quest needs a name";
    std::set<std::string> ids;
    for (const Objective& o : objectives) {
        if (o.id.empty() || !ids.insert(o.id).second) return "quest.duplicate_objective: objective ids are unique and not empty ('" + o.id + "')";
        if (o.required < 1) return "quest.bad_required: objective '" + o.id + "' needs required of at least 1";
    }
    for (const RewardItem& r : reward_items) {
        if (r.item.empty() || r.count < 1) return "quest.parse: a reward item needs a name and a count of at least 1";
    }
    return {};
}

Json QuestToJson(const QuestDef& q) {
    Json j = {{"name", q.name}};
    if (!q.title.empty()) j["title"] = q.title;
    if (!q.title_key.empty()) j["title_key"] = q.title_key;
    if (!q.description.empty()) j["description"] = q.description;
    if (!q.description_key.empty()) j["description_key"] = q.description_key;
    Json objs = Json::array();
    for (const Objective& o : q.objectives) {
        Json oj = {{"id", o.id}, {"kind", KindName(o.kind)}, {"target", o.target}, {"required", o.required}};
        if (!o.text.empty()) oj["text"] = o.text;
        if (!o.text_key.empty()) oj["text_key"] = o.text_key;
        if (o.optional) oj["optional"] = true;
        objs.push_back(std::move(oj));
    }
    j["objectives"] = std::move(objs);
    if (!q.prerequisites.empty()) j["prerequisites"] = q.prerequisites;
    Json rewards = Json::object();
    if (!q.reward_effects.empty()) rewards["effects"] = q.reward_effects;
    if (!q.reward_items.empty()) {
        Json items = Json::array();
        for (const RewardItem& r : q.reward_items) items.push_back({{"item", r.item}, {"count", r.count}});
        rewards["items"] = std::move(items);
    }
    if (!rewards.empty()) j["rewards"] = std::move(rewards);
    return j;
}

bool QuestFromJson(const std::string& text, QuestDef& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "quest.parse: a quest is a JSON object");
    QuestDef q;
    if (!ReadString(j, "name", q.name, error) || !ReadString(j, "title", q.title, error) || !ReadString(j, "title_key", q.title_key, error) ||
        !ReadString(j, "description", q.description, error) || !ReadString(j, "description_key", q.description_key, error) || !ReadStrings(j, "prerequisites", q.prerequisites, error)) {
        return false;
    }
    if (j.contains("objectives")) {
        if (!j["objectives"].is_array()) return Fail(error, "quest.parse: 'objectives' is a list");
        for (const Json& oj : j["objectives"]) {
            if (!oj.is_object()) return Fail(error, "quest.parse: an objective is an object");
            Objective o;
            if (!ReadString(oj, "id", o.id, error) || !ReadString(oj, "text", o.text, error) || !ReadString(oj, "text_key", o.text_key, error) || !ReadString(oj, "target", o.target, error)) return false;
            if (oj.contains("kind") && !oj["kind"].is_string())
                return Fail(error, "quest.parse: objective 'kind' is a string");
            const std::string kind = oj.contains("kind") ? oj["kind"].get<std::string>() : "count";
            if (kind == "count") o.kind = Objective::Kind::Count;
            else if (kind == "tag") o.kind = Objective::Kind::Tag;
            else if (kind == "flag") o.kind = Objective::Kind::Flag;
            else return Fail(error, "quest.bad_kind: objective kind is count, tag or flag, not '" + kind + "'");
            if (oj.contains("required")) {
                if (!oj["required"].is_number_integer()) return Fail(error, "quest.bad_required: 'required' is a whole number");
                o.required = oj["required"].get<i32>();
            }
            if (oj.contains("optional")) {
                if (!oj["optional"].is_boolean())
                    return Fail(error, "quest.parse: objective 'optional' is a boolean");
                o.optional = oj["optional"].get<bool>();
            }
            q.objectives.push_back(std::move(o));
        }
    }
    if (j.contains("rewards")) {
        const Json& r = j["rewards"];
        if (!r.is_object()) return Fail(error, "quest.parse: 'rewards' is an object");
        if (!ReadStrings(r, "effects", q.reward_effects, error)) return false;
        if (r.contains("items")) {
            if (!r["items"].is_array()) return Fail(error, "quest.parse: reward 'items' is a list");
            for (const Json& ij : r["items"]) {
                if (!ij.is_object() || !ij.contains("item") || !ij["item"].is_string()) return Fail(error, "quest.parse: a reward item has an 'item' and a 'count'");
                i32 count = 1;
                if (ij.contains("count")) {
                    if (!ij["count"].is_number_integer())
                        return Fail(error, "quest.parse: reward item 'count' is a whole number");
                    count = ij["count"].get<i32>();
                }
                q.reward_items.push_back({ij["item"].get<std::string>(), count});
            }
        }
    }
    const std::string problem = q.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(q);
    return true;
}

bool QuestLibrary::Register(QuestDef quest, std::string* error) {
    const std::string problem = quest.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = quest.name;
    quests_[name] = std::move(quest);
    return true;
}

const QuestDef* QuestLibrary::Find(const std::string& name) const {
    const auto it = quests_.find(name);
    return it == quests_.end() ? nullptr : &it->second;
}

std::vector<std::string> QuestLibrary::Check(const gas::EffectLibrary* effects) const {
    std::vector<std::string> problems;
    for (const auto& [name, q] : quests_) {
        for (const std::string& p : q.prerequisites) {
            if (quests_.find(p) == quests_.end()) problems.push_back("quest.unknown_prereq: quest '" + name + "' needs '" + p + "', which isn't a quest");
        }
        if (effects != nullptr) {
            for (const std::string& e : q.reward_effects) {
                if (effects->Find(e) == nullptr) problems.push_back("quest.unknown_effect: quest '" + name + "' rewards '" + e + "', which isn't an effect");
            }
        }
    }
    // A prerequisite cycle: follow each quest's prerequisites and see if it comes back to itself.
    for (const auto& [name, q] : quests_) {
        std::set<std::string> seen;
        std::vector<std::string> stack(q.prerequisites.begin(), q.prerequisites.end());
        while (!stack.empty()) {
            const std::string cur = stack.back();
            stack.pop_back();
            if (cur == name) {
                problems.push_back("quest.prereq_cycle: quest '" + name + "' is a prerequisite of itself");
                break;
            }
            if (!seen.insert(cur).second) continue;
            const auto it = quests_.find(cur);
            if (it != quests_.end()) stack.insert(stack.end(), it->second.prerequisites.begin(), it->second.prerequisites.end());
        }
    }
    return problems;
}

} // namespace aether::quest
