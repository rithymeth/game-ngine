#include "aether/gameplay/gameplay_effect.h"

#include <cmath>

namespace aether::gas {

using reflect::Json;

namespace {

const char* DurationName(GameplayEffect::Duration d) {
    switch (d) {
    case GameplayEffect::Duration::Instant: return "instant";
    case GameplayEffect::Duration::Timed: return "timed";
    case GameplayEffect::Duration::Infinite: return "infinite";
    }
    return "instant";
}
const char* OpName(GameplayEffect::Op o) {
    switch (o) {
    case GameplayEffect::Op::Add: return "add";
    case GameplayEffect::Op::Multiply: return "multiply";
    case GameplayEffect::Op::Override: return "override";
    }
    return "add";
}
const char* StackingName(GameplayEffect::Stacking s) {
    switch (s) {
    case GameplayEffect::Stacking::None: return "none";
    case GameplayEffect::Stacking::Refresh: return "refresh";
    case GameplayEffect::Stacking::StackCount: return "stack";
    }
    return "none";
}

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
    if (!j[key].is_array()) return Fail(error, std::string("effect.bad_tag: '") + key + "' is a list of tag names");
    for (const Json& t : j[key]) {
        if (!t.is_string() || !GameplayTag::ValidName(t.get<std::string>())) return Fail(error, "effect.bad_tag: '" + (t.is_string() ? t.get<std::string>() : t.dump()) + "' isn't a valid tag name");
        out.push_back(GameplayTag{t.get<std::string>()});
    }
    return true;
}

bool ReadQuery(const Json& j, const char* key, TagQuery& out, std::string* error) {
    if (!j.contains(key)) return true;
    std::string inner;
    if (!TagQuery::FromJson(j[key], out, &inner)) return Fail(error, std::string("effect.bad_tag: '") + key + "': " + inner);
    return true;
}

} // namespace

std::string GameplayEffect::Validate() const {
    if (name.empty()) return "effect.name_empty: an effect needs a name";
    if (duration_policy == Duration::Timed && !(duration > 0.0f)) return "effect.bad_duration: a timed effect needs a duration above 0";
    if (std::isnan(duration) || std::isinf(duration)) return "effect.bad_duration: the duration isn't a number";
    for (const Modifier& m : modifiers) {
        if (m.attribute.empty() || !std::isfinite(m.magnitude)) return "effect.bad_modifier: a modifier needs an attribute and a finite magnitude";
    }
    if (!std::isfinite(period) || period < 0.0f) return "effect.bad_period: the period is 0 (none) or above";
    if (period > 0.0f && duration_policy == Duration::Instant) return "effect.period_on_instant: an instant effect has no period";
    if (max_stacks < 1) return "effect.bad_stack: max_stacks is at least 1";
    return {};
}

Json EffectToJson(const GameplayEffect& e) {
    Json j = {{"name", e.name}, {"duration_policy", DurationName(e.duration_policy)}, {"stacking", StackingName(e.stacking)}};
    if (e.duration_policy == GameplayEffect::Duration::Timed) j["duration"] = e.duration;
    Json mods = Json::array();
    for (const auto& m : e.modifiers) mods.push_back({{"attribute", m.attribute}, {"op", OpName(m.op)}, {"magnitude", m.magnitude}});
    j["modifiers"] = std::move(mods);
    if (e.stacking == GameplayEffect::Stacking::StackCount) j["max_stacks"] = e.max_stacks;
    if (e.per_source) j["per_source"] = true;
    if (e.period > 0.0f) j["period"] = e.period;
    if (e.execute_on_apply) j["execute_on_apply"] = true;
    if (!e.require.IsEmpty()) j["require"] = e.require.ToJson();
    if (!e.blocked.IsEmpty()) j["blocked"] = e.blocked.ToJson();
    if (!e.granted_tags.empty()) j["granted_tags"] = TagList(e.granted_tags);
    if (!e.remove_on_tags.empty()) j["remove_on_tags"] = TagList(e.remove_on_tags);
    return j;
}

