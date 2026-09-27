#pragma once

#include "aether/renderer/material_codegen.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

// An open material or material function in the material editor (Phase 15
// step 5): the graph, its file, undo/redo and the generated shader, which
// is regenerated when the graph changes (it takes well under a
// millisecond). No ImGui here; the panels (material_editor.h) call it.
//
// Undo keeps whole-material snapshots (the .amat JSON), as the Blueprint
// editor does.
class MaterialDocument {
public:
    MaterialDocument() = default;
    explicit MaterialDocument(mat::Material material, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr); // to Path(); false if it has none
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);

    const mat::Material& Get() const { return material_; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const; // the file's stem, or "Untitled"
    bool Dirty() const { return dirty_; }
    // The functions Function.Call nodes resolve against; kept across undo.
    void SetFunctions(std::shared_ptr<const mat::FunctionLibrary> functions);

    // Every change goes through Edit: a snapshot for undo, then `change`.
    // Consecutive edits with the same non-empty `merge_key` (a value being
    // dragged) share one undo step.
    void Edit(const std::string& label, const std::function<void(mat::Material&)>& change, const std::string& merge_key = {});
    bool Undo();
    void CancelEdit(); // reverts the last Edit without a redo step
    bool Redo();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    std::string UndoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string RedoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    u64 Revision() const { return revision_; }

    // The analysis and shader for the current graph (cached per revision).
    const mat::GeneratedMaterial& Generated() const;
    const mat::Analysis& Analysis() const { return Generated().analysis; }

    // --- Parameters --------------------------------------------------------------
    // Names are non-empty, at most 64 characters, printable, and unique.
    // AddParameter picks a free name ("Param", "Param_1", ...) and returns it.
    std::string AddParameter(mat::PinType type);
    // Renames it and its parameter nodes.
    bool RenameParameter(const std::string& from, const std::string& to, std::string* error = nullptr);
    // Removes it and its nodes (with their links).
    bool RemoveParameter(const std::string& name);
    // Changes the type (resetting the default) and its nodes' kinds; links
    // that no longer fit are broken.
    bool SetParameterType(const std::string& name, mat::PinType type);
    // The node type for a parameter: Param.Scalar:, Param.Vector: or Param.Texture: + name.
    static std::string ParameterNodeType(const mat::Parameter& parameter);
    static bool IsValidName(const std::string& name);

    // --- Nodes ---------------------------------------------------------------------
    // The clipboard form: {"aether.material_nodes": 1, "nodes": [...],
    // "links": [...]}, links only between copied nodes, positions relative
    // to the first node. The Material Output isn't copied.
    std::string CopyNodes(const std::vector<mat::NodeId>& nodes) const;
    std::vector<mat::NodeId> PasteNodes(const std::string& text, float x, float y);
    std::vector<mat::NodeId> DuplicateNodes(const std::vector<mat::NodeId>& nodes);

private:
    struct Snapshot {
        std::string label;
        nlohmann::json state;
        std::string merge_key;
    };
    void Restore(const nlohmann::json& state);
    void Changed();

    mat::Material material_;
    std::filesystem::path path_;
    bool dirty_ = false;
    std::vector<Snapshot> undo_, redo_;
    u64 revision_ = 0;
    mutable u64 generated_revision_ = ~0ull;
    mutable mat::GeneratedMaterial generated_;
};

} // namespace aether::editor
