#pragma once

#include "aether/kit/kit.h"
#include "aether/quests/quest_system.h"
#include "aether/scene/scheduler.h"

#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace aether::quest {

// Builds the quests stage. The host forwards events from other kits before
// the quest queue is drained, then handles quest events without a dependency
// on inventory, scripting, or Blueprint modules.
SystemDesc MakeQuestsSystem(QuestSystem* system, std::function<void(QuestSystem&)> before_drain,
                            std::function<void(const QuestEvent&)> on_event);

struct QuestAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Loads quest definitions and reports both per-file errors and cross-quest validation problems.
std::vector<QuestAssetDiagnostic> LoadQuestAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    const gas::EffectLibrary& effects, QuestLibrary& quests);

// This kit's description for the player's kit registry (Phase 37 step 4): its name, what it needs, the
// components it registers and the scheduler stage it adds.
std::unique_ptr<kit::IKit> MakeQuestsKit();

} // namespace aether::quest
