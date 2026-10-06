#include "aether/inventory/item_def.h"

#include <cmath>

namespace aether::inv {

using reflect::Json;

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool ReadString(const Json& j, const char* key, std::string& out, std::string* error) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return Fail(error, std::string("item.bad_json: '") + key + "' is a string");
    out = j[key].get<std::string>();
    return true;
}

} // namespace

std::string ItemDef::Validate() const {
    if (name.empty()) return "item.name_empty: an item needs a name";
    if (max_stack < 1) return "item.bad_stack: max_stack is at least 1";
    if (!std::isfinite(weight) || weight < 0.0f) return "item.bad_weight: weight is 0 or above";
    for (const gas::GameplayTag& t : tags) {
        if (!t.IsValid() || !gas::GameplayTag::ValidName(t.name)) return "item.bad_tag: '" + t.name + "' isn't a valid tag name";
    }
    if (!equip_effects.empty() && equip_slot.empty()) return "item.equip_effects_without_slot: equip_effects need an equip_slot";
    return {};
}

Json ItemToJson(const ItemDef& item) {
    Json j = {{"name", item.name}, {"max_stack", item.max_stack}};
    if (!item.display_key.empty()) j["display_key"] = item.display_key;
    if (!item.icon.empty()) j["icon"] = item.icon;
    if (item.weight != 0.0f) j["weight"] = item.weight;
    if (!item.tags.empty()) {
        Json list = Json::array();
        for (const gas::GameplayTag& t : item.tags) list.push_back(t.name);
        j["tags"] = std::move(list);
    }
    if (!item.equip_slot.empty()) j["equip_slot"] = item.equip_slot;
    if (!item.equip_effects.empty()) j["equip_effects"] = item.equip_effects;
    if (!item.use_effect.empty()) j["use_effect"] = item.use_effect;
    if (!item.consume_on_use) j["consume_on_use"] = false;
    return j;
}

bool ItemFromJson(const std::string& text, ItemDef& out, std::string* error) {
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_object()) return Fail(error, "item.bad_json: an item is a JSON object");
    ItemDef item;
    if (!ReadString(j, "name", item.name, error) || !ReadString(j, "display_key", item.display_key, error) || !ReadString(j, "icon", item.icon, error) ||
        !ReadString(j, "equip_slot", item.equip_slot, error) || !ReadString(j, "use_effect", item.use_effect, error)) {
        return false;
    }
    if (j.contains("max_stack")) {
        if (!j["max_stack"].is_number_integer()) return Fail(error, "item.bad_stack: 'max_stack' is a whole number");
        item.max_stack = j["max_stack"].get<i32>();
    }
    if (j.contains("weight")) {
        if (!j["weight"].is_number()) return Fail(error, "item.bad_weight: 'weight' is a number");
        item.weight = j["weight"].get<f32>();
    }
    if (j.contains("consume_on_use")) {
        if (!j["consume_on_use"].is_boolean()) return Fail(error, "item.bad_json: 'consume_on_use' is true or false");
        item.consume_on_use = j["consume_on_use"].get<bool>();
    }
    if (j.contains("tags")) {
        if (!j["tags"].is_array()) return Fail(error, "item.bad_tag: 'tags' is a list of tag names");
        for (const Json& t : j["tags"]) {
            if (!t.is_string() || !gas::GameplayTag::ValidName(t.get<std::string>())) return Fail(error, "item.bad_tag: '" + (t.is_string() ? t.get<std::string>() : t.dump()) + "' isn't a valid tag name");
            item.tags.push_back(gas::GameplayTag{t.get<std::string>()});
        }
    }
    if (j.contains("equip_effects")) {
        if (!j["equip_effects"].is_array()) return Fail(error, "item.bad_json: 'equip_effects' is a list of effect names");
        for (const Json& e : j["equip_effects"]) {
            if (!e.is_string()) return Fail(error, "item.bad_json: 'equip_effects' is a list of effect names");
            item.equip_effects.push_back(e.get<std::string>());
        }
    }
    const std::string problem = item.Validate();
    if (!problem.empty()) return Fail(error, problem);
    out = std::move(item);
    return true;
}

bool ItemLibrary::Register(ItemDef item, std::string* error) {
    const std::string problem = item.Validate();
    if (!problem.empty()) return Fail(error, problem);
    const std::string name = item.name;
    items_[name] = std::move(item);
    return true;
}

const ItemDef* ItemLibrary::Find(const std::string& name) const {
    const auto it = items_.find(name);
    return it == items_.end() ? nullptr : &it->second;
}

std::string ItemLibrary::CheckEffects(const ItemDef& item, const gas::EffectLibrary& effects) {
    for (const std::string& e : item.equip_effects) {
        if (effects.Find(e) == nullptr) return "item.unknown_effect: the equip effect '" + e + "' isn't an effect";
    }
    if (!item.use_effect.empty() && effects.Find(item.use_effect) == nullptr) return "item.unknown_effect: the use effect '" + item.use_effect + "' isn't an effect";
    return {};
}

} // namespace aether::inv
