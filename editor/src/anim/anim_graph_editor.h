#pragma once

#include "anim/anim_document.h"
#include "graph/graph_view.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// The animation graph editor (Phase 16 step 6, ROADMAP.md Phase 16): the
// pose graph and each state machine in tabs, drawn with the Phase 12 graph
// widget; the Variables panel; Details for the selected node, state,
// transition or variable; and Diagnostics. It fills the current ImGui window
// and runs headless in tests.

// Node ids the views use for things that aren't pose nodes.
constexpr u32 kOutputNode = 0xFFFFFF00u; // the pose graph's Output Pose
constexpr u32 kEntryNode = 0xFFFFFF01u;  // a machine's Entry
constexpr u32 kAnyStateNode = 0xFFFFFF02u;
constexpr u32 kStateNodeBase = 1;        // state i is node kStateNodeBase + i

// A pose node's input pins, by kind ("A", "B" for Blend; "Base", "Layer" for Layered; ...).
std::vector<std::string> PoseInputPins(const anim::AnimNode& node);
std::string PoseNodeTitle(const anim::AnimNode& node);

GraphViewModel BuildPoseGraphView(const anim::AnimGraph& graph, const std::vector<anim::AnimDiagnostic>& diagnostics);
// `active` is the state playing now (PIE), highlighted along with the transition blending into it.
GraphViewModel BuildStateMachineView(const anim::StateMachine& machine, const std::string& active = {}, bool blending = false);

// Apply what the widget reported through the document; the reasons for refused edits come back.
std::vector<std::string> ApplyPoseGraphEdits(AnimGraphDocument& doc, const GraphViewResult& edits);
std::vector<std::string> ApplyStateMachineEdits(AnimGraphDocument& doc, const std::string& machine, const GraphViewResult& edits);

class AnimGraphEditor {
public:
    explicit AnimGraphEditor(AnimGraphDocument& document);

    void Draw();

    // Tabs: "" is the pose graph; otherwise a state machine's name.
    void OpenTab(const std::string& machine);
    const std::string& ActiveTab() const { return active_; }
    const std::vector<std::string>& Tabs() const { return tabs_; }
    GraphViewState& ViewState(const std::string& tab) { return views_[tab]; }

    enum class ItemKind { None, Variable, Node, State, Transition };
    struct Item {
        ItemKind kind = ItemKind::None;
        std::string name; // the variable, or the machine of a state or transition
        u32 node = 0;
        i32 index = -1; // state or transition
        bool operator==(const Item& o) const { return kind == o.kind && name == o.name && node == o.node && index == o.index; }
    };
    const Item& Selected() const { return selected_; }
    void Select(const Item& item);

    // PIE: the running instance, so machines highlight their current state.
    void SetLiveInstance(const anim::AnimGraphInstance* instance) { live_ = instance; }

    // The add-node menu (pose nodes in the pose graph; states in a machine).
    void OpenPalette(f32 x, f32 y);
    bool PaletteOpen() const { return palette_.open; }
    u32 PlaceNode(anim::AnimNodeKind kind); // in the pose graph, at the palette's point
    i32 PlaceState(bool conduit);           // in the active machine

    bool SaveNow();
    const std::string& Status() const { return status_; }

private:
    struct Palette {
        bool open = false, needs_popup = false;
        std::string tab;
        f32 x = 0, y = 0;
    };
    void DrawToolbar();
    void DrawVariables();
    void DrawTabs();
    void DrawGraph(const std::string& tab);
    void DrawDetails();
    void DrawNodeDetails(u32 id);
    void DrawStateDetails(const std::string& machine, i32 index);
    void DrawTransitionDetails(const std::string& machine, i32 index);
    void DrawVariableDetails(const std::string& name);
    void DrawDiagnostics();
    void DrawPalette();
    void HandleKeys();
    void ForgetMissing();
    std::string ActiveStateOf(const std::string& machine, bool& blending) const;

    AnimGraphDocument& doc_;
    Item selected_;
    std::string name_edit_;
    std::vector<std::string> tabs_{""};
    std::string active_;
    std::string select_tab_;
    std::map<std::string, GraphViewState> views_;
    Palette palette_;
    const anim::AnimGraphInstance* live_ = nullptr;
    std::string status_;
};

} // namespace aether::editor
