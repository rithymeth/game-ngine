#include "graph/blueprint_graph.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>

namespace aether::editor {

using bp::NodeId;
using bp::PinDesc;
using bp::PinDir;
using bp::ValueType;

namespace {

std::optional<bp::NodeSignature> Resolve(const bp::Blueprint& blueprint, const bp::Graph& graph, const bp::Node& node) {
    return bp::ResolveNode(blueprint, graph, node);
}

const PinDesc* FindPin(const bp::NodeSignature& sig, const GraphPinRef& ref) {
    return sig.Find(ref.pin, ref.output ? PinDir::Out : PinDir::In);
}

// Letters of `query` in order within `text`; higher is better (0 = no match).
int FuzzyScore(std::string_view query, std::string_view text) {
    if (query.empty()) return 1;
    usize t = 0;
    int score = 1;
    int run = 0;
    for (char qc : query) {
        if (qc == ' ') continue;
        const char q = static_cast<char>(std::tolower(static_cast<unsigned char>(qc)));
        bool found = false;
        while (t < text.size()) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[t++])));
            if (c == q) {
                found = true;
                run += 1;
                score += 1 + run * 2; // consecutive letters count more
                break;
            }
            run = 0;
        }
        if (!found) return 0;
    }
    // Prefix and whole-word matches rank first.
    std::string lower(text);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string q;
    for (char c : query) {
        if (c != ' ') q.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower.rfind(q, 0) == 0) score += 50;
    else if (lower.find(q) != std::string::npos) score += 20;
    return score;
}

} // namespace

u32 PinColor(const bp::PinType& type) {
    if (type.IsExec()) return IM_COL32(255, 255, 255, 255);
    switch (type.type) {
    case ValueType::Bool: return IM_COL32(0x8C, 0x1A, 0x1A, 255);
    case ValueType::Int: return IM_COL32(0x1F, 0xB3, 0x9C, 255);
    case ValueType::Float: return IM_COL32(0x9A, 0xDB, 0x47, 255);
    case ValueType::String: return IM_COL32(0xF0, 0x4B, 0xC8, 255);
    case ValueType::Vec3: return IM_COL32(0xF5, 0xC5, 0x42, 255);
    case ValueType::Quat: return IM_COL32(0x9C, 0xC7, 0xFF, 255);
    case ValueType::Entity: return IM_COL32(0x2F, 0x7C, 0xF6, 255);
    case ValueType::Struct: return IM_COL32(0x1D, 0x4E, 0x89, 255);
    default: return IM_COL32(160, 160, 160, 255); // wildcard
    }
}

u32 HeaderColor(const bp::NodeSignature& sig) {
    if (sig.IsEvent()) return IM_COL32(150, 30, 30, 255);
    if (sig.kind == bp::NodeKind::Latent) return IM_COL32(170, 90, 20, 255);
    if (sig.kind == bp::NodeKind::Tunnel || sig.category == "Macros") return IM_COL32(95, 70, 125, 255);
    if (sig.category == "Flow Control") return IM_COL32(85, 85, 90, 255);
    if (sig.IsPure()) return IM_COL32(40, 105, 55, 255);
    return IM_COL32(35, 75, 150, 255); // function calls and other impure nodes
}

