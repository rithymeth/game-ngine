#pragma once

#include "aether/blueprint/validate.h"
#include "graph/graph_view.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

// The Blueprint editor's side of the graph widget (Phase 12 step 6): builds
// the view model from a Blueprint graph (pin colors and shapes, header
// colors by category, compile errors, the debugger's state), applies the
// widget's edits to the graph, and searches the node palette.

// Colors from docs/design/BLUEPRINT_NODES.md (ImGui ABGR order).
u32 PinColor(const bp::PinType& type);
u32 HeaderColor(const bp::NodeSignature& signature);

struct BlueprintViewOptions {
    const bp::ValidationResult* diagnostics = nullptr; // errors shown on their nodes
    bp::NodeId highlighted = 0;                        // the debugger's current node
    std::set<bp::NodeId> breakpoints;
    std::set<bp::NodeId> fired; // from the exec trace: wires out of these glow
};

GraphViewModel BuildBlueprintView(const bp::Blueprint& blueprint, const bp::Graph& graph,
                                  const BlueprintViewOptions& options = {});

// --- Edits --------------------------------------------------------------------
bp::NodeId AddNode(bp::Graph& graph, const std::string& type, float x, float y,
                   nlohmann::json config = nlohmann::json::object());
// Links two pins after checking they fit (direction, kind, type). A data
// input or an exec output can have one link, so linking to one replaces
// its old link, as in Unreal. False with the reason otherwise.
bool ConnectPins(const bp::Blueprint& blueprint, bp::Graph& graph, const GraphPinRef& from, const GraphPinRef& to,
                 std::string* error = nullptr);
bool Disconnect(bp::Graph& graph, const bp::Link& link);
void DeleteNodes(bp::Graph& graph, const std::vector<bp::NodeId>& nodes); // with their links
// Applies what the widget reported (moves, links, breaks, deletes). Returns
// the reasons for refused links (for the status bar).
std::vector<std::string> ApplyGraphEdits(const bp::Blueprint& blueprint, bp::Graph& graph, const GraphViewResult& edits);
// After a node is placed from a palette opened by dragging a wire: links
// its first pin that fits `from`. False if none does.
bool ConnectToNewNode(const bp::Blueprint& blueprint, bp::Graph& graph, const GraphPinRef& from, bp::NodeId node);

// --- Palette ------------------------------------------------------------------
// Fuzzy search (the query's letters in order, ignoring case and spaces) over
// the node types this Blueprint can use, best matches first. With `from`,
// only nodes that have a pin fitting that pin are listed (dragging a wire off
// a float output offers nodes with a float-compatible input).
std::vector<bp::PaletteEntry> SearchPalette(const bp::Blueprint& blueprint, const bp::Graph& graph,
                                            std::string_view query, const GraphPinRef* from = nullptr,
                                            usize max_results = 50);

} // namespace aether::editor
