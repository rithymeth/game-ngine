// Fuzz target: the gameplay data loaders (Phase 47 step 1): effects, abilities, tag queries and (when
// built) items and quests. The first input byte picks the loader; the rest is the JSON text.
#include "fuzz_common.h"

#include "aether/gameplay/gameplay_ability.h"
#include "aether/gameplay/gameplay_effect.h"
#include "aether/gameplay/tag_query.h"
#if AETHER_FUZZ_INVENTORY
#include "aether/inventory/item_def.h"
#endif
#if AETHER_FUZZ_QUESTS
#include "aether/quests/quest_def.h"
#endif

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    const std::string text(reinterpret_cast<const char*>(data + 1), size - 1);
    std::string error;
    switch (data[0] % 5) {
    case 0: {
        aether::gas::GameplayEffect e;
        if (aether::gas::EffectFromJson(text, e, &error)) (void)aether::gas::EffectToJson(e);
        break;
    }
    case 1: {
        aether::gas::GameplayAbility a;
        if (aether::gas::AbilityFromJson(text, a, &error)) (void)aether::gas::AbilityToJson(a);
        break;
    }
    case 2: {
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        aether::gas::TagQuery q;
        if (!j.is_discarded() && aether::gas::TagQuery::FromJson(j, q, &error)) (void)q.ToJson();
        break;
    }
#if AETHER_FUZZ_INVENTORY
    case 3: {
        aether::inv::ItemDef item;
        if (aether::inv::ItemFromJson(text, item, &error)) (void)aether::inv::ItemToJson(item);
        break;
    }
#endif
#if AETHER_FUZZ_QUESTS
    case 4: {
        aether::quest::QuestDef quest;
        if (aether::quest::QuestFromJson(text, quest, &error)) (void)aether::quest::QuestToJson(quest);
        break;
    }
#endif
    default: break;
    }
    return 0;
}
