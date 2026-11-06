#pragma once

// Phase 42.6: the data-layer panel -- the list of layers in the open scene, each one switchable.
//
// The checkbox is glue: SetLayerActive is the operation, and it records one undoable edit through
// SetDataLayerActiveCommand. Keeping it public and separate from Draw is what lets the behaviour be
// checked without clicking anything, the same way ContentBrowserPanel::Commit is.

#include "core/command_stack.h"

#include <string>
#include <vector>

namespace aether::editor {

class DataLayerPanel {
public:
    DataLayerPanel(World& world, GuidIndex& guids, CommandStack& stack);

    // What the panel lists, re-read each time: every layer in the scene, how many entities belong to
    // it, and whether it is currently on.
    struct Row {
        std::string layer;
        usize entities = 0;
        bool active = true;
    };
    std::vector<Row> Rows() const;

    // Switches `layer` on or off as one undoable edit. False when nothing changed: no such layer, or
    // it was already in that state.
    bool SetLayerActive(const std::string& layer, bool active);

    // The window. `open` is plain UI state, so a host or a headless test can show it without a key.
    bool open = false;
    void Draw();

private:
    World& world_;
    GuidIndex& guids_;
    CommandStack& stack_;
};

} // namespace aether::editor