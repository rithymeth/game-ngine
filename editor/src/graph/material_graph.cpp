#include "graph/material_graph.h"

#include <imgui.h>

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace aether::editor {

using mat::Material;
using mat::NodeId;
using mat::PinType;

u32 MaterialPinColor(PinType type) {
    switch (type) {
    case PinType::Float: return IM_COL32(0x9A, 0xDB, 0x47, 255);
    case PinType::Float2: return IM_COL32(0x4F, 0xC3, 0xE8, 255);
    case PinType::Float3: return IM_COL32(0xF5, 0xC5, 0x42, 255);
    case PinType::Float4: return IM_COL32(0xE0, 0x6C, 0xD8, 255);
    case PinType::Texture: return IM_COL32(0xE8, 0x5A, 0x3C, 255);
    default: return IM_COL32(160, 160, 160, 255); // generic, not yet inferred
    }
}

u32 MaterialHeaderColor(const std::string& category) {
    if (category == "Output") return IM_COL32(112, 86, 43, 255);
    if (category == "Parameters") return IM_COL32(44, 89, 63, 255);
    if (category == "Constants") return IM_COL32(51, 83, 69, 255);
    if (category == "Texture") return IM_COL32(112, 61, 51, 255);
    if (category == "Coordinates" || category == "Vectors") return IM_COL32(48, 79, 102, 255);
    if (category == "Functions") return IM_COL32(78, 62, 101, 255);
    if (category == "Custom") return IM_COL32(105, 58, 84, 255);
    if (category == "Utility") return IM_COL32(67, 70, 77, 255);
    return IM_COL32(49, 79, 132, 255); // math and shading
}

GraphViewModel BuildMaterialView(const Material& m, const mat::Analysis& analysis) {
    GraphViewModel model;
    std::set<std::pair<NodeId, std::string>> linked_in, linked_out;
    for (const mat::Link& l : m.links) {
        linked_out.insert({l.from.node, l.from.pin});
        linked_in.insert({l.to.node, l.to.pin});
    }
    // A generic node's inputs share the type its result was inferred as.
    auto pin_type = [&](NodeId node, const mat::NodeSignature& sig, const mat::PinDesc& p, bool output) {
        if (output) {
            const PinType t = analysis.TypeOf({node, p.name});
            return t != PinType::None ? t : p.type;
        }
        if (p.type != PinType::Any || !sig.generic || sig.outputs.empty()) return p.type;
        const PinType t = analysis.TypeOf({node, sig.outputs[0].name});
        return sig.outputs[0].type == PinType::Any && t != PinType::None ? t : PinType::Any;
    };
    for (const mat::Node& node : m.nodes) {
        GraphNodeView view;
        view.id = node.id;
        view.x = node.x;
        view.y = node.y;
        std::string error;
        if (std::optional<mat::NodeSignature> sig = mat::ResolveNode(m, node, &error)) {
            view.title = sig->title;
            view.header_color = MaterialHeaderColor(sig->category);
            view.pure = true; // no exec pins in materials
            for (const mat::PinDesc& p : sig->inputs) {
                GraphPinView pin;
                pin.name = pin.label = p.name;
                pin.color = MaterialPinColor(pin_type(node.id, *sig, p, false));
                pin.connected = linked_in.count({node.id, p.name}) != 0;
                view.inputs.push_back(std::move(pin));
            }
            for (const mat::PinDesc& p : sig->outputs) {
                GraphPinView pin;
                pin.name = pin.label = p.name;
                pin.color = MaterialPinColor(pin_type(node.id, *sig, p, true));
                pin.connected = linked_out.count({node.id, p.name}) != 0;
                view.outputs.push_back(std::move(pin));
            }
        } else {
            view.title = node.type;
            view.header_color = IM_COL32(120, 30, 30, 255);
        }
        for (const mat::Diagnostic& d : analysis.diagnostics) {
            if (d.node != node.id || d.severity != mat::Severity::Error) continue;
            if (!view.error.empty()) view.error += "\n";
            view.error += d.code + ": " + d.message;
        }
        model.nodes.push_back(std::move(view));
    }
    for (const mat::Link& l : m.links) {
        GraphLinkView link;
        link.from_node = l.from.node;
        link.from_pin = l.from.pin;
        link.to_node = l.to.node;
        link.to_pin = l.to.pin;
        link.color = MaterialPinColor(analysis.TypeOf(l.from));
        model.links.push_back(std::move(link));
    }
    return model;
}

// --- Edits ---------------------------------------------------------------------------------

NodeId AddMaterialNode(Material& m, const std::string& type, float x, float y, nlohmann::json config) {
    return m.Add(type, std::move(config), x, y);
}

bool ConnectMaterialPins(Material& m, const GraphPinRef& a, const GraphPinRef& b, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (a.output == b.output) return fail(a.output ? "Can't link two outputs." : "Can't link two inputs.");
    const GraphPinRef& from = a.output ? a : b;
    const GraphPinRef& to = a.output ? b : a;
    if (from.node == to.node) return fail("A node can't link to itself.");
    const mat::Node* from_node = m.Find(from.node);
    const mat::Node* to_node = m.Find(to.node);
    if (from_node == nullptr || to_node == nullptr) return fail("That node no longer exists.");
    const std::optional<mat::NodeSignature> from_sig = mat::ResolveNode(m, *from_node);
    const std::optional<mat::NodeSignature> to_sig = mat::ResolveNode(m, *to_node);
    if (!from_sig || !to_sig) return fail("Fix the node's errors first.");
    const mat::PinDesc* out = from_sig->Output(from.pin);
    const mat::PinDesc* in = to_sig->Input(to.pin);
    if (out == nullptr || in == nullptr) return fail("That pin doesn't exist.");

    // The source's current type (generic outputs are inferred).
    PinType type = out->type;
    if (type == PinType::Any) {
        type = mat::Analyze(m).TypeOf({from.node, from.pin});
        if (type == PinType::None) type = PinType::Float;
    }
    if (in->type == PinType::Any) {
        if (type == PinType::Texture) return fail("A math input can't take a texture; sample it first.");
    } else if (mat::CanConnect(type, in->type) == mat::Fit::No) {
        return fail(std::string("A ") + mat::TypeName(type) + " can't connect to the " + mat::TypeName(in->type) + " input '" +
                    to.pin + "'.");
    }
    // No cycles: `to` must not already lead back to `from`.
    std::set<NodeId> seen;
    std::function<bool(NodeId)> reaches = [&](NodeId n) {
        if (n == from.node) return true;
        if (!seen.insert(n).second) return false;
        for (const mat::Link& l : m.links) {
            if (l.from.node == n && reaches(l.to.node)) return true;
        }
        return false;
    };
    if (reaches(to.node)) return fail("That link would make a loop; material graphs can't have cycles.");

    m.links.erase(std::remove_if(m.links.begin(), m.links.end(),
                                 [&](const mat::Link& l) { return l.to.node == to.node && l.to.pin == to.pin; }),
                  m.links.end());
    m.links.push_back({{from.node, from.pin}, {to.node, to.pin}});
    return true;
}

bool DisconnectMaterial(Material& m, const mat::Link& link) {
    const usize before = m.links.size();
    m.links.erase(std::remove_if(m.links.begin(), m.links.end(),
                                 [&](const mat::Link& l) { return l.from == link.from && l.to == link.to; }),
                  m.links.end());
    return m.links.size() != before;
}

usize DeleteMaterialNodes(Material& m, const std::vector<NodeId>& nodes) {
    std::set<NodeId> doomed;
    for (NodeId id : nodes) {
        const mat::Node* n = m.Find(id);
        if (n != nullptr && n->type != "Material.Output") doomed.insert(id);
    }
    m.nodes.erase(std::remove_if(m.nodes.begin(), m.nodes.end(), [&](const mat::Node& n) { return doomed.count(n.id) != 0; }), m.nodes.end());
    m.links.erase(std::remove_if(m.links.begin(), m.links.end(),
                                 [&](const mat::Link& l) { return doomed.count(l.from.node) || doomed.count(l.to.node); }),
                  m.links.end());
    return doomed.size();
}

std::vector<std::string> ApplyMaterialEdits(Material& m, const GraphViewResult& edits) {
    std::vector<std::string> errors;
    for (const auto& [id, pos] : edits.moved) {
        if (mat::Node* n = m.Find(id)) {
            n->x = pos.first;
            n->y = pos.second;
        }
    }
    if (edits.disconnect) {
        DisconnectMaterial(m, {{edits.disconnected.from_node, edits.disconnected.from_pin},
                               {edits.disconnected.to_node, edits.disconnected.to_pin}});
    }
    if (edits.connect) {
        std::string error;
        if (!ConnectMaterialPins(m, edits.connect_from, edits.connect_to, &error)) errors.push_back(error);
    }
    if (!edits.deleted.empty() && DeleteMaterialNodes(m, std::vector<NodeId>(edits.deleted.begin(), edits.deleted.end())) < edits.deleted.size()) {
        errors.push_back("The Material Output can't be deleted.");
    }
    return errors;
}

bool ConnectToNewMaterialNode(Material& m, const GraphPinRef& from, NodeId node) {
    const mat::Node* n = m.Find(node);
    if (n == nullptr) return false;
    const std::optional<mat::NodeSignature> sig = mat::ResolveNode(m, *n);
    if (!sig) return false;
    for (const mat::PinDesc& p : from.output ? sig->inputs : sig->outputs) {
        if (ConnectMaterialPins(m, from, {node, p.name, !from.output})) return true;
    }
    return false;
}

// --- Palette ----------------------------------------------------------------------------------

std::vector<mat::PaletteEntry> SearchMaterialPalette(const Material& m, std::string_view query, const GraphPinRef* from,
                                                     usize max_results) {
    const bool has_output = std::any_of(m.nodes.begin(), m.nodes.end(), [](const mat::Node& n) { return n.type == "Material.Output"; });
    std::optional<PinType> from_type;
    if (from != nullptr) {
        if (const mat::Node* n = m.Find(from->node)) {
            if (std::optional<mat::NodeSignature> sig = mat::ResolveNode(m, *n)) {
                if (const mat::PinDesc* p = from->output ? sig->Output(from->pin) : sig->Input(from->pin)) {
                    from_type = p->type;
                    if (from_type == PinType::Any && from->output) {
                        const PinType t = mat::Analyze(m).TypeOf({from->node, from->pin});
                        from_type = t == PinType::None ? PinType::Float : t;
                    }
                }
            }
        }
    }
    auto fits = [&](PinType pin) {
        if (!from_type) return true;
        const PinType source = from->output ? *from_type : pin, target = from->output ? pin : *from_type;
        if (target == PinType::Any) return source != PinType::Texture;
        if (source == PinType::Any) return target != PinType::Texture;
        return mat::CanConnect(source, target) != mat::Fit::No;
    };
    std::vector<std::pair<int, mat::PaletteEntry>> scored;
    for (mat::PaletteEntry& entry : mat::ListNodeTypes(m)) {
        if (entry.id == "Material.Output" && has_output) continue;
        const int score = std::max(FuzzyScore(query, entry.title), FuzzyScore(query, entry.category + " " + entry.title) / 2);
        if (score == 0) continue;
        if (from_type) {
            mat::Node probe;
            probe.type = entry.id;
            const std::optional<mat::NodeSignature> sig = mat::ResolveNode(m, probe);
            if (!sig) continue;
            const std::vector<mat::PinDesc>& pins = from->output ? sig->inputs : sig->outputs;
            if (std::none_of(pins.begin(), pins.end(), [&](const mat::PinDesc& p) { return fits(p.type); })) continue;
        }
        scored.push_back({score, std::move(entry)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<mat::PaletteEntry> out;
    for (usize i = 0; i < scored.size() && i < max_results; ++i) out.push_back(std::move(scored[i].second));
    return out;
}

} // namespace aether::editor
