#include "graph/material_document.h"

#include <algorithm>
#include <map>
#include <set>

namespace aether::editor {

using mat::Material;
using mat::NodeId;
using mat::PinType;
using nlohmann::json;

namespace {

constexpr usize kMaxUndo = 200;

bool IsParameterNode(const mat::Node& n, const std::string& name) {
    for (const char* prefix : {"Param.Scalar:", "Param.Vector:", "Param.Texture:"}) {
        if (n.type == prefix + name) return true;
    }
    return false;
}

} // namespace

MaterialDocument::MaterialDocument(Material material, std::filesystem::path path)
    : material_(std::move(material)), path_(std::move(path)) {}

bool MaterialDocument::Load(const std::filesystem::path& path, std::string* error) {
    Material loaded;
    if (!mat::LoadMaterial(path, loaded, error)) return false;
    loaded.functions = material_.functions;
    material_ = std::move(loaded);
    path_ = path;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    Changed();
    return true;
}

bool MaterialDocument::Save(std::string* error) {
    if (path_.empty()) {
        if (error != nullptr) *error = "the material has no file yet; use Save As";
        return false;
    }
    if (!mat::SaveMaterial(material_, path_, error)) return false;
    dirty_ = false;
    return true;
}

bool MaterialDocument::SaveAs(const std::filesystem::path& path, std::string* error) {
    if (!mat::SaveMaterial(material_, path, error)) return false;
    path_ = path;
    dirty_ = false;
    return true;
}

std::string MaterialDocument::Name() const { return path_.empty() ? "Untitled" : path_.stem().string(); }

void MaterialDocument::SetFunctions(std::shared_ptr<const mat::FunctionLibrary> functions) {
    material_.functions = std::move(functions);
    Changed();
}

void MaterialDocument::Changed() { ++revision_; }

void MaterialDocument::Edit(const std::string& label, const std::function<void(Material&)>& change, const std::string& merge_key) {
    const bool merge = !merge_key.empty() && !undo_.empty() && undo_.back().merge_key == merge_key && redo_.empty();
    if (!merge) {
        undo_.push_back({label, mat::MaterialToJson(material_), merge_key});
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    }
    redo_.clear();
    change(material_);
    dirty_ = true;
    Changed();
}

void MaterialDocument::Restore(const json& state) {
    Material restored;
    mat::MaterialFromJson(state, restored);
    restored.functions = material_.functions;
    material_ = std::move(restored);
    dirty_ = true;
    Changed();
}

bool MaterialDocument::Undo() {
    if (undo_.empty()) return false;
    Snapshot snap = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back({snap.label, mat::MaterialToJson(material_), {}});
    Restore(snap.state);
    return true;
}

void MaterialDocument::CancelEdit() {
    if (undo_.empty()) return;
    Snapshot snap = std::move(undo_.back());
    undo_.pop_back();
    Restore(snap.state);
}

bool MaterialDocument::Redo() {
    if (redo_.empty()) return false;
    Snapshot snap = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back({snap.label, mat::MaterialToJson(material_), {}});
    Restore(snap.state);
    return true;
}

const mat::GeneratedMaterial& MaterialDocument::Generated() const {
    if (generated_revision_ != revision_) {
        generated_ = mat::GenerateHlsl(material_);
        generated_revision_ = revision_;
    }
    return generated_;
}

// --- Parameters ---------------------------------------------------------------------------

bool MaterialDocument::IsValidName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7F; });
}

std::string MaterialDocument::ParameterNodeType(const mat::Parameter& p) {
    const char* prefix = p.type == PinType::Texture ? "Param.Texture:" : p.type == PinType::Float ? "Param.Scalar:" : "Param.Vector:";
    return prefix + p.name;
}

std::string MaterialDocument::AddParameter(PinType type) {
    std::string name = "Param";
    for (int n = 1; material_.FindParameter(name) != nullptr; ++n) name = "Param_" + std::to_string(n);
    Edit("Add parameter", [&](Material& m) {
        mat::Parameter p;
        p.name = name;
        p.type = type;
        if (type == PinType::Float4 || type == PinType::Float3) p.default_value = Vec4(1, 1, 1, 1);
        m.parameters.push_back(p);
    });
    return name;
}

bool MaterialDocument::RenameParameter(const std::string& from, const std::string& to, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (material_.FindParameter(from) == nullptr) return fail("no parameter named '" + from + "'");
    if (from == to) return true;
    if (!IsValidName(to)) return fail("'" + to + "' isn't a valid parameter name");
    if (material_.FindParameter(to) != nullptr) return fail("there's already a parameter named '" + to + "'");
    Edit("Rename parameter", [&](Material& m) {
        for (mat::Parameter& p : m.parameters) {
            if (p.name != from) continue;
            for (mat::Node& n : m.nodes) {
                if (IsParameterNode(n, from)) n.type = n.type.substr(0, n.type.find(':') + 1) + to;
            }
            p.name = to;
        }
    });
    return true;
}

bool MaterialDocument::RemoveParameter(const std::string& name) {
    if (material_.FindParameter(name) == nullptr) return false;
    Edit("Remove parameter", [&](Material& m) {
        std::set<NodeId> gone;
        for (const mat::Node& n : m.nodes) {
            if (IsParameterNode(n, name)) gone.insert(n.id);
        }
        m.nodes.erase(std::remove_if(m.nodes.begin(), m.nodes.end(), [&](const mat::Node& n) { return gone.count(n.id) != 0; }), m.nodes.end());
        m.links.erase(std::remove_if(m.links.begin(), m.links.end(),
                                     [&](const mat::Link& l) { return gone.count(l.from.node) || gone.count(l.to.node); }),
                      m.links.end());
        m.parameters.erase(std::remove_if(m.parameters.begin(), m.parameters.end(), [&](const mat::Parameter& p) { return p.name == name; }),
                           m.parameters.end());
    });
    return true;
}

