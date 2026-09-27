#pragma once

#include "graph/blueprint_document.h"
#include "graph/blueprint_graph.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// The Blueprint editor (Phase 12 step 6 part 2, ROADMAP.md §12.6): a
// toolbar (Compile, Save, Undo/Redo, the parent class), the My Blueprint
// panel, graph tabs drawn with the graph widget, the Details panel, the
// Compiler Results panel and the node palette popup. It fills the current
// ImGui window and is portable, so it runs headless in tests.
//
// Keys while the editor has focus: Ctrl+Z/Y (undo/redo), Ctrl+S (save), F7
// (compile). The graph's own keys are in graph_view.h.
class BlueprintEditor {
public:
    explicit BlueprintEditor(BlueprintDocument& document);

    void Draw();

    // --- What the Details panel shows --------------------------------------
    enum class ItemKind { None, Variable, Graph, Dispatcher, Node, Comment };
    struct Item {
        ItemKind kind = ItemKind::None;
        std::string name;    // Variable, Graph, Dispatcher; the graph for Node and Comment
        bp::NodeId node = 0; // Node
        int comment = -1;    // Comment
        bool operator==(const Item& o) const {
            return kind == o.kind && name == o.name && node == o.node && comment == o.comment;
        }
    };
    const Item& Selected() const { return selected_; }
    void Select(const Item& item);

    // --- Graph tabs ------------------------------------------------------------
    void OpenGraph(const std::string& name); // opens (or switches to) its tab
    void CloseGraph(const std::string& name); // the Event Graph stays open
    const std::string& ActiveGraph() const { return active_; }
    const std::vector<std::string>& OpenGraphs() const { return tabs_; }
    GraphViewState& ViewState(const std::string& graph) { return views_[graph]; }
    // Opens the node's graph, selects it and frames it (Compiler Results'
    // rows and Find do this).
    void FocusNode(const std::string& graph, bp::NodeId node);

    // --- Palette -------------------------------------------------------------------
    // Opens the palette for the active graph at a canvas point, optionally
    // from a dragged pin (context-sensitive list, and the placed node links
    // to it).
    void OpenPalette(float x, float y, const GraphPinRef* from = nullptr);
    bool PaletteOpen() const { return palette_.open; }
    void SetPaletteQuery(const std::string& query) { palette_.query = query; }
    std::vector<bp::PaletteEntry> PaletteResults() const;
    // Places a node of this type at the palette's point and closes it.
    bp::NodeId PlaceFromPalette(const std::string& type);

    // --- Actions (toolbar and keys) -----------------------------------------------------
    void CompileNow();
    bool SaveNow();
    // Renames from the My Blueprint panel or Details, keeping tabs, view
    // states and the selection in step. The reason goes to the status line.
    bool RenameGraph(const std::string& from, const std::string& to);
    bool RenameVariable(const std::string& from, const std::string& to);
    bool RenameDispatcher(const std::string& from, const std::string& to);

    // The debugger's display state (current node, breakpoints, fired nodes
    // for glowing wires), set by the play session each frame.
    BlueprintViewOptions debug;

    const std::string& Status() const { return status_; }

private:
    struct Palette {
        bool open = false;
        bool needs_popup = false;
        std::string graph;
        float x = 0.0f, y = 0.0f;
        std::optional<GraphPinRef> from;
        std::string query;
    };

    void DrawToolbar();
    void DrawMyBlueprint();
    void DrawGraphTabs();
    void DrawGraph(const std::string& name);
    void DrawDetails();
    void DrawVariableDetails(const std::string& name);
    void DrawGraphDetails(const std::string& name);
    void DrawDispatcherDetails(const std::string& name);
    void DrawNodeDetails(const std::string& graph, bp::NodeId node);
    void DrawCommentDetails(const std::string& graph, int index);
    void DrawParams(const std::string& id, std::vector<bp::Variable>& params, bool allow_exec, const std::string& undo_key,
                    const std::function<std::vector<bp::Variable>&(bp::Blueprint&)>& locate);
    void DrawResults();
    void DrawPalette();
    void HandleKeys();
    void HandleGraphResult(const std::string& graph, const GraphViewResult& result, const GraphViewModel& model);
    void ForgetMissing(); // after undo/redo or removal: drop tabs and selection that no longer exist

    BlueprintDocument& doc_;
    Item selected_;
    std::string name_edit_; // the Details panel's name field, reset when the selection changes
    std::vector<std::string> tabs_;
    std::string active_;
    std::string select_tab_; // a tab to bring to front on the next draw
    std::map<std::string, GraphViewState> views_;
    Palette palette_;
    std::string clipboard_; // also copied to the system clipboard
    std::string status_;
};

} // namespace aether::editor
