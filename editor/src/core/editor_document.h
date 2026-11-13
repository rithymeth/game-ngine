#pragma once

// Stage 0 of docs/design/EDITOR_PARITY_AUDIT.md: the contract every open document answers.
//
// The shell's quit prompt -- and, from Stage 1, its tabs and toolbar -- asks these questions of the
// active document instead of a particular panel's internals. SceneDocument adopts the contract in
// this stage; the other asset documents migrate in Stage 6. A document owns its edit state, not its
// window: panels keep their ImGui state and ask the document for everything below.

#include <filesystem>
#include <string>

namespace aether::editor {

class EditorDocument {
public:
    virtual ~EditorDocument() = default;

    // The file this document saves to; empty when it has never been saved.
    virtual const std::filesystem::path& FilePath() const = 0;
    // What a tab or a prompt calls it: the file name, or "Untitled" before the first save.
    virtual std::string DisplayName() const;
    // What kind of document it is, for prompts: "scene", "sequence", ...
    virtual const char* Kind() const = 0;
    // True while changes are not on disk. This is what a close or quit prompt asks: a clean
    // document closes without one.
    virtual bool IsDirty() const = 0;

    virtual bool CanUndo() const = 0;
    virtual bool CanRedo() const = 0;
    // What the next undo or redo would do, for a menu item or a tooltip ("Move Player").
    virtual std::string UndoLabel() const = 0;
    virtual std::string RedoLabel() const = 0;
    virtual bool Undo() = 0;
    virtual bool Redo() = 0;

    // Writes the document to `file`, or to FilePath() when `file` is empty -- which is save-as.
    // False, with the reason in `error`, leaves the document dirty. A panel may front this with
    // its own naming defaults and overwrite checks; the document is what actually changes.
    virtual bool Save(const std::filesystem::path& file = {}, std::string* error = nullptr) = 0;
};

} // namespace aether::editor
