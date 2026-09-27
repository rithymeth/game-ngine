#include "aether/blueprint/validate.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace aether::bp {

namespace {

struct GraphCheck {
    const Blueprint& bp;
    const Graph& graph;
    ValidationResult& result;
    std::map<NodeId, NodeSignature> signatures;

    void Report(const char* code, Severity severity, NodeId node, std::string pin, std::string message) {
        result.diagnostics.push_back({code, severity, graph.name, node, std::move(pin), std::move(message)});
        (severity == Severity::Error ? result.errors : result.warnings)++;
    }

    std::string Title(NodeId id) const {
        auto it = signatures.find(id);
        return it != signatures.end() ? it->second.title : "node " + std::to_string(id);
    }

    void ResolveNodes() {
        for (const Node& node : graph.nodes) {
            NodeError error;
            std::optional<NodeSignature> s = ResolveNode(bp, graph, node, &error);
            if (!s) {
                Report(error.code.empty() ? "BP007" : error.code.c_str(), Severity::Error, node.id, "", error.message);
                continue;
            }
            if (s->kind == NodeKind::Latent && graph.kind == GraphKind::Function) {
                Report("BP002", Severity::Error, node.id, "",
                       "'" + s->title + "' is a latent node and can't be used in a function. Move it to the "
                                        "Event Graph or a macro.");
            }
            if (s->IsEvent() && graph.kind != GraphKind::EventGraph) {
                Report("BP010", Severity::Error, node.id, "",
                       "'" + s->title + "' is an event and belongs in the Event Graph.");
            }
            // Stored defaults must be for real inputs, of the right type.
            if (node.defaults.is_object()) {
                for (const auto& [pin, value] : node.defaults.items()) {
                    const PinDesc* desc = s->Find(pin, PinDir::In);
                    Value parsed;
                    if (desc == nullptr || desc->type.IsExec()) {
                        Report("BP008", Severity::Error, node.id, pin,
                               "'" + s->title + "' has no input '" + pin + "' (its default is left over).");
                    } else if (!ValueFromJson(value, desc->type, parsed)) {
                        Report("BP008", Severity::Error, node.id, pin,
                               "The default for '" + pin + "' isn't a " + TypeName(desc->type) + ".");
                    }
                }
            }
            signatures.emplace(node.id, std::move(*s));
        }
    }

    void CheckEntries() {
        if (graph.kind != GraphKind::Function) {
            return;
        }
        const auto entries = std::count_if(graph.nodes.begin(), graph.nodes.end(),
                                           [](const Node& n) { return n.type == "Function.Entry"; });
        if (entries != 1) {
            Report("BP011", Severity::Error, 0, "",
                   "The function '" + graph.name + "' needs exactly one Function Entry node (it has " +
                       std::to_string(entries) + ").");
        }
    }

    // Returns the pin, or reports why it can't be used.
    const PinDesc* LinkEnd(const PinRef& ref, PinDir dir, NodeId other) {
        if (graph.Find(ref.node) == nullptr) {
            Report("BP008", Severity::Error, other, "", "A link goes to node " + std::to_string(ref.node) + ", which doesn't exist.");
            return nullptr;
        }
        auto it = signatures.find(ref.node);
        if (it == signatures.end()) {
            return nullptr; // the node itself failed; already reported
        }
        const PinDesc* pin = it->second.Find(ref.pin, dir);
        if (pin == nullptr) {
            const bool wrong_way = it->second.Find(ref.pin) != nullptr;
            Report("BP008", Severity::Error, ref.node, ref.pin,
                   wrong_way ? "'" + ref.pin + "' is an " + (dir == PinDir::In ? "output" : "input") +
                                   "; links go from an output to an input."
                             : "'" + it->second.title + "' has no pin '" + ref.pin + "'.");
        }
        return pin;
    }

    void CheckLinks() {
        std::map<std::pair<NodeId, std::string>, int> into_data;
        std::map<std::pair<NodeId, std::string>, int> out_of_exec;
        for (const Link& link : graph.links) {
            const PinDesc* from = LinkEnd(link.from, PinDir::Out, link.to.node);
            const PinDesc* to = LinkEnd(link.to, PinDir::In, link.from.node);
            if (from == nullptr || to == nullptr) {
                continue;
            }
            switch (CanConnect(from->type, to->type)) {
            case Compat::No:
                Report("BP001", Severity::Error, link.to.node, link.to.pin,
                       "Pin '" + to->name + "' needs a " + TypeName(to->type) + ", but it's connected to a " +
                           TypeName(from->type) + ".");
                continue;
            case Compat::ConvertLossy:
                Report("BP104", Severity::Warning, link.to.node, link.to.pin,
                       "Float to int conversion truncates (1.9 becomes 1). Use Round, Floor or Ceil to be explicit.");
                break;
            default: break;
            }
            if (to->type.IsExec()) {
                out_of_exec[{link.from.node, link.from.pin}]++;
            } else {
                into_data[{link.to.node, link.to.pin}]++;
            }
        }
        for (const auto& [pin, count] : into_data) {
            if (count > 1) {
                Report("BP009", Severity::Error, pin.first, pin.second,
                       "Input '" + pin.second + "' has " + std::to_string(count) + " links; a data input takes one.");
            }
        }
        for (const auto& [pin, count] : out_of_exec) {
            if (count > 1) {
                Report("BP009", Severity::Error, pin.first, pin.second,
                       "Exec output '" + pin.second + "' has " + std::to_string(count) +
                           " links; it can only run one thing. Use a Sequence.");
            }
        }
    }

