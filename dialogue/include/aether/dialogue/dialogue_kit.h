#pragma once

#include "aether/dialogue/dialogue_system.h"
#include "aether/kit/kit.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether::dlg {

struct DialogueAssetDiagnostic {
    std::string path;
    std::string message;
    bool content_read_failure = false;
};

// Loads and validates conversation definitions. The host supplies asset paths and content access; diagnostics keep package read failures
// apart from invalid conversations.
std::vector<DialogueAssetDiagnostic> LoadDialogueAssets(
    const std::vector<std::string>& paths,
    std::function<bool(const std::string&, std::vector<u8>&, std::string*)> read_content,
    DialogueLibrary& dialogues);

// This kit's description for the player's kit registry: its name (no dependencies), the conversations it loads and the system it creates.
std::unique_ptr<kit::IKit> MakeDialogueKit(DialogueLibrary* dialogues = nullptr);

} // namespace aether::dlg
