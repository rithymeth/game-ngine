#include "workspace/command_palette.h"

#include "workspace/editor_workspace.h"

#include <algorithm>
#include <cctype>

namespace aether::editor {

namespace {

bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    const auto equal = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), equal) != haystack.end();
}

} // namespace

CommandPalette::CommandPalette(const EditorWorkspace& workspace) : workspace_(&workspace) { SetQuery(std::string_view{}); }

void CommandPalette::SetQuery(std::string_view query) {
    query_.assign(query);
    results_.clear();
    if (workspace_ == nullptr) return;
    for (std::size_t tool = 0; tool < workspace_->ToolCount(); ++tool) {
        // Match the name or the category: "anim" should find the animation tools whether or not
        // their names contain it.
        if (ContainsNoCase(workspace_->ToolName(tool), query_) || ContainsNoCase(workspace_->ToolCategory(tool), query_)) {
            results_.push_back(tool);
        }
    }
}

bool CommandPalette::Activate(EditorWorkspace& workspace, std::size_t index) const {
    if (index >= results_.size()) return false;
    workspace.Select(results_[index]);
    return true;
}

bool CommandPalette::ActivateFirst(EditorWorkspace& workspace) const { return Activate(workspace, 0); }

} // namespace aether::editor
