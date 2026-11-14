#pragma once

#include "core/editor_document.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace aether::editor {

// Stage 1 of docs/design/EDITOR_PARITY_AUDIT.md: the documents the editor has open, in tab order. A document is its
// own identity, so opening it again focuses the tab it already has rather than making a second copy. The last tab
// cannot be closed: the document still exists, and an empty strip would leave the toolbar and the quit prompt with no
// document to mean.
class DocumentTabs {
public:
    // Opens `document` and focuses it. `opened` says whether a tab was added (false: an existing one was focused).
    std::size_t Open(EditorDocument& document, bool* opened = nullptr);
    // Focuses the tab at `index`; out of range is ignored.
    void Activate(std::size_t index);
    // Closes the tab at `index`, leaving a neighbor active. A dirty document is left open unless `discard` is true,
    // and so is the last document. False means nothing changed.
    bool Close(std::size_t index, bool discard = false);
    // Keeps only the tab at `index`. Nothing changes when the index is out of range, or when another document is
    // dirty and `discard` is false.
    bool CloseOthers(std::size_t index, bool discard = false);

    std::size_t Count() const { return documents_.size(); }
    bool Empty() const { return documents_.empty(); }
    std::size_t ActiveIndex() const { return active_; }
    EditorDocument* Active() const { return documents_.empty() ? nullptr : documents_[active_]; }
    EditorDocument& At(std::size_t index) const { return *documents_[index]; }
    std::optional<std::size_t> IndexOf(const EditorDocument& document) const;

private:
    std::vector<EditorDocument*> documents_;
    std::size_t active_ = 0;
};

} // namespace aether::editor
