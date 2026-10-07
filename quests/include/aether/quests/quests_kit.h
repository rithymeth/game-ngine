#pragma once

#include "aether/kit/kit.h"
#include "aether/quests/quest_system.h"
#include "aether/scene/scheduler.h"

#include <memory>

namespace aether::quest {

// Builds the quests stage. The host forwards events from other kits before
// the quest queue is drained, then handles quest events without a dependency
// on inventory, scripting, or Blueprint modules.
SystemDesc MakeQuestsSystem(QuestSystem* system, std::function<void(QuestSystem&)> before_drain,
                            std::function<void(const QuestEvent&)> on_event);

// This kit's description for the player's kit registry (Phase 37 step 4): its name, what it needs, the
// components it registers and the scheduler stage it adds.
std::unique_ptr<kit::IKit> MakeQuestsKit();

} // namespace aether::quest
