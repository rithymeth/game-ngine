#pragma once

#include "ai/bt_document.h"
#include "aether/ai/bt_runtime.h"
#include "graph/graph_view.h"

#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// The Behavior Tree editor (Phase 20 step 6, ROADMAP.md Phase 20): the tree
// drawn top-down with the Phase 12 graph widget (laid out automatically:
// children in order, left to right), the Blackboard panel, Details for the
// selected node (its settings, decorators and services), Diagnostics, and
// the debugger (with a running instance: the active branch highlighted,
// finished nodes coloured by result, and the blackboard's values). It fills
// the current ImGui window and runs headless in tests.

struct BtLayoutNode {
    BtPath path;
    const ai::BtNode* node = nullptr;
    u32 id = 0; // the node's depth-first index + 1 (as BehaviorTreeInstance numbers them)
    f32 x = 0.0f, y = 0.0f;
    i32 parent = -1; // index in the layout
};
constexpr f32 kBtSlotWidth = 220.0f;
constexpr f32 kBtRowHeight = 160.0f;
std::vector<BtLayoutNode> LayoutBehaviorTree(const ai::BehaviorTreeAsset& tree);

// A node's title ("Wait 2s", "Move To Goal") and its decorators and services as lines.
std::string BtNodeTitle(const ai::BtNode& node);
std::string BtNodeNotes(const ai::BtNode& node);

GraphViewModel BuildBehaviorTreeView(const ai::BehaviorTreeAsset& tree, const std::vector<BtLayoutNode>& layout,
                                     const std::vector<ai::BtProblem>& problems, const ai::BehaviorTreeInstance* live = nullptr);
// Applies what the widget reported: a wire from a composite to a node moves the node there; a
// drag sideways reorders it among its siblings; Delete removes; Ctrl+D duplicates. The reasons
// for refused edits come back.
std::vector<std::string> ApplyBehaviorTreeEdits(BehaviorTreeDocument& doc, const std::vector<BtLayoutNode>& layout, const GraphViewResult& edits);

class BehaviorTreeEditor {
public:
    explicit BehaviorTreeEditor(BehaviorTreeDocument& document);

    void Draw();

    const std::optional<BtPath>& Selected() const { return selected_; }
    void Select(const BtPath& path);
    void ClearSelection();
    GraphViewState& View() { return view_; }
    const std::vector<BtLayoutNode>& Layout();

    // The add-node menu: nodes go under `parent` (a composite).
    void OpenPalette(const BtPath& parent);
    bool PaletteOpen() const { return palette_open_; }
    std::optional<BtPath> PlaceNode(ai::BtNodeType type);

    // The debugger: a running instance of this tree and its blackboard (null to stop).
    void SetLiveInstance(const ai::BehaviorTreeInstance* instance, const ai::Blackboard* blackboard = nullptr);

    bool SaveNow();
    const std::string& Status() const { return status_; }

private:
    void DrawToolbar();
    void DrawBlackboard();
    void DrawTree();
    void DrawPalette();
    void DrawDetails();
    void DrawDecorators(const BtPath& path);
    void DrawServices(const BtPath& path);
    void DrawDiagnostics();
    void DrawDebugger();
    void HandleKeys();
    const std::vector<const char*>& KeyNames();

    BehaviorTreeDocument& doc_;
    GraphViewState view_;
    std::vector<BtLayoutNode> layout_;
    u64 layout_revision_ = 0;
    std::optional<BtPath> selected_;
    bool palette_open_ = false, palette_needs_popup_ = false;
    BtPath palette_parent_;
    const ai::BehaviorTreeInstance* live_ = nullptr;
    const ai::Blackboard* live_board_ = nullptr;
    std::string new_key_ = "Target";
    int new_key_type_ = 5;
    std::string name_edit_;
    std::vector<std::string> key_names_;
    std::vector<const char*> key_ptrs_;
    std::string status_;
};

} // namespace aether::editor
