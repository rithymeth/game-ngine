#include "aether/crafting/recipe_def.h"

#include <set>

namespace aether::craft {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

std::string CheckLines(const std::vector<Ingredient>& lines, const char* what) {
    std::set<std::string> seen;
    for (const Ingredient& line : lines) {
        if (line.item.empty()) return std::string("recipe.item_empty: an ") + what + " needs an item name";
        if (line.count < 1 || line.count > RecipeDef::kMaxCount) {
            return "recipe.bad_count: the count of '" + line.item + "' is between 1 and " + std::to_string(RecipeDef::kMaxCount);
        }
        if (!seen.insert(line.item).second) return "recipe.duplicate_item: '" + line.item + "' is listed twice among the " + what + "s";
    }
    return {};
}

Json LinesToJson(const std::vector<Ingredient>& lines) {
    Json list = Json::array();
    for (const Ingredient& line : lines) list.push_back({{"item", line.item}, {"count", line.count}});
    return list;
}

bool LinesFromJson(const Json& j, const char* key, std::vector<Ingredient>& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return Fail(error, std::string("recipe.bad_json: '") + key + "' is a list of {item, count}");
    for (const Json& entry : j[key]) {
        if (!entry.is_object() || !entry.contains("item") || !entry["item"].is_string()) return Fail(error, std::string("recipe.bad_json: every entry of '") + key + "' has an item name");
        Ingredient line;
        line.item = entry["item"].get<std::string>();
        if (entry.contains("count")) {
            if (!entry["count"].is_number_integer()) return Fail(error, "recipe.bad_count: the count of '" + line.item + "' is a whole number");
            const i64 count = entry["count"].get<i64>();
            line.count = count < 0 ? 0 : (count > RecipeDef::kMaxCount + 1 ? RecipeDef::kMaxCount + 1 : static_cast<i32>(count)); // out of range: Validate names it
        }
        out.push_back(std::move(line));
    }
    return true;
}

} // namespace

std::string RecipeDef::Validate() const {
    if (name.empty()) return "recipe.name_empty: a recipe needs a name";
    if (inputs.empty()) return "recipe.no_inputs: '" + name + "' takes nothing (a recipe needs at least one input)";
    if (outputs.empty()) return "recipe.no_outputs: '" + name + "' makes nothing";
    if (std::string problem = CheckLines(inputs, "input"); !problem.empty()) return problem;
    return CheckLines(outputs, "output");
}

Json RecipeToJson(const RecipeDef& recipe) {
    Json j = {{"name", recipe.name}, {"inputs", LinesToJson(recipe.inputs)}, {"outputs", LinesToJson(recipe.outputs)}};
    if (!recipe.title.empty()) j["title"] = recipe.title;
    return j;
}

bool RecipeFromJson(const std::string& text, RecipeDef& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "recipe.bad_json: a recipe is a JSON object");
    RecipeDef recipe;
    for (const auto& [key, field] : std::initializer_list<std::pair<const char*, std::string*>>{{"name", &recipe.name}, {"title", &recipe.title}}) {
        if (!j.contains(key)) continue;
        if (!j[key].is_string()) return Fail(error, std::string("recipe.bad_json: '") + key + "' is a string");
        *field = j[key].get<std::string>();
    }
    if (!LinesFromJson(j, "inputs", recipe.inputs, error) || !LinesFromJson(j, "outputs", recipe.outputs, error)) return false;
    const std::string problem = recipe.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(recipe);
    return true;
}

bool RecipeLibrary::Register(RecipeDef recipe, std::string* error) {
    const std::string problem = recipe.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = recipe.name;
    recipes_[name] = std::move(recipe);
    return true;
}

const RecipeDef* RecipeLibrary::Find(const std::string& name) const {
    const auto it = recipes_.find(name);
    return it == recipes_.end() ? nullptr : &it->second;
}

std::vector<std::string> RecipeLibrary::Names() const {
    std::vector<std::string> names;
    names.reserve(recipes_.size());
    for (const auto& entry : recipes_) names.push_back(entry.first);
    return names;
}

std::string RecipeLibrary::CheckItems(const RecipeDef& recipe, const inv::ItemLibrary& items) {
    for (const auto* lines : {&recipe.inputs, &recipe.outputs}) {
        for (const Ingredient& line : *lines) {
            if (items.Find(line.item) == nullptr) return "recipe.unknown_item: '" + recipe.name + "' names '" + line.item + "', which isn't an item";
        }
    }
    return {};
}

std::vector<std::string> RecipeLibrary::Check(const inv::ItemLibrary& items) const {
    std::vector<std::string> problems;
    for (const auto& entry : recipes_) {
        std::string problem = CheckItems(entry.second, items);
        if (!problem.empty()) problems.push_back(std::move(problem));
    }
    return problems;
}

} // namespace aether::craft
