#pragma once

#include "aether/renderer/material.h"
#include "graph/graph_view.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether::editor {

// The material editor's side of the graph widget (Phase 15 step 5): the
// view model from a material graph (pin colors by type, header colors by
// category, errors on their nodes), the widget's edits applied with type
// checks, and palette search.

u32 MaterialPinColor(mat::PinType type);
u32 MaterialHeaderColor(const std::string& category);

// `analysis` gives generic pins their inferred colors and the errors shown
// on nodes; pass Analyze(material) (or the document's cached one).
GraphViewModel BuildMaterialView(const mat::Material& material, const mat::Analysis& analysis);

// --- Edits --------------------------------------------------------------------
mat::NodeId AddMaterialNode(mat::Material& material, const std::string& type, float x, float y,
                            nlohmann::json config = nlohmann::json::object());
// Links an output to an input (either order) after checking the pins exist,
// the types fit (CanConnect, with generic pins taking anything but a
// texture) and the link wouldn't make a cycle. An input has one link, so
// linking to it replaces the old one. False with the reason otherwise.
bool ConnectMaterialPins(mat::Material& material, const GraphPinRef& a, const GraphPinRef& b, std::string* error = nullptr);
bool DisconnectMaterial(mat::Material& material, const mat::Link& link);
// Deletes nodes with their links. The Material Output can't be deleted;
// returns how many nodes went.
usize DeleteMaterialNodes(mat::Material& material, const std::vector<mat::NodeId>& nodes);
// Applies the widget's moves, links, breaks and deletes; the reasons for
// refused links come back (for the status line).
std::vector<std::string> ApplyMaterialEdits(mat::Material& material, const GraphViewResult& edits);
// After placing a node from a palette opened by dragging a wire: links the
// first pin of the new node that fits.
bool ConnectToNewMaterialNode(mat::Material& material, const GraphPinRef& from, mat::NodeId node);

// --- Palette ------------------------------------------------------------------
// Fuzzy search over the node types the material can use, best first. With
// `from`, only nodes with a pin that could link to it are listed.
std::vector<mat::PaletteEntry> SearchMaterialPalette(const mat::Material& material, std::string_view query,
                                                     const GraphPinRef* from = nullptr, usize max_results = 50);

} // namespace aether::editor