GraphViewModel BuildBlueprintView(const bp::Blueprint& blueprint, const bp::Graph& graph,
                                  const BlueprintViewOptions& options) {
    GraphViewModel model;
    std::set<std::pair<NodeId, std::string>> linked_in, linked_out;
    for (const bp::Link& l : graph.links) {
        linked_out.insert({l.from.node, l.from.pin});
        linked_in.insert({l.to.node, l.to.pin});
    }
    std::map<std::pair<NodeId, std::string>, bp::PinType> out_types;
    for (const bp::Node& node : graph.nodes) {
        GraphNodeView view;
        view.id = node.id;
        view.x = node.x;
        view.y = node.y;
        view.comment = node.comment;
        view.highlighted = node.id == options.highlighted;
        view.breakpoint = options.breakpoints.count(node.id) != 0;
        bp::NodeError error;
        if (std::optional<bp::NodeSignature> sig = bp::ResolveNode(blueprint, graph, node, &error)) {
            view.title = sig->title;
            view.header_color = HeaderColor(*sig);
            view.pure = sig->IsPure();
            for (const PinDesc& p : sig->pins) {
                GraphPinView pin;
                pin.name = p.name;
                pin.exec = p.type.IsExec();
                pin.array = p.type.is_array;
                pin.color = PinColor(p.type);
                const bool is_default_exec = pin.exec && (p.name == "exec" || p.name == "then");
                pin.label = is_default_exec ? std::string() : p.name;
                if (p.dir == PinDir::In) {
                    pin.connected = linked_in.count({node.id, p.name}) != 0;
                    view.inputs.push_back(std::move(pin));
                } else {
                    pin.connected = linked_out.count({node.id, p.name}) != 0;
                    out_types[{node.id, p.name}] = p.type;
                    view.outputs.push_back(std::move(pin));
                }
            }
        } else {
            view.title = node.type;
            view.header_color = IM_COL32(120, 30, 30, 255);
            view.error = error.message;
        }
        if (options.diagnostics != nullptr) {
            for (const bp::Diagnostic* d : options.diagnostics->For(graph.name, node.id)) {
                if (d->severity != bp::Severity::Error) continue;
                if (!view.error.empty()) view.error += "\n";
                view.error += d->code + ": " + d->message;
            }
        }
        model.nodes.push_back(std::move(view));
    }
    for (const bp::Link& l : graph.links) {
        GraphLinkView link;
        link.from_node = l.from.node;
        link.from_pin = l.from.pin;
        link.to_node = l.to.node;
        link.to_pin = l.to.pin;
        auto type = out_types.find({l.from.node, l.from.pin});
        link.color = type != out_types.end() ? PinColor(type->second) : IM_COL32(200, 200, 200, 255);
        link.glow = type != out_types.end() && type->second.IsExec() && options.fired.count(l.from.node) ? 1.0f : 0.0f;
        model.links.push_back(std::move(link));
    }
    return model;
}

NodeId AddNode(bp::Graph& graph, const std::string& type, float x, float y, nlohmann::json config) {
    return bp::GraphBuilder(graph).Add(type, std::move(config), x, y);
}

bool ConnectPins(const bp::Blueprint& blueprint, bp::Graph& graph, const GraphPinRef& from, const GraphPinRef& to,
                 std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!from.output || to.output) return fail("A link goes from an output to an input.");
    if (from.node == to.node) return fail("A node can't be linked to itself.");
    const bp::Node* a = graph.Find(from.node);
    const bp::Node* b = graph.Find(to.node);
    if (a == nullptr || b == nullptr) return fail("That node doesn't exist.");
    const std::optional<bp::NodeSignature> sa = Resolve(blueprint, graph, *a), sb = Resolve(blueprint, graph, *b);
    const PinDesc* pa = sa ? FindPin(*sa, from) : nullptr;
    const PinDesc* pb = sb ? FindPin(*sb, to) : nullptr;
    if (pa == nullptr || pb == nullptr) return fail("That pin doesn't exist.");
    if (bp::CanConnect(pa->type, pb->type) == bp::Compat::No) {
        return fail("A " + bp::TypeName(pa->type) + " can't connect to a " + bp::TypeName(pb->type) + ".");
    }
    // One link into a data input, one out of an exec output: the new one replaces it.
    auto& links = graph.links;
    links.erase(std::remove_if(links.begin(), links.end(),
                               [&](const bp::Link& l) {
                                   const bool same_input = !pb->type.IsExec() && l.to.node == to.node && l.to.pin == to.pin;
                                   const bool same_exec_out = pa->type.IsExec() && l.from.node == from.node && l.from.pin == from.pin;
                                   return same_input || same_exec_out;
                               }),
                links.end());
    links.push_back({{from.node, from.pin}, {to.node, to.pin}});
    return true;
}

bool Disconnect(bp::Graph& graph, const bp::Link& link) {
    const auto before = graph.links.size();
    graph.links.erase(std::remove(graph.links.begin(), graph.links.end(), link), graph.links.end());
    return graph.links.size() != before;
}

