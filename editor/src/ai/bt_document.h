#pragma once

#include "aether/ai/behavior_tree.h"
#include "anim/json_history.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// A node's place in the tree: child indices from the root (empty is the root).
using BtPath = std::vector<usize>;
// As the checks write it: "root/Sequence[0]/Wait[1]".
std::string BtProblemPath(const ai::BehaviorTreeAsset& tree, const BtPath& path);
std::vector<ai::BtNodeType> BtCompositeTypes();
std::vector<ai::BtNodeType> BtTaskTypes();

// An open Behavior Tree (.abt) in the Behavior Tree editor (Phase 20 step
// 6): the asset, its file, whole-document undo, cached checks, and edits
// that keep it whole (children only under composites, no node under
// itself, key renames that every reference follows). Fields are edited
// through the saved form, so a value that doesn't load is refused with
// the loader's reason.
class BehaviorTreeDocument {
public:
    BehaviorTreeDocument(); // a Selector with a Wait under it
    explicit BehaviorTreeDocument(ai::BehaviorTreeAsset tree, std::filesystem::path path = {});

    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);
    bool SaveAs(const std::filesystem::path& path, std::string* error = nullptr);
    std::string Text() const { return ai::SaveBehaviorTree(tree_).dump(2); }

    const ai::BehaviorTreeAsset& Get() const { return tree_; }
    const ai::BtNode* Node(const BtPath& path) const;
    const std::filesystem::path& Path() const { return path_; }
    std::string Name() const;
    bool Dirty() const { return dirty_; }
    u64 Revision() const { return revision_; }

    void Edit(const std::string& label, const std::function<void(ai::BehaviorTreeAsset&)>& change, const std::string& merge_key = {});
    bool Undo();
    bool Redo();
    bool CanUndo() const { return history_.CanUndo(); }
    bool CanRedo() const { return history_.CanRedo(); }
    std::string UndoLabel() const { return history_.UndoLabel(); }

    // --- Nodes ----------------------------------------------------------------------------
    // Under a composite, at `index` (the end without one). The new node's path.
    std::optional<BtPath> AddNode(const BtPath& parent, ai::BtNodeType type, std::optional<usize> index = {}, std::string* error = nullptr);
    bool RemoveNode(const BtPath& path, std::string* error = nullptr); // not the root
    // To `index` under `parent` (counted without the node). Not the root, nor under itself.
    std::optional<BtPath> MoveNode(const BtPath& path, const BtPath& parent, usize index, std::string* error = nullptr);
    std::optional<BtPath> DuplicateNode(const BtPath& path, std::string* error = nullptr); // just after it
    // Composites and tasks swap freely, but a node with children stays a composite.
    bool SetNodeType(const BtPath& path, ai::BtNodeType type, std::string* error = nullptr);
    // The node as saved, without its children.
    nlohmann::json NodeJson(const BtPath& path) const;
    bool SetNodeField(const BtPath& path, const std::string& key, const nlohmann::json& value, std::string* error = nullptr,
                      const std::string& merge_key = {});

    // --- Decorators and services ----------------------------------------------------------
    bool AddDecorator(const BtPath& path, ai::BtDecoratorType type);
    bool RemoveDecorator(const BtPath& path, usize index);
    bool MoveDecorator(const BtPath& path, usize from, usize to);
    nlohmann::json DecoratorJson(const BtPath& path, usize index) const;
    bool SetDecoratorField(const BtPath& path, usize index, const std::string& key, const nlohmann::json& value, std::string* error = nullptr,
                           const std::string& merge_key = {});
    bool AddService(const BtPath& path, ai::BtServiceType type);
    bool RemoveService(const BtPath& path, usize index);
    nlohmann::json ServiceJson(const BtPath& path, usize index) const;
    bool SetServiceField(const BtPath& path, usize index, const std::string& key, const nlohmann::json& value, std::string* error = nullptr,
                         const std::string& merge_key = {});

    // --- The blackboard -------------------------------------------------------------------
    bool AddKey(const std::string& name, ai::BlackboardType type, std::string* error = nullptr);
    bool RemoveKey(const std::string& name); // what used it shows up in the checks (BT003)
    bool RenameKey(const std::string& from, const std::string& to, std::string* error = nullptr);
    bool SetKeyType(const std::string& name, ai::BlackboardType type); // values of the old type are cleared
    bool SetKeyInitial(const std::string& name, const nlohmann::json& value, std::string* error = nullptr);
    bool SetKeyDescription(const std::string& name, const std::string& text);
    std::string UniqueKeyName(const std::string& base) const;
    std::vector<BtPath> KeyUsers(const std::string& name) const;

    const std::vector<ai::BtProblem>& Diagnostics() const; // ValidateBehaviorTree, per revision
    usize ErrorCount() const;

private:
    nlohmann::json Snapshot() const { return ai::SaveBehaviorTree(tree_); }
    void Restore(const nlohmann::json& snapshot);
    ai::BtNode* Mutable(ai::BehaviorTreeAsset& t, const BtPath& path) const;
    // Edits the saved form of a node (or its decorator or service) and reloads the whole tree.
    bool EditJson(const std::string& label, const BtPath& path, const std::function<bool(nlohmann::json& node, std::string& why)>& change,
                  std::string* error, const std::string& merge_key);

    ai::BehaviorTreeAsset tree_;
    std::filesystem::path path_;
    JsonHistory history_;
    bool dirty_ = false;
    u64 revision_ = 1;
    mutable u64 diagnostics_revision_ = 0;
    mutable std::vector<ai::BtProblem> diagnostics_;
};

} // namespace aether::editor
