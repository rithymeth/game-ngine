#include "aether/blueprint/graph.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace aether::bp {

using nlohmann::json;

namespace {

constexpr int kFormatVersion = 1;

const char* KindName(GraphKind kind) {
    switch (kind) {
    case GraphKind::EventGraph: return "EventGraph";
    case GraphKind::Function: return "Function";
    case GraphKind::Macro: return "Macro";
    }
    return "EventGraph";
}

bool ParseKind(const std::string& name, GraphKind& out) {
    if (name == "EventGraph") out = GraphKind::EventGraph;
    else if (name == "Function") out = GraphKind::Function;
    else if (name == "Macro") out = GraphKind::Macro;
    else return false;
    return true;
}

json VariableToJson(const Variable& v) {
    json j = {{"name", v.name}, {"type", TypeName(v.type)}, {"default", ValueToJson(v.default_value)}};
    json flags = json::array();
    if (v.flags & Var_InstanceEditable) flags.push_back("InstanceEditable");
    if (v.flags & Var_ExposeOnSpawn) flags.push_back("ExposeOnSpawn");
    if (!flags.empty()) j["flags"] = flags;
    if (!v.tooltip.empty()) j["tooltip"] = v.tooltip;
    if (!v.category.empty()) j["category"] = v.category;
    return j;
}

// `allow_exec`: macro signatures have exec pins too.
bool VariableFromJson(const json& j, Variable& v, std::string& error, bool allow_exec = false) {
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
        error = "a variable needs a name";
        return false;
    }
    v.name = j["name"].get<std::string>();
    const std::string type = j.value("type", "float");
    std::optional<PinType> parsed = ParseType(type);
    if (!parsed || (parsed->IsExec() && !allow_exec)) {
        error = "variable '" + v.name + "' has an unknown type '" + type + "'";
        return false;
    }
    v.type = *parsed;
    v.default_value = DefaultValue(v.type);
    if (j.contains("default") && !j["default"].is_null() && !ValueFromJson(j["default"], v.type, v.default_value)) {
        error = "variable '" + v.name + "' has a default that isn't a " + type;
        return false;
    }
    v.flags = Var_None;
    for (const json& flag : j.value("flags", json::array())) {
        if (flag == "InstanceEditable") v.flags |= Var_InstanceEditable;
        else if (flag == "ExposeOnSpawn") v.flags |= Var_ExposeOnSpawn;
    }
    v.tooltip = j.value("tooltip", "");
    v.category = j.value("category", "");
    return true;
}

bool ReadPinRef(const json& j, PinRef& out) {
    if (!j.is_array() || j.size() != 2 || !j[0].is_number_unsigned() || !j[1].is_string()) {
        return false;
    }
    out.node = j[0].get<NodeId>();
    out.pin = j[1].get<std::string>();
    return true;
}

} // namespace

Node* Graph::Find(NodeId id) {
    for (Node& node : nodes) {
        if (node.id == id) return &node;
    }
    return nullptr;
}

const Node* Graph::Find(NodeId id) const { return const_cast<Graph*>(this)->Find(id); }

NodeId Graph::NextId() const {
    NodeId highest = 0;
    for (const Node& node : nodes) highest = std::max(highest, node.id);
    return highest + 1;
}

