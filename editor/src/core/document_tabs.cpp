#include "core/document_tabs.h"

namespace aether::editor {

std::size_t DocumentTabs::Open(EditorDocument& document, bool* opened) {
    if (const std::optional<std::size_t> found = IndexOf(document)) {
        active_ = *found;
        if (opened != nullptr) *opened = false;
        return *found;
    }
    documents_.push_back(&document);
    active_ = documents_.size() - 1;
    if (opened != nullptr) *opened = true;
    return active_;
}

void DocumentTabs::Activate(std::size_t index) {
    if (index < documents_.size()) active_ = index;
}

bool DocumentTabs::Close(std::size_t index, bool discard) {
    if (index >= documents_.size() || documents_.size() == 1) return false;
    if (!discard && documents_[index]->IsDirty()) return false;
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
    if (active_ > index) {
        --active_;
    } else if (active_ == index) {
        active_ = index == documents_.size() ? index - 1 : index;
    }
    return true;
}

bool DocumentTabs::CloseOthers(std::size_t index, bool discard) {
    if (index >= documents_.size()) return false;
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (i != index && !discard && documents_[i]->IsDirty()) return false;
    }
    documents_ = {documents_[index]};
    active_ = 0;
    return true;
}

std::optional<std::size_t> DocumentTabs::IndexOf(const EditorDocument& document) const {
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (documents_[i] == &document) return i;
    }
    return std::nullopt;
}

} // namespace aether::editor
