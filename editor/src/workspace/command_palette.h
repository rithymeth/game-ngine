#pragma once

// Phase 43.5: the command palette's matching half.
//
// A query filters the workspace's tools by a case-insensitive substring of the tool name or
// its category, and activating a result selects that tool. Keeping the matching out of the
// drawing code is what makes it testable: the behaviour -- what a query matches and what
// choosing a result does -- is checked without a window.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

class EditorWorkspace;

class CommandPalette {
public:
    // Reads the workspace's tool list; it must outlive the palette.
    explicit CommandPalette(const EditorWorkspace& workspace);

    // An empty query lists every tool, in workspace order, so the palette doubles as an index.
    // Matching is a case-insensitive substring of the name or the category, and the results
    // stay in workspace order so the same query always lists the same thing.
    void SetQuery(std::string_view query);
    const std::string& Query() const { return query_; }
    const std::vector<std::size_t>& Results() const { return results_; }

    // Selects the tool at `index` in Results(); false when the index is out of range.
    bool Activate(EditorWorkspace& workspace, std::size_t index) const;
    // Selects the first result, for Enter with nothing highlighted; false when nothing matched.
    bool ActivateFirst(EditorWorkspace& workspace) const;

private:
    const EditorWorkspace* workspace_ = nullptr;
    std::string query_;
    std::vector<std::size_t> results_;
};

} // namespace aether::editor
