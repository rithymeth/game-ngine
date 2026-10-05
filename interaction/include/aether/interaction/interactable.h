#pragma once

#include "aether/core/base.h"
#include "aether/reflection/reflection.h"

#include <string>
#include <vector>

namespace aether::interact {

// Something an entity can walk up to and use (Phase 30 step 7b, §30.8): a door, a lever,
// a chest, a person to talk to. A component, saved with the entity. What using it does is
// up to its owner's script or Blueprint (it hears OnInteract), plus the optional effect
// and ability applied to whoever used it.
struct Interactable {
    bool enabled = true;
    std::string prompt = "Interact";  // the shown text, when there is no key (or no translation)
    std::string prompt_key;           // a localization key for the prompt
    f32 range = 2.0f;                 // how close the user must be; 0 for no limit
    std::vector<std::string> required_tags; // the user must have all of these (hierarchical)
    std::vector<std::string> blocked_tags;  // and none of these
    std::string effect;               // an effect applied to the user (source: this entity), optional
    std::string ability;              // an ability the user activates, optional
    bool one_shot = false;            // can be used once
    f32 cooldown = 0.0f;              // seconds before it can be used again
    // Runtime state (saved, so a used door stays used).
    f32 remaining = 0.0f;
    bool used = false;
};

} // namespace aether::interact

AETHER_REFLECT(aether::interact::Interactable, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(prompt, Field_EditAnywhere, {.tooltip = "The text shown when you can use it"}),
    AETHER_FIELD(prompt_key, Field_EditAnywhere, {.tooltip = "A localization key for the prompt"}),
    AETHER_FIELD(range, Field_EditAnywhere, {.tooltip = "How close the user must be; 0 for no limit"}),
    AETHER_FIELD(required_tags, Field_EditAnywhere, {.tooltip = "The user must have all of these tags"}),
    AETHER_FIELD(blocked_tags, Field_EditAnywhere, {.tooltip = "The user must have none of these tags"}),
    AETHER_FIELD(effect, Field_EditAnywhere, {.tooltip = "An effect applied to the user"}),
    AETHER_FIELD(ability, Field_EditAnywhere, {.tooltip = "An ability the user activates"}),
    AETHER_FIELD(one_shot, Field_EditAnywhere),
    AETHER_FIELD(cooldown, Field_EditAnywhere),
    AETHER_FIELD(remaining, Field_EditAnywhere),
    AETHER_FIELD(used, Field_EditAnywhere))
