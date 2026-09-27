#pragma once

#include "aether/blueprint/compiler.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

// An open Blueprint in the editor (Phase 12 step 6 part 2): the Blueprint,
// its file, undo/redo, the last compile, and the edits the My Blueprint
// panel and the graph make. No ImGui here; the panels (blueprint_editor.h)
// call it.
//
// Undo keeps whole-Blueprint snapshots (the .abp JSON), which is simple and
// small next to a scene: a large Blueprint is tens of KB.
class BlueprintDocument {
public:
    BlueprintDocument() = default;
    explicit BlueprintDocument(bp::Blueprint blueprint, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr); // to Path(); false if it has none
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);

    const bp::Blueprint& Get() const { return blueprint_; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const; // the file's stem, or "Untitled"
    bool Dirty() const { return dirty_; }

    // Every change goes through Edit: a snapshot for undo, then `change`.
    // Consecutive edits with the same non-empty `merge_key` (a text field
    // being typed in) share one undo step.
    void Edit(const std::string& label, const std::function<void(bp::Blueprint&)>& change,
              const std::string& merge_key = {});
    bool Undo();
    // Reverts the last Edit without a redo step (an edit that turned out
    // to change nothing).
    void CancelEdit();
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    std::string UndoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string RedoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }

    // --- Compiling ------------------------------------------------------------
    enum class Status { NotCompiled, UpToDate, Warnings, Errors, Stale };
    const bp::CompileResult& Compile();
    const bp::CompileResult& LastCompile() const { return compiled_; }
    Status CompileStatus() const;

    // --- My Blueprint -----------------------------------------------------------
    // Names are identifiers (letters, digits, '_', not starting with a digit)
    // and unique among their kind. The Add functions pick a free name
    // ("NewVar", "NewVar_1", ...) and return it.
    std::string AddVariable(const bp::PinType& type = bp::PinType::Of(bp::ValueType::Bool));
    // Renames and updates the Get/Set nodes in every graph.
    bool RenameVariable(const std::string& from, const std::string& to, std::string* error = nullptr);
    // Removes it and its Get/Set nodes (with their links), as Unreal does.
    bool RemoveVariable(const std::string& name);
    // Changes the type, resetting the default; links that no longer fit
    // are broken.
    bool SetVariableType(const std::string& name, const bp::PinType& type);

    std::string AddFunction(); // with its Entry and Return nodes, linked
    std::string AddMacro();    // with its Inputs and Outputs tunnels
    // Renames a function or macro and updates its call, macro-instance,
    // and Sort/Filter "by" references.
    bool RenameGraph(const std::string& from, const std::string& to, std::string* error = nullptr);
    // Removes a function or macro and the nodes that call it. The Event
    // Graph can't be removed.
    bool RemoveGraph(const std::string& name, std::string* error = nullptr);

    std::string AddDispatcher();
    bool RenameDispatcher(const std::string& from, const std::string& to, std::string* error = nullptr);
    bool RemoveDispatcher(const std::string& name);

    // --- Graph editing -----------------------------------------------------------
    // The clipboard form: {"aether.nodes": 1, "nodes": [...], "links": [...]},
    // links only between copied nodes, positions relative to the first
    // node's top-left.
    std::string CopyNodes(const std::string& graph, const std::vector<bp::NodeId>& nodes) const;
    // Pastes with fresh IDs at (x, y). Returns the new nodes (empty if the
    // text isn't a node clipboard).
    std::vector<bp::NodeId> PasteNodes(const std::string& graph, const std::string& text, float x, float y);
    std::vector<bp::NodeId> DuplicateNodes(const std::string& graph, const std::vector<bp::NodeId>& nodes);
    // A comment box around a region (the selected nodes' bounds, from
    // NodeBounds in graph_view.h), with a margin and room for its title.
    bool AddComment(const std::string& graph, float x0, float y0, float x1, float y1,
                    const std::string& text = "Comment");

    static bool IsValidName(const std::string& name);

private:
    struct Snapshot {
        std::string label;
        nlohmann::json state;
        std::string merge_key;
    };
    std::string FreeName(const std::string& base, const std::function<bool(const std::string&)>& taken) const;
    void Restore(const nlohmann::json& state);

    bp::Blueprint blueprint_;
    std::filesystem::path path_;
    bool dirty_ = false;
    std::vector<Snapshot> undo_, redo_;
    u64 revision_ = 0, compiled_revision_ = 0;
    bool compiled_once_ = false;
    bp::CompileResult compiled_;
};

} // namespace aether::editor
