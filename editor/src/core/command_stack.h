#pragma once

#include "aether/core/base.h"

#include <memory>
#include <string>
#include <vector>

namespace aether {
class World;
class GuidIndex;
} // namespace aether

namespace aether::editor {

// What commands act on. Grows as the editor does (selection, assets, ...);
// kept to the essentials for now. See docs/design/PHASE_SPECS.md §7.2.
struct CommandContext {
    World& world;
    GuidIndex& guids;
};

// One undoable editor action. Do() is called once when the command is first
// executed and again on every redo; Undo() reverses it. Commands must refer to
// entities by EntityGuid, never by Entity handle: undoing a delete recreates
// the entity with a new handle but the same GUID.
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Do(CommandContext& ctx) = 0;
    virtual void Undo(CommandContext& ctx) = 0;
    virtual std::string Label() const = 0;

    // Offered the next command when it's executed with merging allowed (e.g.
    // successive values during one slider drag). Return true after absorbing
    // `next`'s effect, so this entry now undoes both at once; `next` has
    // already been applied and is then discarded.
    virtual bool TryMerge(const ICommand& next) {
        (void)next;
        return false;
    }

    // Approximate memory this entry holds, for the stack's memory budget.
    virtual usize MemoryBytes() const { return sizeof(*this); }
};

enum class MergePolicy { Never, Allow };

// The editor's undo/redo history.
//
// - Execute applies a command and records it, clearing the redo history.
// - With MergePolicy::Allow, a command may merge into the previous entry
//   (ICommand::TryMerge) — until BreakMergeChain() is called, which the UI
//   does when an edit is committed (mouse released, Enter pressed). Undo,
//   redo, saving and transactions also break the chain, so a merge never
//   reaches back past any of them.
// - BeginTransaction/EndTransaction group everything executed between them
//   into one entry (nested pairs are allowed; the outermost label is used).
// - The stack tracks the "saved" point for the unsaved-changes marker, and
//   drops the oldest entries once they exceed the memory budget.
class CommandStack {
public:
    static constexpr usize kDefaultMemoryBudget = 256ull * 1024 * 1024;

    void Execute(CommandContext& ctx, std::unique_ptr<ICommand> command, MergePolicy merge = MergePolicy::Never);

    bool CanUndo() const { return !undo_.empty() && transaction_depth_ == 0; }
    bool CanRedo() const { return !redo_.empty() && transaction_depth_ == 0; }
    // Return false (doing nothing) when there's nothing to undo/redo, or while
    // a transaction is open.
    bool Undo(CommandContext& ctx);
    bool Redo(CommandContext& ctx);

    std::string UndoLabel() const; // "" if nothing to undo
    std::string RedoLabel() const;

    void BeginTransaction(const std::string& label);
    void EndTransaction();
    bool InTransaction() const { return transaction_depth_ > 0; }

    void BreakMergeChain() { merge_open_ = false; }

    void MarkSaved();
    bool IsDirty() const;

    void SetMemoryBudget(usize bytes);
    usize MemoryUsed() const { return memory_used_; }

    usize UndoCount() const { return undo_.size(); }
    usize RedoCount() const { return redo_.size(); }
    void Clear();

private:
    void PushUndo(std::unique_ptr<ICommand> command);
    void EnforceBudget();

    std::vector<std::unique_ptr<ICommand>> undo_; // oldest first
    std::vector<std::unique_ptr<ICommand>> redo_; // next redo last
    std::vector<std::unique_ptr<ICommand>> transaction_commands_;
    std::string transaction_label_;
    int transaction_depth_ = 0;
    bool merge_open_ = false;

    // Undo depth at the last MarkSaved, and whether that state is still
    // reachable by undo/redo (it isn't once a new command replaces the redo
    // history past it, or the budget drops it).
    usize saved_depth_ = 0;
    bool saved_reachable_ = true;
    usize dropped_ = 0; // entries removed from the bottom by the budget

    usize memory_budget_ = kDefaultMemoryBudget;
    usize memory_used_ = 0;
};

} // namespace aether::editor
