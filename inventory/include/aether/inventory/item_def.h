#pragma once

#include "aether/core/base.h"
#include "aether/gameplay/gameplay_effect.h"
#include "aether/gameplay/gameplay_tag.h"

#include <map>
#include <string>
#include <vector>

namespace aether::inv {

// An item definition (Phase 30 step 7a, §30.8, `.aitem`): what a kind of item is,
// how it stacks, what it weighs, and what it does when used or equipped.
struct ItemDef {
    std::string name;
    std::string display_key;       // localization key for the shown name (empty: the name)
    std::string icon;              // an asset path (optional)
    i32 max_stack = 1;
    f32 weight = 0.0f;             // per item
    std::vector<gas::GameplayTag> tags;
    std::string equip_slot;        // where it is worn ("Head", "MainHand"); empty: not equippable
    std::vector<std::string> equip_effects; // effects active while equipped
    std::string use_effect;        // an effect applied to the user by Use (optional)
    bool consume_on_use = true;

    // Empty string if valid, else a named error ("item.name_empty", ...).
    std::string Validate() const;
    bool operator==(const ItemDef&) const = default;
};

reflect::Json ItemToJson(const ItemDef& item);
// False (with `error`, "item.<code>: message") for bad JSON or an invalid item.
bool ItemFromJson(const std::string& text, ItemDef& out, std::string* error = nullptr);

// Items by name: filled from cooked `.aitem` assets by the player, or by code.
class ItemLibrary {
public:
    bool Register(ItemDef item, std::string* error = nullptr);
    const ItemDef* Find(const std::string& name) const;
    void Clear() { items_.clear(); }
    usize Size() const { return items_.size(); }

    // The effects an item names must exist: "item.unknown_effect". Empty string if fine.
    static std::string CheckEffects(const ItemDef& item, const gas::EffectLibrary& effects);

private:
    std::map<std::string, ItemDef> items_;
};

} // namespace aether::inv
