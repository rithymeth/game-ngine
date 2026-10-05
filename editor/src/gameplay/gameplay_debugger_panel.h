#pragma once

#include "gameplay/gameplay_debugger_document.h"

#include <string>

namespace aether::editor {

// The Gameplay Debugger (Phase 30 step 6, §30.7): the attributes (base, current,
// bounds and the modifiers on them), active effects, abilities and tags of the
// entities in the world it is pointed at, with a filter, and buttons to set a
// base, remove an effect, cancel an ability and add or remove a tag. It rebuilds
// its rows every frame, and caps what it draws.
class GameplayDebuggerPanel {
public:
    explicit GameplayDebuggerPanel(GameplayDebuggerDocument& document) : doc_(document) {}

    const std::string& Message() const { return message_; }
    void Draw();

private:
    GameplayDebuggerDocument& doc_;
    std::string filter_;
    std::string tag_text_;
    std::string message_;
    bool selected_only_ = false;
};

} // namespace aether::editor
