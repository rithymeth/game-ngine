#include "aether/achievements/achievement_def.h"

namespace aether::ach {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool ReadString(const Json& j, const char* key, std::string& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return Fail(error, std::string("achievement.bad_json: '") + key + "' is a string");
    out = j[key].get<std::string>();
    return true;
}

} // namespace

std::string AchievementDef::Validate() const {
    if (name.empty()) return "achievement.name_empty: an achievement needs a name";
    if (target < 1 || target > kMaxTarget) {
        return "achievement.bad_target: the target of '" + name + "' is between 1 and " + std::to_string(kMaxTarget);
    }
    return {};
}

Json AchievementToJson(const AchievementDef& a) {
    Json j = {{"name", a.name}, {"target", a.target}};
    if (!a.title.empty()) j["title"] = a.title;
    if (!a.description.empty()) j["description"] = a.description;
    if (a.hidden) j["hidden"] = true;
    return j;
}

bool AchievementFromJson(const std::string& text, AchievementDef& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "achievement.bad_json: an achievement is a JSON object");
    AchievementDef a;
    if (!ReadString(j, "name", a.name, error) || !ReadString(j, "title", a.title, error) || !ReadString(j, "description", a.description, error)) return false;
    if (j.contains("target")) {
        if (!j["target"].is_number_integer()) return Fail(error, "achievement.bad_target: 'target' is a whole number");
        const i64 target = j["target"].get<i64>();
        a.target = target < 0 ? 0 : (target > AchievementDef::kMaxTarget + 1 ? AchievementDef::kMaxTarget + 1 : static_cast<i32>(target)); // out of range: Validate names it
    }
    if (j.contains("hidden")) {
        if (!j["hidden"].is_boolean()) return Fail(error, "achievement.bad_json: 'hidden' is true or false");
        a.hidden = j["hidden"].get<bool>();
    }
    const std::string problem = a.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(a);
    return true;
}

bool AchievementLibrary::Register(AchievementDef achievement, std::string* error) {
    const std::string problem = achievement.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = achievement.name;
    achievements_[name] = std::move(achievement);
    return true;
}

const AchievementDef* AchievementLibrary::Find(const std::string& name) const {
    const auto it = achievements_.find(name);
    return it == achievements_.end() ? nullptr : &it->second;
}

std::vector<std::string> AchievementLibrary::Names() const {
    std::vector<std::string> names;
    names.reserve(achievements_.size());
    for (const auto& entry : achievements_) names.push_back(entry.first);
    return names;
}

} // namespace aether::ach
