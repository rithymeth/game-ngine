#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether::editor {

// Whole-document undo for the animation editors (Phase 16 step 6): JSON
// snapshots, with consecutive edits under one non-empty merge key (a value
// being dragged) sharing a step, as the Blueprint and material editors do.
class JsonHistory {
public:
    static constexpr size_t kMaxSteps = 200;

    // Call before a change, with the state it changes from.
    void Record(const std::string& label, nlohmann::json current, const std::string& merge_key = {}) {
        const bool merge = !merge_key.empty() && !undo_.empty() && undo_.back().merge_key == merge_key && redo_.empty();
        if (!merge) {
            undo_.push_back({label, std::move(current), merge_key});
            if (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
        }
        redo_.clear();
    }
    // The state to go back to; `current` becomes the redo step.
    bool Undo(nlohmann::json current, nlohmann::json& restore) { return Move(undo_, redo_, std::move(current), restore); }
    bool Redo(nlohmann::json current, nlohmann::json& restore) { return Move(redo_, undo_, std::move(current), restore); }
    // Drops the last step without a redo (an edit that changed nothing).
    bool Cancel(nlohmann::json& restore) {
        if (undo_.empty()) return false;
        restore = std::move(undo_.back().state);
        undo_.pop_back();
        return true;
    }
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    std::string UndoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string RedoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    void Clear() { undo_.clear(), redo_.clear(); }

private:
    struct Step {
        std::string label;
        nlohmann::json state;
        std::string merge_key;
    };
    static bool Move(std::vector<Step>& from, std::vector<Step>& to, nlohmann::json current, nlohmann::json& restore) {
        if (from.empty()) return false;
        Step step = std::move(from.back());
        from.pop_back();
        to.push_back({step.label, std::move(current), {}});
        restore = std::move(step.state);
        return true;
    }
    std::vector<Step> undo_, redo_;
};

} // namespace aether::editor