bool MaterialDocument::SetParameterType(const std::string& name, PinType type) {
    const mat::Parameter* current = material_.FindParameter(name);
    if (current == nullptr || type == PinType::None || type == PinType::Any) return false;
    if (current->type == type) return true;
    Edit("Change parameter type", [&](Material& m) {
        std::set<NodeId> nodes;
        for (mat::Parameter& p : m.parameters) {
            if (p.name != name) continue;
            p.type = type;
            p.default_value = type == PinType::Float3 || type == PinType::Float4 ? Vec4(1, 1, 1, 1) : Vec4(0, 0, 0, 0);
            p.texture.clear();
            for (mat::Node& n : m.nodes) {
                if (!IsParameterNode(n, name)) continue;
                n.type = ParameterNodeType(p);
                nodes.insert(n.id);
            }
        }
        // Break the links out of its nodes that no longer fit.
        const mat::Analysis a = mat::Analyze(m);
        std::set<std::pair<NodeId, std::string>> bad;
        for (const mat::Diagnostic& d : a.diagnostics) {
            if (d.code == "MT003") bad.insert({d.node, d.pin});
        }
        m.links.erase(std::remove_if(m.links.begin(), m.links.end(),
                                     [&](const mat::Link& l) { return nodes.count(l.from.node) && bad.count({l.to.node, l.to.pin}); }),
                      m.links.end());
    });
    return true;
}

// --- Nodes -------------------------------------------------------------------------------------

std::string MaterialDocument::CopyNodes(const std::vector<NodeId>& nodes) const {
    std::set<NodeId> chosen;
    for (NodeId id : nodes) {
        const mat::Node* n = material_.Find(id);
        if (n != nullptr && n->type != "Material.Output") chosen.insert(id);
    }
    float x0 = 1e30f, y0 = 1e30f;
    for (const mat::Node& n : material_.nodes) {
        if (chosen.count(n.id) == 0) continue;
        x0 = std::min(x0, n.x);
        y0 = std::min(y0, n.y);
    }
    json out_nodes = json::array(), out_links = json::array();
    for (const mat::Node& n : material_.nodes) {
        if (chosen.count(n.id) == 0) continue;
        json node = {{"id", n.id}, {"type", n.type}, {"pos", json::array({n.x - x0, n.y - y0})}};
        if (!n.config.empty()) node["config"] = n.config;
        if (!n.defaults.empty()) node["defaults"] = n.defaults;
        out_nodes.push_back(std::move(node));
    }
    if (out_nodes.empty()) return {};
    for (const mat::Link& l : material_.links) {
        if (chosen.count(l.from.node) && chosen.count(l.to.node)) {
            out_links.push_back({{"from", json::array({l.from.node, l.from.pin})}, {"to", json::array({l.to.node, l.to.pin})}});
        }
    }
    return json{{"aether.material_nodes", 1}, {"nodes", out_nodes}, {"links", out_links}}.dump();
}

std::vector<NodeId> MaterialDocument::PasteNodes(const std::string& text, float x, float y) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("aether.material_nodes", 0) != 1) return {};
    std::vector<NodeId> added;
    Edit("Paste", [&](Material& m) {
        std::map<NodeId, NodeId> ids;
        NodeId next = m.NextId();
        for (const json& n : j.value("nodes", json::array())) {
            if (!n.is_object() || !n.value("id", json()).is_number_unsigned() || !n.value("type", json()).is_string()) continue;
            if (n["type"] == "Material.Output") continue;
            mat::Node node;
            node.id = next++;
            node.type = n["type"].get<std::string>();
            const json pos = n.value("pos", json::array({0, 0}));
            if (pos.is_array() && pos.size() == 2 && pos[0].is_number() && pos[1].is_number()) {
                node.x = x + pos[0].get<float>();
                node.y = y + pos[1].get<float>();
            }
            node.config = n.value("config", json::object());
            node.defaults = n.value("defaults", json::object());
            ids[n["id"].get<NodeId>()] = node.id;
            added.push_back(node.id);
            m.nodes.push_back(std::move(node));
        }
        for (const json& l : j.value("links", json::array())) {
            const json from = l.value("from", json()), to = l.value("to", json());
            if (!from.is_array() || !to.is_array() || from.size() != 2 || to.size() != 2 || !from[0].is_number_unsigned() ||
                !to[0].is_number_unsigned() || !from[1].is_string() || !to[1].is_string()) {
                continue;
            }
            auto a = ids.find(from[0].get<NodeId>()), b = ids.find(to[0].get<NodeId>());
            if (a != ids.end() && b != ids.end()) m.links.push_back({{a->second, from[1].get<std::string>()}, {b->second, to[1].get<std::string>()}});
        }
    });
    if (added.empty()) CancelEdit();
    return added;
}

std::vector<NodeId> MaterialDocument::DuplicateNodes(const std::vector<NodeId>& nodes) {
    const std::string text = CopyNodes(nodes);
    if (text.empty()) return {};
    float x0 = 1e30f, y0 = 1e30f;
    for (NodeId id : nodes) {
        if (const mat::Node* n = material_.Find(id); n != nullptr && n->type != "Material.Output") {
            x0 = std::min(x0, n->x);
            y0 = std::min(y0, n->y);
        }
    }
    return PasteNodes(text, x0 + 30.0f, y0 + 30.0f);
}

} // namespace aether::editor
