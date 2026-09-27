#pragma once

#include "anim/json_history.h"
#include "aether/animation/anim_graph.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace aether::editor {

// An open animation graph in the editor (Phase 16 step 6): the graph, its
// file, undo, the cached diagnostics, and the edits that have to keep
// references straight (renaming a variable updates the nodes and
// conditions that use it; removing a state fixes up transition indices).
class AnimGraphDocument {
public:
    AnimGraphDocument() = default;
    explicit AnimGraphDocument(anim::AnimGraph graph, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);

    const anim::AnimGraph& Get() const { return graph_; }
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return dirty_; }
    u64 Revision() const { return revision_; }
    // Clips and blend spaces, for AG010 (missing assets) and the palettes.
    void SetAssets(anim::AnimAssets assets, std::vector<std::string> clip_names = {}, std::vector<std::string> blend_space_names = {});
    const std::vector<std::string>& ClipNames() const { return clip_names_; }
    const std::vector<std::string>& BlendSpaceNames() const { return blend_space_names_; }

    void Edit(const std::string& label, const std::function<void(anim::AnimGraph&)>& change, const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    void CancelEdit();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }
    std::string UndoLabel() const { return history_.UndoLabel(); }

    const std::vector<anim::AnimDiagnostic>& Diagnostics() const; // cached per revision
    usize ErrorCount() const;

    // --- Variables -----------------------------------------------------------------
    static bool IsValidName(const std::string& name); // non-empty, <= 64, printable, no leading/trailing space
    std::string AddVariable(anim::VarType type);      // "NewVar", "NewVar_1", ...
    bool RenameVariable(const std::string& from, const std::string& to, std::string* error = nullptr);
    // Removes it; nodes using it lose the reference and conditions on it go.
    bool RemoveVariable(const std::string& name);
    // Changes the type; conditions whose comparison no longer fits go.
    bool SetVariableType(const std::string& name, anim::VarType type);

    // --- Pose nodes ------------------------------------------------------------------
    u32 AddNode(anim::AnimNodeKind kind, f32 x, f32 y);
    // Deletes nodes; inputs, states and the output that used them are unset.
    void DeleteNodes(const std::vector<u32>& ids);
    // Makes `from` input `index` of `to` (growing a Blend by Int's inputs).
    // Refused (with a reason) if it would loop.
    bool ConnectPose(u32 from, u32 to, usize index, std::string* error = nullptr);
    bool SetOutput(u32 node);

    // --- State machines --------------------------------------------------------------
    // A new machine and a State Machine node that runs it.
    std::string AddMachine(f32 x, f32 y, u32* node = nullptr);
    bool RenameMachine(const std::string& from, const std::string& to, std::string* error = nullptr);
    // Removes it and the nodes that run it.
    bool RemoveMachine(const std::string& name);
    // A new state with a Clip node for its pose (none for a conduit). Returns its index.
    i32 AddState(const std::string& machine, f32 x, f32 y, bool conduit = false);
    bool RenameState(const std::string& machine, const std::string& from, const std::string& to, std::string* error = nullptr);
    // Removes it and its transitions; the entry moves to the first state if it was the entry.
    bool RemoveState(const std::string& machine, const std::string& state);
    bool SetEntry(const std::string& machine, const std::string& state);
    // `from` -1 means any state. Returns the transition's index, or -1.
    i32 AddTransition(const std::string& machine, i32 from, u32 to);
    bool RemoveTransition(const std::string& machine, usize index);

private:
    void Restore(const nlohmann::json& state);
    anim::StateMachine* Machine(anim::AnimGraph& g, const std::string& name);

    anim::AnimGraph graph_;
    std::filesystem::path path_;
    bool dirty_ = false;
    JsonHistory history_;
    u64 revision_ = 0;
    anim::AnimAssets assets_;
    bool has_assets_ = false;
    std::vector<std::string> clip_names_, blend_space_names_;
    mutable u64 diagnosed_ = ~0ull;
    mutable std::vector<anim::AnimDiagnostic> diagnostics_;
};

} // namespace aether::editor
