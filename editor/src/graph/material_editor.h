#pragma once

#include "graph/material_document.h"
#include "graph/material_graph.h"

#include <optional>
#include <string>
#include <vector>

namespace aether::editor {

// The material editor (Phase 15 step 5, ROADMAP.md Phase 15): a toolbar
// (Save, Undo/Redo, the material's state), the Parameters panel, the graph
// (Phase 12's widget), the Details panel (the selected node, parameter or
// the material's settings) and a bottom panel with Stats, Diagnostics and
// the generated HLSL. It fills the current ImGui window and is portable, so
// it runs headless in tests. The live preview (a lit sphere) is the GPU's,
// in step 6.
//
// Keys while the editor has focus: Ctrl+Z/Y (undo/redo), Ctrl+S (save).
// The graph's own keys are in graph_view.h.
class MaterialEditor {
public:
    explicit MaterialEditor(MaterialDocument& document);

    void Draw();

    // --- What the Details panel shows -----------------------------------------
    enum class ItemKind { None, Parameter, Node };
    struct Item {
        ItemKind kind = ItemKind::None; // None: the material's settings
        std::string parameter;
        mat::NodeId node = 0;
        bool operator==(const Item& o) const { return kind == o.kind && parameter == o.parameter && node == o.node; }
    };
    const Item& Selected() const { return selected_; }
    void Select(const Item& item);

    GraphViewState& View() { return view_; }
    // Selects and frames a node (the Diagnostics panel's rows do this).
    void FocusNode(mat::NodeId node);

    // --- Palette ---------------------------------------------------------------
    void OpenPalette(float x, float y, const GraphPinRef* from = nullptr);
    bool PaletteOpen() const { return palette_.open; }
    void SetPaletteQuery(const std::string& query) { palette_.query = query; }
    std::vector<mat::PaletteEntry> PaletteResults() const;
    mat::NodeId PlaceFromPalette(const std::string& type);

    // --- Actions -------------------------------------------------------------------
    bool SaveNow();
    bool RenameParameter(const std::string& from, const std::string& to);
    // Places a node for the parameter (what dragging it into the graph does).
    mat::NodeId AddParameterNode(const std::string& name, float x, float y);

    enum class BottomTab { Stats, Diagnostics, Hlsl };
    BottomTab bottom_tab = BottomTab::Stats;

    const std::string& Status() const { return status_; }

private:
    struct Palette {
        bool open = false;
        bool needs_popup = false;
        float x = 0.0f, y = 0.0f;
        std::optional<GraphPinRef> from;
        std::string query;
    };

    void DrawToolbar();
    void DrawParameters();
    void DrawGraph();
    void DrawDetails();
    void DrawMaterialDetails();
    void DrawParameterDetails(const std::string& name);
    void DrawNodeDetails(mat::NodeId id);
    void DrawBottom();
    void DrawPalette();
    void HandleKeys();
    void HandleGraphResult(const GraphViewResult& result);
    void ForgetMissing();

    MaterialDocument& doc_;
    Item selected_;
    std::string name_edit_;
    GraphViewState view_;
    Palette palette_;
    std::string clipboard_;
    std::string status_;
};

} // namespace aether::editor
