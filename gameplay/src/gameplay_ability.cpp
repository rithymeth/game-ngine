#include "aether/gameplay/gameplay_ability.h"

#include <cmath>

namespace aether::gas {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

Json TagList(const std::vector<GameplayTag>& tags) {
    Json list = Json::array();
    for (const GameplayTag& t : tags) list.push_back(t.name);
    return list;
}

bool ReadTags(const Json& j, const char* key, std::vector<GameplayTag>& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return Fail(error, std::string("ability.bad_tag: '") + key + "' is a list of tag names");
    for (const Json& t : j[key]) {
        if (!t.is_string() || !GameplayTag::ValidName(t.get<std::string>())) return Fail(error, "ability.bad_tag: '" + (t.is_string() ? t.get<std::string>() : t.dump()) + "' isn't a valid tag name");
        out.push_back(GameplayTag{t.get<std::string>()});
    }
    return true;
}

bool ReadQuery(const Json& j, const char* key, TagQuery& out, std::string* error) {
    if (!j.contains(key)) return true;
    std::string inner;
    if (!TagQuery::FromJson(j[key], out, &inner)) return Fail(error, std::string("ability.bad_tag: '") + key + "': " + inner);
    return true;
}

bool ReadName(const Json& j, const char* key, std::string& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return Fail(error, std::string("ability.bad_json: '") + key + "' is a string");
    out = j[key].get<std::string>();
    return true;
}

} // namespace

std::string GameplayAbility::Validate() const {
    if (name.empty()) return "ability.name_empty: an ability needs a name";
    if (!std::isfinite(max_duration) || max_duration < 0.0f) return "ability.bad_duration: max_duration is 0 (none) or above";
    return {};
}

Json AbilityToJson(const GameplayAbility& a) {
    Json j = {{"name", a.name}};
    if (!a.tags.empty()) j["tags"] = TagList(a.tags);
    if (!a.activation_required.IsEmpty()) j["activation_required"] = a.activation_required.ToJson();
    if (!a.activation_blocked.IsEmpty()) j["activation_blocked"] = a.activation_blocked.ToJson();
    if (!a.cancel_abilities_with_tags.IsEmpty()) j["cancel_abilities_with_tags"] = a.cancel_abilities_with_tags.ToJson();
    if (!a.block_abilities_with_tags.IsEmpty()) j["block_abilities_with_tags"] = a.block_abilities_with_tags.ToJson();
    if (!a.activation_owned_tags.empty()) j["activation_owned_tags"] = TagList(a.activation_owned_tags);
    if (!a.cost.empty()) j["cost"] = a.cost;
    if (!a.cooldown.empty()) j["cooldown"] = a.cooldown;
    if (a.max_duration > 0.0f) j["max_duration"] = a.max_duration;
    if (!a.commit_on_activate) j["commit_on_activate"] = false;
    return j;
}

bool AbilityFromJson(const std::string& text, GameplayAbility& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "ability.bad_json: an ability is a JSON object");
    GameplayAbility a;
    if (!ReadName(j, "name", a.name, error) || !ReadName(j, "cost", a.cost, error) || !ReadName(j, "cooldown", a.cooldown, error)) return false;
    if (j.contains("max_duration")) {
        if (!j["max_duration"].is_number()) return Fail(error, "ability.bad_duration: 'max_duration' is a number of seconds");
        a.max_duration = j["max_duration"].get<f32>();
    }
    if (j.contains("commit_on_activate")) {
        if (!j["commit_on_activate"].is_boolean()) return Fail(error, "ability.bad_json: 'commit_on_activate' is true or false");
        a.commit_on_activate = j["commit_on_activate"].get<bool>();
    }
    if (!ReadTags(j, "tags", a.tags, error) || !ReadTags(j, "activation_owned_tags", a.activation_owned_tags, error)) return false;
    if (!ReadQuery(j, "activation_required", a.activation_required, error) || !ReadQuery(j, "activation_blocked", a.activation_blocked, error) ||
        !ReadQuery(j, "cancel_abilities_with_tags", a.cancel_abilities_with_tags, error) || !ReadQuery(j, "block_abilities_with_tags", a.block_abilities_with_tags, error)) {
        return false;
    }
    const std::string problem = a.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(a);
    return true;
}

bool AbilityLibrary::Register(GameplayAbility ability, std::string* error) {
    const std::string problem = ability.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = ability.name;
    abilities_[name] = std::move(ability);
    return true;
}

const GameplayAbility* AbilityLibrary::Find(const std::string& name) const {
    const auto it = abilities_.find(name);
    return it == abilities_.end() ? nullptr : &it->second;
}

std::string AbilityLibrary::CheckEffects(const GameplayAbility& ability, const EffectLibrary& effects) {
    if (!ability.cost.empty()) {
        const GameplayEffect* e = effects.Find(ability.cost);
        if (e == nullptr) return "ability.unknown_effect: the cost '" + ability.cost + "' isn't an effect";
        if (e->duration_policy != GameplayEffect::Duration::Instant) return "ability.cost_not_instant: the cost '" + ability.cost + "' must be an instant effect";
    }
    if (!ability.cooldown.empty()) {
        const GameplayEffect* e = effects.Find(ability.cooldown);
        if (e == nullptr) return "ability.unknown_effect: the cooldown '" + ability.cooldown + "' isn't an effect";
        if (e->duration_policy != GameplayEffect::Duration::Timed || e->granted_tags.empty()) return "ability.cooldown_not_timed: the cooldown '" + ability.cooldown + "' must be a timed effect that grants a tag";
    }
    return {};
}

} // namespace aether::gas