void DeleteNodes(bp::Graph& graph, const std::vector<NodeId>& nodes) {
    const std::set<NodeId> doomed(nodes.begin(), nodes.end());
    graph.nodes.erase(std::remove_if(graph.nodes.begin(), graph.nodes.end(),
                                     [&](const bp::Node& n) { return doomed.count(n.id) != 0; }),
                      graph.nodes.end());
    graph.links.erase(std::remove_if(graph.links.begin(), graph.links.end(),
                                     [&](const bp::Link& l) { return doomed.count(l.from.node) || doomed.count(l.to.node); }),
                      graph.links.end());
}

std::vector<std::string> ApplyGraphEdits(const bp::Blueprint& blueprint, bp::Graph& graph, const GraphViewResult& edits) {
    std::vector<std::string> errors;
    for (const auto& [id, pos] : edits.moved) {
        if (bp::Node* n = graph.Find(id)) {
            n->x = pos.first;
            n->y = pos.second;
        }
    }
    if (edits.disconnect) {
        Disconnect(graph, {{edits.disconnected.from_node, edits.disconnected.from_pin},
                           {edits.disconnected.to_node, edits.disconnected.to_pin}});
    }
    if (edits.connect) {
        std::string error;
        if (!ConnectPins(blueprint, graph, edits.connect_from, edits.connect_to, &error)) errors.push_back(error);
    }
    if (!edits.deleted.empty()) DeleteNodes(graph, std::vector<NodeId>(edits.deleted.begin(), edits.deleted.end()));
    return errors;
}

bool ConnectToNewNode(const bp::Blueprint& blueprint, bp::Graph& graph, const GraphPinRef& from, NodeId node) {
    const bp::Node* n = graph.Find(node);
    if (n == nullptr) return false;
    const std::optional<bp::NodeSignature> sig = Resolve(blueprint, graph, *n);
    if (!sig) return false;
    for (const PinDesc& p : sig->pins) {
        if ((p.dir == PinDir::In) != from.output) continue; // the opposite direction
        const GraphPinRef other{node, p.name, p.dir == PinDir::Out};
        if (from.output ? ConnectPins(blueprint, graph, from, other) : ConnectPins(blueprint, graph, other, from)) {
            return true;
        }
    }
    return false;
}

std::vector<bp::PaletteEntry> SearchPalette(const bp::Blueprint& blueprint, const bp::Graph& graph,
                                            std::string_view query, const GraphPinRef* from, usize max_results) {
    std::optional<bp::PinType> from_type;
    if (from != nullptr) {
        if (const bp::Node* n = graph.Find(from->node)) {
            if (std::optional<bp::NodeSignature> sig = Resolve(blueprint, graph, *n)) {
                if (const PinDesc* p = FindPin(*sig, *from)) from_type = p->type;
            }
        }
    }
    std::vector<std::pair<int, bp::PaletteEntry>> scored;
    for (bp::PaletteEntry& entry : bp::ListNodeTypes(blueprint)) {
        const int score = std::max(FuzzyScore(query, entry.title), FuzzyScore(query, entry.category + " " + entry.title) / 2);
        if (score == 0) continue;
        if (from_type) {
            bp::Node probe;
            probe.type = entry.id;
            const std::optional<bp::NodeSignature> sig = Resolve(blueprint, graph, probe);
            if (!sig) continue;
            const bool fits = std::any_of(sig->pins.begin(), sig->pins.end(), [&](const PinDesc& p) {
                if ((p.dir == PinDir::In) != from->output) return false;
                return from->output ? bp::CanConnect(*from_type, p.type) != bp::Compat::No
                                    : bp::CanConnect(p.type, *from_type) != bp::Compat::No;
            });
            if (!fits) continue;
        }
        scored.push_back({score, std::move(entry)});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<bp::PaletteEntry> out;
    for (usize i = 0; i < scored.size() && i < max_results; ++i) out.push_back(std::move(scored[i].second));
    return out;
}

} // namespace aether::editor