    bool Linked(NodeId node, const std::string& pin) const {
        return std::any_of(graph.links.begin(), graph.links.end(),
                           [&](const Link& l) { return l.to.node == node && l.to.pin == pin; });
    }

    void CheckUnconnected() {
        for (const Node& node : graph.nodes) {
            auto it = signatures.find(node.id);
            if (it == signatures.end()) continue;
            for (const PinDesc& pin : it->second.pins) {
                if ((pin.flags & Pin_WarnIfUnconnected) == 0 || pin.dir != PinDir::In || Linked(node.id, pin.name)) {
                    continue;
                }
                Value value = pin.default_value;
                if (node.defaults.contains(pin.name)) {
                    ValueFromJson(node.defaults[pin.name], pin.type, value);
                }
                Report("BP101", Severity::Warning, node.id, pin.name,
                       "Input '" + pin.name + "' isn't connected and will use its default value (" + ValueText(value) + ").");
            }
        }
    }

    // BP003: pure nodes feeding each other in a loop.
    void CheckPureCycles() {
        std::map<NodeId, std::vector<NodeId>> feeds; // pure node -> pure nodes reading it
        for (const Link& link : graph.links) {
            auto a = signatures.find(link.from.node), b = signatures.find(link.to.node);
            if (a != signatures.end() && b != signatures.end() && a->second.IsPure() && b->second.IsPure()) {
                feeds[link.from.node].push_back(link.to.node);
            }
        }
        std::map<NodeId, int> state; // 0 new, 1 on the stack, 2 done
        std::vector<NodeId> stack;
        std::set<NodeId> reported;
        std::function<void(NodeId)> visit = [&](NodeId id) {
            state[id] = 1;
            stack.push_back(id);
            for (NodeId next : feeds[id]) {
                if (state[next] == 1) {
                    const auto begin = std::find(stack.begin(), stack.end(), next);
                    if (!reported.count(next)) {
                        std::string chain;
                        for (auto it = begin; it != stack.end(); ++it) chain += Title(*it) + " -> ";
                        chain += Title(next);
                        Report("BP003", Severity::Error, next, "",
                               "Pure nodes form a loop: " + chain + ". A value can't depend on itself.");
                        for (auto it = begin; it != stack.end(); ++it) reported.insert(*it);
                    }
                } else if (state[next] == 0) {
                    visit(next);
                }
            }
            stack.pop_back();
            state[id] = 2;
        };
        for (const Node& node : graph.nodes) {
            if (state[node.id] == 0 && feeds.count(node.id)) visit(node.id);
        }
    }
};

} // namespace

std::vector<const Diagnostic*> ValidationResult::For(const std::string& graph, NodeId node) const {
    std::vector<const Diagnostic*> out;
    for (const Diagnostic& d : diagnostics) {
        if (d.graph == graph && d.node == node) out.push_back(&d);
    }
    return out;
}

bool ValidationResult::Has(const std::string& code) const {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d) { return d.code == code; });
}

ValidationResult ValidateBlueprint(const Blueprint& blueprint) {
    ValidationResult result;
    std::map<std::string, std::pair<std::string, NodeId>> events; // event key -> first (graph, node)
    for (const Graph& graph : blueprint.graphs) {
        GraphCheck check{blueprint, graph, result, {}};
        check.ResolveNodes();
        check.CheckEntries();
        check.CheckLinks();
        check.CheckUnconnected();
        check.CheckPureCycles();
        for (const Node& node : graph.nodes) {
            auto it = check.signatures.find(node.id);
            if (it == check.signatures.end() || it->second.event_key.empty()) continue;
            if (!events.emplace(it->second.event_key, std::make_pair(graph.name, node.id)).second) {
                check.Report("BP006", Severity::Error, node.id, "",
                             "Event '" + it->second.title + "' appears more than once in this Blueprint.");
            }
        }
    }
    return result;
}

} // namespace aether::bp
