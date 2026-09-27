#include "macros.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>

namespace aether::bp {

namespace {

constexpr int kMaxExpansions = 4096; // a guard against pathological graphs (cycles are refused earlier)

std::string MacroName(const Node& node) { return node.type.substr(6); } // after "Macro:"
bool IsInstance(const Node& node) { return node.type.rfind("Macro:", 0) == 0; }

} // namespace

void CheckMacroCycles(const Blueprint& bp, ValidationResult& result) {
    std::map<std::string, int> state; // 0 unvisited, 1 on the stack, 2 done
    std::set<std::string> reported;
    std::function<void(const Graph&)> visit = [&](const Graph& macro) {
        state[macro.name] = 1;
        for (const Node& node : macro.nodes) {
            if (!IsInstance(node)) continue;
            const Graph* inner = bp.FindGraph(MacroName(node));
            if (inner == nullptr || inner->kind != GraphKind::Macro) continue; // BP004 elsewhere
            if (state[inner->name] == 1) {
                if (reported.insert(macro.name + "/" + std::to_string(node.id)).second) {
                    result.diagnostics.push_back({"BP016", Severity::Error, macro.name, node.id, "",
                                                  "The macro '" + inner->name +
                                                      "' ends up containing itself here. A macro can't be inlined into itself."});
                    result.errors++;
                }
            } else if (state[inner->name] == 0) {
                visit(*inner);
            }
        }
        state[macro.name] = 2;
    };
    for (const Graph& g : bp.graphs) {
        if (g.kind == GraphKind::Macro && state[g.name] == 0) visit(g);
    }
}

void ExpandMacros(const Blueprint& bp, Graph& graph, std::map<NodeId, NodeId>& origin) {
    for (int round = 0; round < kMaxExpansions; ++round) {
        auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(), IsInstance);
        if (it == graph.nodes.end()) return;
        const Node instance = *it;
        const Graph* macro = bp.FindGraph(MacroName(instance));
        const NodeId root = origin.count(instance.id) != 0 ? origin[instance.id] : instance.id;
        graph.nodes.erase(it);
        if (macro == nullptr) continue; // validation reported it

        // Outer wiring of the instance, by pin.
        std::map<std::string, std::vector<PinRef>> sources, targets;
        std::vector<Link> kept;
        for (const Link& l : graph.links) {
            if (l.to.node == instance.id) sources[l.to.pin].push_back(l.from);
            else if (l.from.node == instance.id) targets[l.from.pin].push_back(l.to);
            else kept.push_back(l);
        }
        graph.links = std::move(kept);

        // The macro's tunnels and a fresh ID for each of its other nodes.
        const NodeId base = graph.NextId();
        std::set<NodeId> inputs, outputs;
        for (const Node& n : macro->nodes) {
            if (n.type == "Macro.Inputs") inputs.insert(n.id);
            else if (n.type == "Macro.Outputs") outputs.insert(n.id);
        }
        auto fresh = [&](NodeId inner) { return base + inner; };
        for (const Node& n : macro->nodes) {
            if (inputs.count(n.id) || outputs.count(n.id)) continue;
            Node copy = n;
            copy.id = fresh(n.id);
            graph.nodes.push_back(std::move(copy));
            origin[fresh(n.id)] = root;
        }
        // An input the instance leaves unconnected feeds its value (the
        // instance's default, else the macro's) through a literal node of the
        // input's type, so the usual conversions apply where it's used.
        std::map<std::string, PinRef> literals;
        auto literal_for = [&](const std::string& pin) -> std::optional<PinRef> {
            if (auto it = literals.find(pin); it != literals.end()) return it->second;
            const Variable* input = nullptr;
            for (const Variable& v : macro->inputs) {
                if (v.name == pin) input = &v;
            }
            if (input == nullptr || input->type.IsExec() || input->type.is_array) return std::nullopt;
            static const std::map<ValueType, const char*> kLiteral = {
                {ValueType::Bool, "Literal.Bool"}, {ValueType::Int, "Literal.Int"}, {ValueType::Float, "Literal.Float"},
                {ValueType::String, "Literal.String"}, {ValueType::Vec3, "Literal.Vec3"}};
            auto kind = kLiteral.find(input->type.type);
            if (kind == kLiteral.end()) return std::nullopt; // Quat, Entity: their pins' own defaults apply
            Node literal;
            literal.id = graph.NextId();
            literal.type = kind->second;
            literal.config["value"] = instance.defaults.contains(pin) ? instance.defaults[pin] : ValueToJson(input->default_value);
            graph.nodes.push_back(literal);
            origin[literal.id] = root;
            return literals[pin] = PinRef{literal.id, "value"};
        };
        auto feed_default = [&](const PinRef& at, const std::string& pin) {
            if (std::optional<PinRef> literal = literal_for(pin)) graph.links.push_back({*literal, at});
        };
        auto is_exec = [&](const std::vector<Variable>& vars, const std::string& pin) {
            for (const Variable& v : vars) {
                if (v.name == pin) return v.type.IsExec();
            }
            return false;
        };

        for (const Link& l : macro->links) {
            const bool from_in = inputs.count(l.from.node) != 0, to_out = outputs.count(l.to.node) != 0;
            if (!from_in && !to_out) {
                graph.links.push_back({{fresh(l.from.node), l.from.pin}, {fresh(l.to.node), l.to.pin}});
            } else if (from_in && !to_out) {
                const PinRef inner{fresh(l.to.node), l.to.pin};
                const auto& outer = sources[l.from.pin];
                for (const PinRef& s : outer) graph.links.push_back({s, inner});
                if (outer.empty() && !is_exec(macro->inputs, l.from.pin)) feed_default(inner, l.from.pin);
            } else if (!from_in && to_out) {
                for (const PinRef& t : targets[l.to.pin]) graph.links.push_back({{fresh(l.from.node), l.from.pin}, t});
            } else { // straight through the macro
                const auto& outer = sources[l.from.pin];
                for (const PinRef& t : targets[l.to.pin]) {
                    for (const PinRef& s : outer) graph.links.push_back({s, t});
                    if (outer.empty() && !is_exec(macro->inputs, l.from.pin)) feed_default(t, l.from.pin);
                }
            }
        }
    }
}

} // namespace aether::bp
