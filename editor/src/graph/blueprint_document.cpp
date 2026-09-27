#include "graph/blueprint_document.h"

#include "aether/blueprint/nodes.h"
#include "graph/blueprint_graph.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace aether::editor {

using bp::Graph;
using bp::GraphKind;
using bp::Node;
using bp::NodeId;
using nlohmann::json;

namespace {

constexpr usize kMaxUndo = 200;

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

// Rewrites node types that refer to a renamed thing: "Var.Get:Old" -> "Var.Get:New".
void RenameTypes(bp::Blueprint& blueprint, const std::vector<std::string>& prefixes, const std::string& from,
                 const std::string& to) {
    for (Graph& g : blueprint.graphs) {
        for (Node& n : g.nodes) {
            for (const std::string& prefix : prefixes) {
                if (n.type == prefix + from) n.type = prefix + to;
            }
        }
    }
}

// Removes the nodes of these types (and their links) from every graph.
void RemoveTypes(bp::Blueprint& blueprint, const std::set<std::string>& types) {
    for (Graph& g : blueprint.graphs) {
        std::vector<NodeId> doomed;
        for (const Node& n : g.nodes) {
            if (types.count(n.type) != 0) doomed.push_back(n.id);
        }
        DeleteNodes(g, doomed);
    }
}

// Breaks links whose ends no longer resolve or fit (after a type change).
void DropBrokenLinks(bp::Blueprint& blueprint) {
    for (Graph& g : blueprint.graphs) {
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
                                     [&](const bp::Link& l) {
                                         const Node* from = g.Find(l.from.node);
                                         const Node* to = g.Find(l.to.node);
                                         if (from == nullptr || to == nullptr) return true;
                                         const auto a = bp::ResolveNode(blueprint, g, *from);
                                         const auto b = bp::ResolveNode(blueprint, g, *to);
                                         if (!a || !b) return false; // a broken node: leave it for validation to report
                                         const bp::PinDesc* out = a->Find(l.from.pin, bp::PinDir::Out);
                                         const bp::PinDesc* in = b->Find(l.to.pin, bp::PinDir::In);
                                         return out == nullptr || in == nullptr ||
                                                bp::CanConnect(out->type, in->type) == bp::Compat::No;
                                     }),
                      g.links.end());
    }
}

} // namespace

BlueprintDocument::BlueprintDocument(bp::Blueprint blueprint, std::filesystem::path path)
    : blueprint_(std::move(blueprint)), path_(std::move(path)) {}

bool BlueprintDocument::Load(const std::filesystem::path& path, std::string* error) {
    bp::Blueprint loaded;
    if (!bp::LoadBlueprint(path, loaded, error)) return false;
    *this = BlueprintDocument(std::move(loaded), path);
    return true;
}

bool BlueprintDocument::Save(std::string* error) {
    if (path_.empty()) return Fail(error, "the Blueprint has no file yet (use Save As)");
    if (!bp::SaveBlueprint(blueprint_, path_, error)) return false;
    dirty_ = false;
    return true;
}

bool BlueprintDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    if (!bp::SaveBlueprint(blueprint_, path, error)) return false;
    path_ = path;
    dirty_ = false;
    return true;
}

std::string BlueprintDocument::Name() const { return path_.empty() ? std::string("Untitled") : path_.stem().string(); }

