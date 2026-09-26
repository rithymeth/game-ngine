#include "core/command_stack.h"

#include <utility>

namespace aether::editor {

namespace {

// A transaction's commands, undone as one entry.
class CompoundCommand final : public ICommand {
public:
    CompoundCommand(std::string label, std::vector<std::unique_ptr<ICommand>> commands)
        : label_(std::move(label)), commands_(std::move(commands)) {}

    void Do(CommandContext& ctx) override {
        for (auto& command : commands_) {
            command->Do(ctx);
        }
    }
    void Undo(CommandContext& ctx) override {
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
            (*it)->Undo(ctx);
        }
    }
    std::string Label() const override { return label_; }
    usize MemoryBytes() const override {
        usize total = sizeof(*this);
        for (const auto& command : commands_) {
            total += command->MemoryBytes();
        }
        return total;
    }

private:
    std::string label_;
    std::vector<std::unique_ptr<ICommand>> commands_;
};

} // namespace

void CommandStack::Execute(CommandContext& ctx, std::unique_ptr<ICommand> command, MergePolicy merge) {
    command->Do(ctx);
    Record(std::move(command), merge);
}

void CommandStack::SetFrozen(bool frozen) {
    frozen_ = frozen;
    merge_open_ = false;
}

void CommandStack::Record(std::unique_ptr<ICommand> command, MergePolicy merge) {
    if (frozen_) {
        return; // applied, but not part of the history (see SetFrozen)
    }
    if (transaction_depth_ > 0) {
        transaction_commands_.push_back(std::move(command));
        return;
    }

    // Any new action makes the redo history unreachable — including the
    // saved state, if it was in there.
    if (!redo_.empty()) {
        if (saved_depth_ > undo_.size() + dropped_) {
            saved_reachable_ = false;
        }
        redo_.clear();
    }

    if (merge == MergePolicy::Allow && merge_open_ && !undo_.empty()) {
        usize before = undo_.back()->MemoryBytes();
        if (undo_.back()->TryMerge(*command)) {
            memory_used_ = memory_used_ - before + undo_.back()->MemoryBytes();
            return;
        }
    }

    PushUndo(std::move(command));
    merge_open_ = merge == MergePolicy::Allow;
}

bool CommandStack::Undo(CommandContext& ctx) {
    if (!CanUndo()) {
        return false;
    }
    std::unique_ptr<ICommand> command = std::move(undo_.back());
    undo_.pop_back();
    memory_used_ -= command->MemoryBytes();
    command->Undo(ctx);
    redo_.push_back(std::move(command));
    merge_open_ = false;
    return true;
}

bool CommandStack::Redo(CommandContext& ctx) {
    if (!CanRedo()) {
        return false;
    }
    std::unique_ptr<ICommand> command = std::move(redo_.back());
    redo_.pop_back();
    command->Do(ctx);
    PushUndo(std::move(command));
    merge_open_ = false;
    return true;
}

std::string CommandStack::UndoLabel() const {
    return undo_.empty() ? std::string() : undo_.back()->Label();
}

std::string CommandStack::RedoLabel() const {
    return redo_.empty() ? std::string() : redo_.back()->Label();
}

void CommandStack::BeginTransaction(const std::string& label) {
    if (transaction_depth_++ == 0) {
        transaction_label_ = label;
        transaction_commands_.clear();
    }
}

void CommandStack::EndTransaction() {
    if (transaction_depth_ == 0) {
        return;
    }
    if (--transaction_depth_ > 0) {
        return;
    }
    merge_open_ = false;
    if (transaction_commands_.empty()) {
        return;
    }
    if (!redo_.empty()) {
        if (saved_depth_ > undo_.size() + dropped_) {
            saved_reachable_ = false;
        }
        redo_.clear();
    }
    PushUndo(std::make_unique<CompoundCommand>(std::move(transaction_label_), std::move(transaction_commands_)));
    transaction_commands_.clear();
}

void CommandStack::MarkSaved() {
    saved_depth_ = undo_.size() + dropped_;
    saved_reachable_ = true;
    merge_open_ = false; // a merge would silently change the saved entry
}

bool CommandStack::IsDirty() const {
    return !saved_reachable_ || saved_depth_ != undo_.size() + dropped_;
}

void CommandStack::SetMemoryBudget(usize bytes) {
    memory_budget_ = bytes;
    EnforceBudget();
}

void CommandStack::Clear() {
    undo_.clear();
    redo_.clear();
    transaction_commands_.clear();
    transaction_depth_ = 0;
    merge_open_ = false;
    memory_used_ = 0;
    dropped_ = 0;
    saved_depth_ = 0;
    saved_reachable_ = true;
}

void CommandStack::PushUndo(std::unique_ptr<ICommand> command) {
    memory_used_ += command->MemoryBytes();
    undo_.push_back(std::move(command));
    EnforceBudget();
}

void CommandStack::EnforceBudget() {
    // Always keep the most recent entry, even if it alone exceeds the budget.
    while (undo_.size() > 1 && memory_used_ > memory_budget_) {
        memory_used_ -= undo_.front()->MemoryBytes();
        undo_.erase(undo_.begin());
        ++dropped_;
        if (saved_depth_ < dropped_) {
            saved_reachable_ = false; // the saved state can no longer be undone back to
        }
    }
}

} // namespace aether::editor