const Variable* Blueprint::FindVariable(std::string_view name) const {
    for (const Variable& v : variables) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

const Graph* Blueprint::FindGraph(std::string_view name) const { return const_cast<Blueprint*>(this)->FindGraph(name); }

Graph* Blueprint::FindGraph(std::string_view name) {
    for (Graph& g : graphs) {
        if (g.name == name) return &g;
    }
    return nullptr;
}

const Dispatcher* Blueprint::FindDispatcher(std::string_view name) const {
    for (const Dispatcher& d : dispatchers) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

bool Blueprint::Implements(std::string_view interface_name) const {
    return std::find(interfaces.begin(), interfaces.end(), interface_name) != interfaces.end();
}

json BlueprintToJson(const Blueprint& bp) {
    json variables = json::array();
    for (const Variable& v : bp.variables) variables.push_back(VariableToJson(v));
    json graphs = json::array();
    for (const Graph& g : bp.graphs) {
        json nodes = json::array();
        for (const Node& n : g.nodes) {
            json node = {{"id", n.id}, {"type", n.type}, {"pos", json::array({n.x, n.y})}};
            if (!n.config.empty()) node["config"] = n.config;
            if (!n.defaults.empty()) node["defaults"] = n.defaults;
            if (!n.comment.empty()) node["comment"] = n.comment;
            nodes.push_back(std::move(node));
        }
        json links = json::array();
        for (const Link& l : g.links) {
            links.push_back({{"from", json::array({l.from.node, l.from.pin})}, {"to", json::array({l.to.node, l.to.pin})}});
        }
        json graph = {{"name", g.name}, {"kind", KindName(g.kind)}, {"nodes", nodes}, {"links", links}};
        if (!g.comments.empty()) {
            json comments = json::array();
            for (const CommentBox& c : g.comments) {
                comments.push_back({{"text", c.text}, {"rect", json::array({c.x, c.y, c.width, c.height})}, {"color", c.color}});
            }
            graph["comments"] = comments;
        }
        if (g.kind == GraphKind::Function || g.kind == GraphKind::Macro) {
            json inputs = json::array(), outputs = json::array();
            for (const Variable& v : g.inputs) inputs.push_back(VariableToJson(v));
            for (const Variable& v : g.outputs) outputs.push_back(VariableToJson(v));
            graph["inputs"] = inputs;
            graph["outputs"] = outputs;
            if (g.pure) graph["pure"] = true;
        }
        graphs.push_back(std::move(graph));
    }
    json out = {{"$type", "Blueprint"}, {"$version", kFormatVersion}, {"parent", bp.parent},
                {"variables", variables}, {"graphs", graphs}};
    if (!bp.dispatchers.empty()) {
        json dispatchers = json::array();
        for (const Dispatcher& d : bp.dispatchers) {
            json params = json::array();
            for (const Variable& v : d.params) params.push_back(VariableToJson(v));
            dispatchers.push_back({{"name", d.name}, {"params", params}});
        }
        out["dispatchers"] = dispatchers;
    }
    if (!bp.interfaces.empty()) out["interfaces"] = bp.interfaces;
    return out;
}

bool BlueprintFromJson(const json& j, Blueprint& out, std::string* error) {
    std::string message;
    auto fail = [&](const std::string& text) {
        if (error != nullptr) *error = text;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "Blueprint") {
        return fail("not a Blueprint file");
    }
    if (j.value("$version", 0) > kFormatVersion) {
        return fail("saved by a newer version of the engine (format " + std::to_string(j.value("$version", 0)) + ")");
    }
    Blueprint bp;
    bp.parent = j.value("parent", "native:Entity");
    for (const json& v : j.value("variables", json::array())) {
        Variable variable;
        if (!VariableFromJson(v, variable, message)) return fail(message);
        if (bp.FindVariable(variable.name) != nullptr) return fail("two variables are named '" + variable.name + "'");
        bp.variables.push_back(std::move(variable));
    }
    for (const json& d : j.value("dispatchers", json::array())) {
        Dispatcher dispatcher;
        dispatcher.name = d.value("name", "");
        if (dispatcher.name.empty()) return fail("an event dispatcher needs a name");
        for (const json& p : d.value("params", json::array())) {
            Variable param;
            if (!VariableFromJson(p, param, message)) return fail("dispatcher '" + dispatcher.name + "': " + message);
            dispatcher.params.push_back(std::move(param));
        }
        if (bp.FindDispatcher(dispatcher.name) != nullptr) return fail("two event dispatchers are named '" + dispatcher.name + "'");
        bp.dispatchers.push_back(std::move(dispatcher));
    }
    for (const json& i : j.value("interfaces", json::array())) {
        if (!i.is_string() || i.get<std::string>().empty()) return fail("an implemented interface needs a name");
        bp.interfaces.push_back(i.get<std::string>());
    }
    for (const json& g : j.value("graphs", json::array())) {
        Graph graph;
        graph.name = g.value("name", "");
        if (graph.name.empty()) return fail("a graph needs a name");
        if (!ParseKind(g.value("kind", "EventGraph"), graph.kind)) {
            return fail("graph '" + graph.name + "' has an unknown kind");
        }
        graph.pure = g.value("pure", false);
        for (const char* side : {"inputs", "outputs"}) {
            for (const json& v : g.value(side, json::array())) {
                Variable variable;
                if (!VariableFromJson(v, variable, message, graph.kind == GraphKind::Macro)) {
                    return fail("graph '" + graph.name + "': " + message);
                }
                (std::string(side) == "inputs" ? graph.inputs : graph.outputs).push_back(std::move(variable));
            }
        }
        for (const json& n : g.value("nodes", json::array())) {
            Node node;
            if (!n.is_object() || !n.contains("id") || !n["id"].is_number_unsigned() || n["id"].get<NodeId>() == 0 ||
                !n.contains("type") || !n["type"].is_string()) {
                return fail("graph '" + graph.name + "' has a node without an id or type");
            }
            node.id = n["id"].get<NodeId>();
            if (graph.Find(node.id) != nullptr) {
                return fail("graph '" + graph.name + "' has two nodes with id " + std::to_string(node.id));
            }
            node.type = n["type"].get<std::string>();
            const json pos = n.value("pos", json::array({0.0, 0.0}));
            if (pos.is_array() && pos.size() == 2 && pos[0].is_number() && pos[1].is_number()) {
                node.x = pos[0].get<float>();
                node.y = pos[1].get<float>();
            }
            node.config = n.value("config", json::object());
            node.defaults = n.value("defaults", json::object());
            node.comment = n.value("comment", "");
            graph.nodes.push_back(std::move(node));
        }
        for (const json& l : g.value("links", json::array())) {
            Link link;
            if (!l.is_object() || !ReadPinRef(l.value("from", json()), link.from) ||
                !ReadPinRef(l.value("to", json()), link.to)) {
                return fail("graph '" + graph.name + "' has a malformed link");
            }
            graph.links.push_back(std::move(link));
        }
        for (const json& c : g.value("comments", json::array())) {
            CommentBox box;
            const json rect = c.is_object() ? c.value("rect", json()) : json();
            if (!rect.is_array() || rect.size() != 4 || !std::all_of(rect.begin(), rect.end(), [](const json& v) { return v.is_number(); })) {
                return fail("graph '" + graph.name + "' has a malformed comment box");
            }
            box.text = c.value("text", "");
            box.x = rect[0].get<float>();
            box.y = rect[1].get<float>();
            box.width = rect[2].get<float>();
            box.height = rect[3].get<float>();
            box.color = c.value("color", box.color);
            graph.comments.push_back(std::move(box));
        }
        if (bp.FindGraph(graph.name) != nullptr) return fail("two graphs are named '" + graph.name + "'");
        bp.graphs.push_back(std::move(graph));
    }
    out = std::move(bp);
    return true;
}

bool SaveBlueprint(const Blueprint& blueprint, const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) *error = "can't write " + path.string();
        return false;
    }
    file << BlueprintToJson(blueprint).dump(2) << '\n';
    return static_cast<bool>(file);
}

bool LoadBlueprint(const std::filesystem::path& path, Blueprint& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) *error = "can't read " + path.string();
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    json j = json::parse(text.str(), nullptr, false);
    if (j.is_discarded()) {
        if (error != nullptr) *error = path.filename().string() + " isn't valid JSON";
        return false;
    }
    return BlueprintFromJson(j, out, error);
}

NodeId GraphBuilder::Add(const std::string& type, json config, float x, float y) {
    Node node;
    node.id = graph_.NextId();
    node.type = type;
    node.config = config.is_object() ? std::move(config) : json::object();
    node.x = x;
    node.y = y;
    graph_.nodes.push_back(std::move(node));
    return graph_.nodes.back().id;
}

GraphBuilder& GraphBuilder::Connect(NodeId from, const std::string& from_pin, NodeId to, const std::string& to_pin) {
    graph_.links.push_back({{from, from_pin}, {to, to_pin}});
    return *this;
}

GraphBuilder& GraphBuilder::Default(NodeId node, const std::string& pin, json value) {
    if (Node* n = graph_.Find(node)) {
        n->defaults[pin] = std::move(value);
    }
    return *this;
}

} // namespace aether::bp