void BlueprintDocument::Edit(const std::string& label, const std::function<void(bp::Blueprint&)>& change,
                             const std::string& merge_key) {
    const bool merge = !merge_key.empty() && !undo_.empty() && undo_.back().merge_key == merge_key && redo_.empty();
    if (!merge) {
        undo_.push_back({label, bp::BlueprintToJson(blueprint_), merge_key});
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
    redo_.clear();
    change(blueprint_);
    dirty_ = true;
    ++revision_;
}

void BlueprintDocument::Restore(const json& state) {
    bp::Blueprint restored;
    if (bp::BlueprintFromJson(state, restored)) blueprint_ = std::move(restored);
    dirty_ = true;
    ++revision_;
}

bool BlueprintDocument::Undo() {
    if (undo_.empty()) return false;
    Snapshot snap = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({snap.label, bp::BlueprintToJson(blueprint_), {}});
    Restore(snap.state);
    return true;
}

void BlueprintDocument::CancelEdit() {
    if (undo_.empty()) return;
    Snapshot snap = std::move(undo_.back());
    undo_.pop_back();
    Restore(snap.state);
}

bool BlueprintDocument::Redo() {
    if (redo_.empty()) return false;
    Snapshot snap = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({snap.label, bp::BlueprintToJson(blueprint_), {}});
    Restore(snap.state);
    return true;
}

const bp::CompileResult& BlueprintDocument::Compile() {
    compiled_ = bp::CompileBlueprint(blueprint_);
    compiled_once_ = true;
    compiled_revision_ = revision_;
    return compiled_;
}

BlueprintDocument::Status BlueprintDocument::CompileStatus() const {
    if (!compiled_once_) return Status::NotCompiled;
    if (compiled_revision_ != revision_) return Status::Stale;
    if (!compiled_.Ok()) return Status::Errors;
    return compiled_.diagnostics.warnings > 0 ? Status::Warnings : Status::UpToDate;
}

bool BlueprintDocument::IsValidName(const std::string& name) {
    if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

std::string BlueprintDocument::FreeName(const std::string& base, const std::function<bool(const std::string&)>& taken) const {
    if (!taken(base)) return base;
    for (int i = 1;; ++i) {
        const std::string name = base + "_" + std::to_string(i);
        if (!taken(name)) return name;
    }
}

// --- Variables -------------------------------------------------------------------

std::string BlueprintDocument::AddVariable(const bp::PinType& type) {
    const std::string name = FreeName("NewVar", [&](const std::string& n) { return blueprint_.FindVariable(n) != nullptr; });
    Edit("Add variable " + name, [&](bp::Blueprint& b) {
        bp::Variable v;
        v.name = name;
        v.type = type;
        v.default_value = bp::DefaultValue(type);
        b.variables.push_back(std::move(v));
    });
    return name;
}

bool BlueprintDocument::RenameVariable(const std::string& from, const std::string& to, std::string* error) {
    if (from == to) return true;
    if (blueprint_.FindVariable(from) == nullptr) return Fail(error, "no variable named '" + from + "'");
    if (!IsValidName(to)) return Fail(error, "'" + to + "' isn't a valid name (letters, digits and _)");
    if (blueprint_.FindVariable(to) != nullptr) return Fail(error, "a variable named '" + to + "' already exists");
    Edit("Rename variable " + from, [&](bp::Blueprint& b) {
        for (bp::Variable& v : b.variables) {
            if (v.name == from) v.name = to;
        }
        RenameTypes(b, {"Var.Get:", "Var.Set:"}, from, to);
    });
    return true;
}

bool BlueprintDocument::RemoveVariable(const std::string& name) {
    if (blueprint_.FindVariable(name) == nullptr) return false;
    Edit("Remove variable " + name, [&](bp::Blueprint& b) {
        b.variables.erase(std::remove_if(b.variables.begin(), b.variables.end(),
                                         [&](const bp::Variable& v) { return v.name == name; }),
                          b.variables.end());
        RemoveTypes(b, {"Var.Get:" + name, "Var.Set:" + name});
    });
    return true;
}

bool BlueprintDocument::SetVariableType(const std::string& name, const bp::PinType& type) {
    const bp::Variable* v = blueprint_.FindVariable(name);
    if (v == nullptr || type.IsExec()) return false;
    if (v->type == type) return true;
    Edit("Change type of " + name, [&](bp::Blueprint& b) {
        for (bp::Variable& var : b.variables) {
            if (var.name != name) continue;
            var.type = type;
            var.default_value = bp::DefaultValue(type);
        }
        DropBrokenLinks(b);
    });
    return true;
}

// --- Functions and macros ------------------------------------------------------------

std::string BlueprintDocument::AddFunction() {
    const std::string name = FreeName("NewFunction", [&](const std::string& n) { return blueprint_.FindGraph(n) != nullptr; });
    Edit("Add function " + name, [&](bp::Blueprint& b) {
        Graph g;
        g.name = name;
        g.kind = GraphKind::Function;
        bp::GraphBuilder builder(g);
        const NodeId entry = builder.Add("Function.Entry", json::object(), 0, 0);
        const NodeId ret = builder.Add("Function.Return", json::object(), 300, 0);
        builder.Connect(entry, "then", ret, "exec");
        b.graphs.push_back(std::move(g));
    });
    return name;
}

std::string BlueprintDocument::AddMacro() {
    const std::string name = FreeName("NewMacro", [&](const std::string& n) { return blueprint_.FindGraph(n) != nullptr; });
    Edit("Add macro " + name, [&](bp::Blueprint& b) {
        Graph g;
        g.name = name;
        g.kind = GraphKind::Macro;
        bp::Variable in, out;
        in.name = "In";
        in.type = bp::PinType::Exec();
        out.name = "Out";
        out.type = bp::PinType::Exec();
        g.inputs = {in};
        g.outputs = {out};
        bp::GraphBuilder builder(g);
        const NodeId inputs = builder.Add("Macro.Inputs", json::object(), 0, 0);
        const NodeId outputs = builder.Add("Macro.Outputs", json::object(), 300, 0);
        builder.Connect(inputs, "In", outputs, "Out");
        b.graphs.push_back(std::move(g));
    });
    return name;
}

bool BlueprintDocument::RenameGraph(const std::string& from, const std::string& to, std::string* error) {
    if (from == to) return true;
    const Graph* g = blueprint_.FindGraph(from);
    if (g == nullptr) return Fail(error, "no graph named '" + from + "'");
    if (g->kind == GraphKind::EventGraph) return Fail(error, "the Event Graph can't be renamed");
    if (!IsValidName(to)) return Fail(error, "'" + to + "' isn't a valid name (letters, digits and _)");
    if (blueprint_.FindGraph(to) != nullptr) return Fail(error, "a graph named '" + to + "' already exists");
    Edit("Rename " + from, [&](bp::Blueprint& b) {
        b.FindGraph(from)->name = to;
        RenameTypes(b, {"Call.Self:", "Macro:"}, from, to);
        for (Graph& graph : b.graphs) {
            for (Node& n : graph.nodes) {
                if (n.config.is_object() && n.config.value("by", "") == from) n.config["by"] = to; // Sort, Filter
            }
        }
    });
    return true;
}

bool BlueprintDocument::RemoveGraph(const std::string& name, std::string* error) {
    const Graph* g = blueprint_.FindGraph(name);
    if (g == nullptr) return Fail(error, "no graph named '" + name + "'");
    if (g->kind == GraphKind::EventGraph) return Fail(error, "the Event Graph can't be removed");
    Edit("Remove " + name, [&](bp::Blueprint& b) {
        b.graphs.erase(std::remove_if(b.graphs.begin(), b.graphs.end(), [&](const Graph& x) { return x.name == name; }),
                       b.graphs.end());
        RemoveTypes(b, {"Call.Self:" + name, "Macro:" + name});
    });
    return true;
}

// --- Dispatchers ---------------------------------------------------------------------

namespace {
const std::vector<std::string> kDispatcherPrefixes = {"Dispatch.Call:", "Dispatch.Bind:", "Dispatch.Unbind:",
                                                      "Dispatch.UnbindAll:"};
}

std::string BlueprintDocument::AddDispatcher() {
    const std::string name =
        FreeName("NewEventDispatcher", [&](const std::string& n) { return blueprint_.FindDispatcher(n) != nullptr; });
    Edit("Add event dispatcher " + name, [&](bp::Blueprint& b) { b.dispatchers.push_back({name, {}}); });
    return name;
}

bool BlueprintDocument::RenameDispatcher(const std::string& from, const std::string& to, std::string* error) {
    if (from == to) return true;
    if (blueprint_.FindDispatcher(from) == nullptr) return Fail(error, "no event dispatcher named '" + from + "'");
    if (!IsValidName(to)) return Fail(error, "'" + to + "' isn't a valid name (letters, digits and _)");
    if (blueprint_.FindDispatcher(to) != nullptr) return Fail(error, "an event dispatcher named '" + to + "' already exists");
    Edit("Rename " + from, [&](bp::Blueprint& b) {
        for (bp::Dispatcher& d : b.dispatchers) {
            if (d.name == from) d.name = to;
        }
        RenameTypes(b, kDispatcherPrefixes, from, to);
    });
    return true;
}

bool BlueprintDocument::RemoveDispatcher(const std::string& name) {
    if (blueprint_.FindDispatcher(name) == nullptr) return false;
    Edit("Remove " + name, [&](bp::Blueprint& b) {
        b.dispatchers.erase(std::remove_if(b.dispatchers.begin(), b.dispatchers.end(),
                                           [&](const bp::Dispatcher& d) { return d.name == name; }),
                            b.dispatchers.end());
        // Only this Blueprint's own Call nodes go: Bind/Unbind address a
        // dispatcher by name on another entity, which may still have it.
        RemoveTypes(b, {"Dispatch.Call:" + name});
    });
    return true;
}

// --- Clipboard and comments -----------------------------------------------------------

std::string BlueprintDocument::CopyNodes(const std::string& graph, const std::vector<NodeId>& nodes) const {
    const Graph* g = blueprint_.FindGraph(graph);
    if (g == nullptr) return {};
    const std::set<NodeId> chosen(nodes.begin(), nodes.end());
    float x0 = 1e30f, y0 = 1e30f;
    for (const Node& n : g->nodes) {
        if (chosen.count(n.id) == 0) continue;
        x0 = std::min(x0, n.x);
        y0 = std::min(y0, n.y);
    }
    json out_nodes = json::array(), out_links = json::array();
    for (const Node& n : g->nodes) {
        if (chosen.count(n.id) == 0) continue;
        json node = {{"id", n.id}, {"type", n.type}, {"pos", json::array({n.x - x0, n.y - y0})}};
        if (!n.config.empty()) node["config"] = n.config;
        if (!n.defaults.empty()) node["defaults"] = n.defaults;
        if (!n.comment.empty()) node["comment"] = n.comment;
        out_nodes.push_back(std::move(node));
    }
    if (out_nodes.empty()) return {};
    for (const bp::Link& l : g->links) {
        if (chosen.count(l.from.node) && chosen.count(l.to.node)) {
            out_links.push_back({{"from", json::array({l.from.node, l.from.pin})}, {"to", json::array({l.to.node, l.to.pin})}});
        }
    }
    return json{{"aether.nodes", 1}, {"nodes", out_nodes}, {"links", out_links}}.dump();
}

std::vector<NodeId> BlueprintDocument::PasteNodes(const std::string& graph, const std::string& text, float x, float y) {
    if (blueprint_.FindGraph(graph) == nullptr) return {};
    const json clip = json::parse(text, nullptr, false);
    if (!clip.is_object() || !clip.contains("aether.nodes") || !clip.value("nodes", json()).is_array()) return {};
    std::vector<NodeId> added;
    Edit("Paste", [&](bp::Blueprint& b) {
        Graph& g = *b.FindGraph(graph);
        std::map<NodeId, NodeId> ids;
        NodeId next = g.NextId();
        for (const json& n : clip["nodes"]) {
            if (!n.is_object() || !n.value("id", json()).is_number_unsigned() || !n.value("type", json()).is_string()) continue;
            Node node;
            node.id = next++;
            node.type = n["type"].get<std::string>();
            const json pos = n.value("pos", json::array({0.0, 0.0}));
            if (pos.is_array() && pos.size() == 2 && pos[0].is_number() && pos[1].is_number()) {
                node.x = x + pos[0].get<float>();
                node.y = y + pos[1].get<float>();
            }
            node.config = n.value("config", json::object());
            node.defaults = n.value("defaults", json::object());
            node.comment = n.value("comment", "");
            ids[n["id"].get<NodeId>()] = node.id;
            added.push_back(node.id);
            g.nodes.push_back(std::move(node));
        }
        for (const json& l : clip.value("links", json::array())) {
            const json from = l.value("from", json()), to = l.value("to", json());
            if (!from.is_array() || !to.is_array() || from.size() != 2 || to.size() != 2) continue;
            if (!from[0].is_number_unsigned() || !to[0].is_number_unsigned() || !from[1].is_string() || !to[1].is_string()) continue;
            auto a = ids.find(from[0].get<NodeId>()), c = ids.find(to[0].get<NodeId>());
            if (a == ids.end() || c == ids.end()) continue;
            g.links.push_back({{a->second, from[1].get<std::string>()}, {c->second, to[1].get<std::string>()}});
        }
    });
    if (added.empty()) CancelEdit(); // nothing pasted: don't leave an empty undo step
    return added;
}

std::vector<NodeId> BlueprintDocument::DuplicateNodes(const std::string& graph, const std::vector<NodeId>& nodes) {
    const Graph* g = blueprint_.FindGraph(graph);
    if (g == nullptr) return {};
    float x0 = 1e30f, y0 = 1e30f;
    for (NodeId id : nodes) {
        if (const Node* n = g->Find(id)) {
            x0 = std::min(x0, n->x);
            y0 = std::min(y0, n->y);
        }
    }
    const std::string clip = CopyNodes(graph, nodes);
    if (clip.empty()) return {};
    return PasteNodes(graph, clip, x0 + 30.0f, y0 + 30.0f);
}

bool BlueprintDocument::AddComment(const std::string& graph, float x0, float y0, float x1, float y1, const std::string& text) {
    if (blueprint_.FindGraph(graph) == nullptr || x0 > x1 || y0 > y1) return false;
    constexpr float margin = 20.0f, title = 30.0f;
    Edit("Add comment", [&](bp::Blueprint& b) {
        bp::CommentBox box;
        box.text = text;
        box.x = x0 - margin;
        box.y = y0 - margin - title;
        box.width = x1 - x0 + 2 * margin;
        box.height = y1 - y0 + 2 * margin + title;
        b.FindGraph(graph)->comments.push_back(std::move(box));
    });
    return true;
}

} // namespace aether::editor