bool EffectFromJson(const std::string& text, GameplayEffect& out, std::string* error) {
    Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "effect.bad_json: an effect is a JSON object");
    GameplayEffect e;
    if (j.contains("name")) {
        if (!j["name"].is_string()) return Fail(error, "effect.name_empty: 'name' is a string");
        e.name = j["name"].get<std::string>();
    }
    if (j.contains("duration_policy")) {
        const std::string d = j["duration_policy"].is_string() ? j["duration_policy"].get<std::string>() : "";
        if (d == "instant") e.duration_policy = GameplayEffect::Duration::Instant;
        else if (d == "timed") e.duration_policy = GameplayEffect::Duration::Timed;
        else if (d == "infinite") e.duration_policy = GameplayEffect::Duration::Infinite;
        else return Fail(error, "effect.bad_duration: duration_policy is instant, timed or infinite");
    }
    if (j.contains("duration")) {
        if (!j["duration"].is_number()) return Fail(error, "effect.bad_duration: 'duration' is a number of seconds");
        e.duration = j["duration"].get<f32>();
    }
    if (j.contains("modifiers")) {
        if (!j["modifiers"].is_array()) return Fail(error, "effect.bad_modifier: 'modifiers' is a list");
        for (const Json& m : j["modifiers"]) {
            if (!m.is_object() || !m.contains("attribute") || !m["attribute"].is_string() || !m.contains("magnitude") || !m["magnitude"].is_number()) return Fail(error, "effect.bad_modifier: a modifier has an 'attribute' and a 'magnitude'");
            GameplayEffect::Modifier mod;
            mod.attribute = m["attribute"].get<std::string>();
            mod.magnitude = m["magnitude"].get<f32>();
            const std::string op = m.contains("op") && m["op"].is_string() ? m["op"].get<std::string>() : "add";
            if (op == "add") mod.op = GameplayEffect::Op::Add;
            else if (op == "multiply") mod.op = GameplayEffect::Op::Multiply;
            else if (op == "override") mod.op = GameplayEffect::Op::Override;
            else return Fail(error, "effect.bad_op: op is add, multiply or override, not '" + op + "'");
            e.modifiers.push_back(std::move(mod));
        }
    }
    if (j.contains("stacking")) {
        const std::string s = j["stacking"].is_string() ? j["stacking"].get<std::string>() : "";
        if (s == "none") e.stacking = GameplayEffect::Stacking::None;
        else if (s == "refresh") e.stacking = GameplayEffect::Stacking::Refresh;
        else if (s == "stack") e.stacking = GameplayEffect::Stacking::StackCount;
        else return Fail(error, "effect.bad_stack: stacking is none, refresh or stack");
    }
    if (j.contains("max_stacks")) {
        if (!j["max_stacks"].is_number_integer()) return Fail(error, "effect.bad_stack: 'max_stacks' is a whole number");
        e.max_stacks = j["max_stacks"].get<i32>();
    }
    e.per_source = j.value("per_source", false);
    e.execute_on_apply = j.value("execute_on_apply", false);
    if (j.contains("period")) {
        if (!j["period"].is_number()) return Fail(error, "effect.bad_period: 'period' is a number of seconds");
        e.period = j["period"].get<f32>();
    }
    if (!ReadQuery(j, "require", e.require, error) || !ReadQuery(j, "blocked", e.blocked, error)) return false;
    if (!ReadTags(j, "granted_tags", e.granted_tags, error) || !ReadTags(j, "remove_on_tags", e.remove_on_tags, error)) return false;
    const std::string problem = e.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(e);
    return true;
}

bool EffectLibrary::Register(GameplayEffect effect, std::string* error) {
    const std::string problem = effect.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = effect.name;
    effects_[name] = std::move(effect);
    return true;
}

const GameplayEffect* EffectLibrary::Find(const std::string& name) const {
    const auto it = effects_.find(name);
    return it == effects_.end() ? nullptr : &it->second;
}

} // namespace aether::gas
