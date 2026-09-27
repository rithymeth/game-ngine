#include "aether/renderer/material.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace aether::mat {

using nlohmann::json;

namespace {

constexpr int kFormatVersion = 1;

PinDesc In(const std::string& name, PinType type, Vec4 def = {0, 0, 0, 0}) { return {name, type, def, false, {}}; }
PinDesc Out(const std::string& name, PinType type) { return {name, type, {0, 0, 0, 0}, false, {}}; }

NodeSignature Sig(const std::string& title, const std::string& category, std::vector<PinDesc> in, std::vector<PinDesc> out,
                  bool generic = false) {
    NodeSignature s;
    s.title = title;
    s.category = category;
    s.inputs = std::move(in);
    s.outputs = std::move(out);
    s.generic = generic;
    return s;
}

const char* ShadingName(ShadingModel m) { return m == ShadingModel::Unlit ? "Unlit" : "DefaultLit"; }
const char* BlendName(BlendMode b) {
    switch (b) {
    case BlendMode::Opaque: return "Opaque";
    case BlendMode::Masked: return "Masked";
    case BlendMode::Translucent: return "Translucent";
    case BlendMode::Additive: return "Additive";
    }
    return "Opaque";
}

json VecJson(const Vec4& v, u32 components) {
    json a = json::array();
    const f32 c[4] = {v.x, v.y, v.z, v.w};
    for (u32 i = 0; i < std::max(components, 1u); ++i) a.push_back(c[i]);
    return a;
}

bool VecFrom(const json& j, Vec4& out) {
    if (j.is_number()) {
        const f32 f = j.get<f32>();
        out = Vec4(f, f, f, f);
        return true;
    }
    if (!j.is_array() || j.empty() || j.size() > 4) return false;
    f32 c[4] = {0, 0, 0, 0};
    for (usize i = 0; i < j.size(); ++i) {
        if (!j[i].is_number()) return false;
        c[i] = j[i].get<f32>();
    }
    out = Vec4(c[0], c[1], c[2], c[3]);
    return true;
}

// The node catalog: exact types, and prefixes whose suffix is a parameter name.
using Factory = std::function<std::optional<NodeSignature>(const Material&, const Node&, const std::string& suffix, std::string&)>;
struct Catalog {
    std::map<std::string, Factory> exact;
    std::map<std::string, Factory> families;
};

const Catalog& GetCatalog() {
    static const Catalog catalog = [] {
        Catalog c;
        const PinType F = PinType::Float, F2 = PinType::Float2, F3 = PinType::Float3, F4 = PinType::Float4, A = PinType::Any,
                      T = PinType::Texture;
        auto fixed = [&](const std::string& id, NodeSignature sig) {
            c.exact[id] = [sig](const Material&, const Node&, const std::string&, std::string&) { return std::optional<NodeSignature>(sig); };
        };
        fixed("Material.Output", Sig("Material", "Output",
                                     {In("BaseColor", F3, {0.5f, 0.5f, 0.5f, 0}), In("Metallic", F), In("Roughness", F, {0.5f, 0, 0, 0}),
                                      In("Normal", F3, {0, 0, 1, 0}), In("Emissive", F3), In("AO", F, {1, 0, 0, 0}),
                                      In("Opacity", F, {1, 0, 0, 0}), In("OpacityMask", F, {1, 0, 0, 0}), In("WorldPositionOffset", F3)},
                                     {}));
        // Constants: config {"value": number or array}.
        for (u32 n = 1; n <= 4; ++n) {
            const std::string id = n == 1 ? "Const.Float" : "Const.Float" + std::to_string(n);
            c.exact[id] = [n](const Material&, const Node& node, const std::string&, std::string& error) -> std::optional<NodeSignature> {
                Vec4 v;
                if (node.config.contains("value") && !VecFrom(node.config["value"], v)) {
                    error = "a constant's value must be a number or up to 4 numbers";
                    return std::nullopt;
                }
                return Sig(n == 1 ? "Constant" : "Constant" + std::to_string(n), "Constants", {}, {Out("value", FloatN(n))});
            };
        }
        // Parameters by name: Param.Scalar:Roughness, Param.Vector:Tint, Param.Texture:Albedo.
        auto param = [&](const std::string& prefix, bool texture, bool vector) {
            c.families[prefix] = [=](const Material& m, const Node&, const std::string& name, std::string& error) -> std::optional<NodeSignature> {
                const Parameter* p = m.FindParameter(name);
                if (p == nullptr) {
                    error = "MT006:no parameter named '" + name + "'";
                    return std::nullopt;
                }
                const bool is_texture = p->type == PinType::Texture, is_vector = !is_texture && p->type != PinType::Float;
                if (is_texture != texture || (!texture && is_vector != vector)) {
                    error = "MT006:'" + name + "' is a " + TypeName(p->type) + " parameter";
                    return std::nullopt;
                }
                return Sig(name, "Parameters", {}, {Out("value", p->type)});
            };
        };
        param("Param.Scalar:", false, false);
        param("Param.Vector:", false, true);
        param("Param.Texture:", true, false);

        NodeSignature sample = Sig("Texture Sample", "Texture", {In("texture", T), In("uv", F2)},
                                   {Out("rgba", F4), Out("rgb", F3), Out("r", F), Out("g", F), Out("b", F), Out("a", F)});
        sample.inputs[0].required = true;
        sample.inputs[1].implicit = "uv0";
        fixed("Texture.Sample", sample);
        c.exact["Input.TexCoord"] = [=](const Material&, const Node& node, const std::string&, std::string& error) -> std::optional<NodeSignature> {
            const json index = node.config.value("index", json(0));
            if (!index.is_number_integer() || index.get<i64>() < 0 || index.get<i64>() > 3) {
                error = "TexCoord's index must be 0 to 3";
                return std::nullopt;
            }
            return Sig("TexCoord " + std::to_string(index.get<u32>()), "Coordinates", {}, {Out("uv", F2)});
        };
        fixed("Input.Time", Sig("Time", "Constants", {}, {Out("time", F)}));
        fixed("Input.WorldPosition", Sig("World Position", "Coordinates", {}, {Out("position", F3)}));
        fixed("Input.VertexNormal", Sig("Vertex Normal", "Vectors", {}, {Out("normal", F3)}));
        fixed("Input.CameraVector", Sig("Camera Vector", "Vectors", {}, {Out("direction", F3)}));

        // Generic math.
        for (const char* op : {"Add", "Subtract", "Multiply", "Divide", "Min", "Max", "Power"}) {
            fixed(std::string("Math.") + op,
                  Sig(op, "Math", {In("a", A), In("b", A, std::string(op) == "Multiply" || std::string(op) == "Divide" || std::string(op) == "Power" ? Vec4(1, 1, 1, 1) : Vec4(0, 0, 0, 0))},
                      {Out("result", A)}, true));
        }
        for (const char* op : {"Abs", "Sqrt", "Sin", "Cos", "Frac", "Floor", "Saturate", "OneMinus", "Normalize"}) {
            fixed(std::string("Math.") + op, Sig(op, "Math", {In("x", A)}, {Out("result", A)}, true));
        }
        fixed("Math.Lerp", Sig("Lerp", "Math", {In("a", A), In("b", A, {1, 1, 1, 1}), In("alpha", A, {0.5f, 0.5f, 0.5f, 0.5f})},
                               {Out("result", A)}, true));
        fixed("Math.Clamp", Sig("Clamp", "Math", {In("x", A), In("min", A), In("max", A, {1, 1, 1, 1})}, {Out("result", A)}, true));
        fixed("Math.Dot", Sig("Dot", "Math", {In("a", A), In("b", A)}, {Out("result", F)}, true));
        fixed("Math.Length", Sig("Length", "Math", {In("x", A)}, {Out("result", F)}, true));
        // Append and ComponentMask have types that depend on their inputs; Analyze works them out.
        fixed("Math.Append", Sig("Append", "Math", {In("a", A), In("b", A)}, {Out("result", A)}));
        c.exact["Math.ComponentMask"] = [=](const Material&, const Node& node, const std::string&, std::string& error) -> std::optional<NodeSignature> {
            const std::string channels = node.config.value("channels", "r");
            if (channels.empty() || channels.size() > 4 || channels.find_first_not_of("rgba") != std::string::npos) {
                error = "ComponentMask's channels must be 1 to 4 of r, g, b, a";
                return std::nullopt;
            }
            return Sig("Mask (" + channels + ")", "Math", {In("x", A)}, {Out("result", FloatN(static_cast<u32>(channels.size())))});
        };
        NodeSignature fresnel = Sig("Fresnel", "Shading", {In("normal", F3), In("exponent", F, {5, 0, 0, 0})}, {Out("result", F)});
        fresnel.inputs[0].implicit = "normal";
        fixed("Shading.Fresnel", fresnel);
        NodeSignature panner = Sig("Panner", "Coordinates", {In("uv", F2), In("speed", F2, {0.1f, 0, 0, 0}), In("time", F)}, {Out("uv", F2)});
        panner.inputs[0].implicit = "uv0";
        panner.inputs[2].implicit = "time";
        fixed("Coords.Panner", panner);
        fixed("Texture.NormalUnpack", Sig("Unpack Normal", "Texture", {In("packed", F4, {0.5f, 0.5f, 1, 1})}, {Out("normal", F3)}));
        return c;
    }();
    return catalog;
}

} // namespace

// --- Types --------------------------------------------------------------------------------

const char* TypeName(PinType t) {
    switch (t) {
    case PinType::None: return "none";
    case PinType::Float: return "float";
    case PinType::Float2: return "float2";
    case PinType::Float3: return "float3";
    case PinType::Float4: return "float4";
    case PinType::Texture: return "texture";
    case PinType::Any: return "any";
    }
    return "?";
}

std::optional<PinType> ParseType(const std::string& name) {
    for (PinType t : {PinType::Float, PinType::Float2, PinType::Float3, PinType::Float4, PinType::Texture}) {
        if (name == TypeName(t)) return t;
    }
    return std::nullopt;
}

u32 Components(PinType t) {
    switch (t) {
    case PinType::Float: return 1;
    case PinType::Float2: return 2;
    case PinType::Float3: return 3;
    case PinType::Float4: return 4;
    default: return 0;
    }
}

PinType FloatN(u32 n) {
    switch (n) {
    case 1: return PinType::Float;
    case 2: return PinType::Float2;
    case 3: return PinType::Float3;
    case 4: return PinType::Float4;
    default: return PinType::None;
    }
}

Fit CanConnect(PinType from, PinType to) {
    if (from == PinType::Texture || to == PinType::Texture) return from == to ? Fit::Yes : Fit::No;
    const u32 a = Components(from), b = Components(to);
    if (a == 0 || b == 0) return Fit::No;
    if (a == b) return Fit::Yes;
    if (a == 1) return Fit::Broadcast;
    if (a > b) return Fit::Truncate;
    return Fit::No;
}

// --- Material -------------------------------------------------------------------------------

const Parameter* Material::FindParameter(const std::string& name) const {
    for (const Parameter& p : parameters) {
        if (p.name == name) return &p;
    }
    return nullptr;
}
const Node* Material::Find(NodeId id) const {
    for (const Node& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}
Node* Material::Find(NodeId id) { return const_cast<Node*>(static_cast<const Material*>(this)->Find(id)); }
NodeId Material::NextId() const {
    NodeId next = 1;
    for (const Node& n : nodes) next = std::max(next, n.id + 1);
    return next;
}
NodeId Material::Add(const std::string& type, json config, f32 x, f32 y) {
    Node n;
    n.id = NextId();
    n.type = type;
    n.config = std::move(config);
    n.x = x;
    n.y = y;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}
Material& Material::Connect(NodeId from, const std::string& from_pin, NodeId to, const std::string& to_pin) {
    links.push_back({{from, from_pin}, {to, to_pin}});
    return *this;
}

const PinDesc* NodeSignature::Input(const std::string& name) const {
    for (const PinDesc& p : inputs) {
        if (p.name == name) return &p;
    }
    return nullptr;
}
const PinDesc* NodeSignature::Output(const std::string& name) const {
    for (const PinDesc& p : outputs) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

std::optional<NodeSignature> ResolveNode(const Material& m, const Node& node, std::string* error) {
    const Catalog& c = GetCatalog();
    std::string message;
    std::optional<NodeSignature> sig;
    if (auto it = c.exact.find(node.type); it != c.exact.end()) {
        sig = it->second(m, node, {}, message);
    } else {
        bool found = false;
        for (const auto& [prefix, factory] : c.families) {
            if (node.type.rfind(prefix, 0) == 0) {
                sig = factory(m, node, node.type.substr(prefix.size()), message);
                found = true;
                break;
            }
        }
        if (!found) message = "unknown node type '" + node.type + "'";
    }
    if (!sig && error != nullptr) *error = message;
    return sig;
}

std::vector<PaletteEntry> ListNodeTypes(const Material& m) {
    std::vector<PaletteEntry> out;
    for (const auto& [id, factory] : GetCatalog().exact) {
        Node probe;
        probe.type = id;
        std::string error;
        if (auto sig = factory(m, probe, {}, error)) out.push_back({id, sig->title, sig->category});
    }
    for (const Parameter& p : m.parameters) {
        const std::string prefix = p.type == PinType::Texture ? "Param.Texture:" : p.type == PinType::Float ? "Param.Scalar:" : "Param.Vector:";
        out.push_back({prefix + p.name, p.name, "Parameters"});
    }
    return out;
}

// --- Analysis ---------------------------------------------------------------------------------

bool Analysis::Has(const std::string& code) const {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d) { return d.code == code; });
}

PinType Analysis::TypeOf(const PinRef& pin) const {
    for (const auto& [ref, type] : types) {
        if (ref == pin) return type;
    }
    return PinType::None;
}

Analysis Analyze(const Material& m) {
    Analysis a;
    auto report = [&](const std::string& code, NodeId node, const std::string& pin, const std::string& message,
                      Severity severity = Severity::Error) {
        a.diagnostics.push_back({code, severity, node, pin, message});
        if (severity == Severity::Error) ++a.errors;
    };

    std::map<NodeId, NodeSignature> sigs;
    usize outputs = 0;
    std::set<NodeId> seen;
    for (const Node& n : m.nodes) {
        if (!seen.insert(n.id).second || n.id == 0) {
            report("MT002", n.id, "", "Two nodes share the id " + std::to_string(n.id) + ".");
            continue;
        }
        std::string error;
        if (auto sig = ResolveNode(m, n, &error)) {
            sigs.emplace(n.id, std::move(*sig));
        } else if (error.rfind("MT006:", 0) == 0) {
            report("MT006", n.id, "", error.substr(6) + ".");
        } else {
            report("MT002", n.id, "", error + ".");
        }
        if (n.type == "Material.Output") {
            ++outputs;
            a.output = n.id;
        }
    }
    if (outputs == 0) report("MT001", 0, "", "The material has no Material Output node.");
    if (outputs > 1) report("MT001", 0, "", "The material has more than one Material Output node.");

    // Links: pins exist, one link per input.
    std::map<std::pair<NodeId, std::string>, const Link*> incoming;
    for (const Link& l : m.links) {
        auto from = sigs.find(l.from.node), to = sigs.find(l.to.node);
        if (from == sigs.end() || to == sigs.end()) {
            if (m.Find(l.from.node) == nullptr || m.Find(l.to.node) == nullptr) report("MT004", l.to.node, l.to.pin, "A link points at a node that doesn't exist.");
            continue;
        }
        if (from->second.Output(l.from.pin) == nullptr) {
            report("MT004", l.from.node, l.from.pin, "'" + from->second.title + "' has no output '" + l.from.pin + "'.");
            continue;
        }
        if (to->second.Input(l.to.pin) == nullptr) {
            report("MT004", l.to.node, l.to.pin, "'" + to->second.title + "' has no input '" + l.to.pin + "'.");
            continue;
        }
        if (!incoming.emplace(std::make_pair(l.to.node, l.to.pin), &l).second) {
            report("MT008", l.to.node, l.to.pin, "The input '" + l.to.pin + "' has more than one link.");
        }
    }

    // Types, by walking back through the inputs (memoized; a cycle is reported once).
    std::map<NodeId, PinType> generic_type; // a generic node's resolved Any type
    std::map<NodeId, int> state;            // 0 unvisited, 1 visiting, 2 done
    std::map<std::pair<NodeId, std::string>, PinType> out_types;
    bool cycle = false;
    std::function<void(NodeId)> visit = [&](NodeId id) {
        auto sig_it = sigs.find(id);
        if (sig_it == sigs.end() || state[id] == 2) return;
        if (state[id] == 1) {
            if (!cycle) report("MT005", id, "", "The graph loops back on itself here; material graphs can't have cycles.");
            cycle = true;
            return;
        }
        state[id] = 1;
        const NodeSignature& sig = sig_it->second;
        std::map<std::string, PinType> in_types;
        for (const PinDesc& p : sig.inputs) {
            auto link = incoming.find({id, p.name});
            if (link == incoming.end()) continue;
            visit(link->second->from.node);
            in_types[p.name] = out_types.count({link->second->from.node, link->second->from.pin})
                                   ? out_types[{link->second->from.node, link->second->from.pin}]
                                   : PinType::None;
        }
        const Node& node = *m.Find(id);
        if (node.type == "Math.Append") {
            u32 total = 0;
            for (const char* pin : {"a", "b"}) {
                auto t = in_types.find(pin);
                total += t == in_types.end() ? 1 : std::max(Components(t->second), 1u);
            }
            if (total > 4) report("MT003", id, "", "Append would make " + std::to_string(total) + " components; at most 4.");
            out_types[{id, "result"}] = FloatN(std::min(total, 4u));
        } else {
            // Generic nodes: the widest connected Any input; mismatched widths are an error.
            u32 width = 1;
            if (sig.generic || node.type == "Math.ComponentMask") {
                for (const PinDesc& p : sig.inputs) {
                    auto t = in_types.find(p.name);
                    if (p.type != PinType::Any || t == in_types.end()) continue;
                    const u32 c = Components(t->second);
                    if (c == 0) {
                        if (t->second != PinType::None) report("MT003", id, p.name, "A math input can't take a texture; sample it first.");
                        continue;
                    }
                    if (c > 1 && width > 1 && c != width) {
                        report("MT003", id, p.name,
                               std::string("'") + sig.title + "' mixes " + TypeName(FloatN(width)) + " and " + TypeName(t->second) +
                                   " (only a float mixes with any size).");
                    }
                    width = std::max(width, c);
                }
                generic_type[id] = FloatN(width);
            }
            if (node.type == "Math.ComponentMask") {
                const std::string channels = node.config.value("channels", "r");
                const auto in = in_types.find("x");
                const u32 have = in == in_types.end() ? 1 : Components(in->second);
                for (char ch : channels) {
                    const u32 index = ch == 'r' ? 0 : ch == 'g' ? 1 : ch == 'b' ? 2 : 3;
                    if (index >= have) report("MT003", id, "x", std::string("The mask reads '") + ch + "' from a " + TypeName(FloatN(have)) + ".");
                }
            }
            for (const PinDesc& p : sig.outputs) out_types[{id, p.name}] = p.type == PinType::Any ? FloatN(width) : p.type;
        }
        state[id] = 2;
    };
    for (const Node& n : m.nodes) visit(n.id);

    // Each link's fit, and required inputs.
    for (const auto& [key, link] : incoming) {
        const NodeSignature& to_sig = sigs.at(key.first);
        const PinDesc* in = to_sig.Input(key.second);
        PinType to = in->type;
        if (to == PinType::Any) to = generic_type.count(key.first) ? generic_type[key.first] : PinType::Float;
        const PinType from = out_types.count({link->from.node, link->from.pin}) ? out_types[{link->from.node, link->from.pin}] : PinType::None;
        if (from == PinType::None) continue; // its source already reported
        if (m.Find(key.first)->type == "Math.Append" || m.Find(key.first)->type == "Math.ComponentMask") {
            if (from == PinType::Texture) report("MT003", key.first, key.second, "A math input can't take a texture; sample it first.");
            continue;
        }
        const Fit fit = CanConnect(from, to);
        if (fit == Fit::No) {
            report("MT003", key.first, key.second, std::string("A ") + TypeName(from) + " can't connect to the " + TypeName(to) + " input '" + key.second + "'.");
        } else if (fit == Fit::Truncate) {
            report("MT010", key.first, key.second,
                   std::string("A ") + TypeName(from) + " feeds the " + TypeName(to) + " input '" + key.second + "'; the extra components are dropped.",
                   Severity::Warning);
        }
    }
    for (const auto& [id, sig] : sigs) {
        for (const PinDesc& p : sig.inputs) {
            if (p.required && !incoming.count({id, p.name})) report("MT007", id, p.name, "'" + sig.title + "' needs its '" + p.name + "' input connected.");
        }
    }
    // Pins the material's settings ignore.
    if (a.output != 0) {
        auto used = [&](const char* pin) { return incoming.count({a.output, pin}) != 0; };
        if (used("Opacity") && (m.blend == BlendMode::Opaque || m.blend == BlendMode::Masked)) {
            report("MT009", a.output, "Opacity", "Opacity only matters for Translucent and Additive materials.", Severity::Warning);
        }
        if (used("OpacityMask") && m.blend != BlendMode::Masked) {
            report("MT009", a.output, "OpacityMask", "Opacity Mask only matters for Masked materials.", Severity::Warning);
        }
        if (m.shading == ShadingModel::Unlit) {
            for (const char* pin : {"Metallic", "Roughness", "Normal", "AO"}) {
                if (used(pin)) report("MT009", a.output, pin, std::string(pin) + " is ignored by Unlit materials.", Severity::Warning);
            }
        }
    }
    for (const auto& [ref, type] : out_types) a.types.push_back({{ref.first, ref.second}, type});
    return a;
}

// --- Files ------------------------------------------------------------------------------------------

json MaterialToJson(const Material& m) {
    json params = json::array();
    for (const Parameter& p : m.parameters) {
        json j = {{"name", p.name}, {"type", TypeName(p.type)}};
        if (p.type == PinType::Texture) {
            j["texture"] = p.texture;
        } else {
            j["default"] = VecJson(p.default_value, Components(p.type));
        }
        if (!p.group.empty()) j["group"] = p.group;
        params.push_back(std::move(j));
    }
    json nodes = json::array();
    for (const Node& n : m.nodes) {
        json j = {{"id", n.id}, {"type", n.type}, {"pos", json::array({n.x, n.y})}};
        if (!n.config.empty()) j["config"] = n.config;
        if (!n.defaults.empty()) j["defaults"] = n.defaults;
        nodes.push_back(std::move(j));
    }
    json links = json::array();
    for (const Link& l : m.links) links.push_back({{"from", json::array({l.from.node, l.from.pin})}, {"to", json::array({l.to.node, l.to.pin})}});
    return {{"$type", "Material"}, {"$version", kFormatVersion}, {"shading", ShadingName(m.shading)}, {"blend", BlendName(m.blend)},
            {"two_sided", m.two_sided}, {"opacity_mask_clip", m.opacity_mask_clip}, {"parameters", params}, {"nodes", nodes}, {"links", links}};
}

bool MaterialFromJson(const json& j, Material& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!j.is_object() || j.value("$type", "") != "Material") return fail("not a material file");
    if (j.value("$version", 0) > kFormatVersion) return fail("saved by a newer version of the engine");
    Material m;
    const std::string shading = j.value("shading", "DefaultLit");
    if (shading == "Unlit") m.shading = ShadingModel::Unlit;
    else if (shading != "DefaultLit") return fail("unknown shading model '" + shading + "'");
    const std::string blend = j.value("blend", "Opaque");
    bool known = false;
    for (BlendMode b : {BlendMode::Opaque, BlendMode::Masked, BlendMode::Translucent, BlendMode::Additive}) {
        if (blend == BlendName(b)) m.blend = b, known = true;
    }
    if (!known) return fail("unknown blend mode '" + blend + "'");
    m.two_sided = j.value("two_sided", false);
    m.opacity_mask_clip = j.value("opacity_mask_clip", m.opacity_mask_clip);
    for (const json& p : j.value("parameters", json::array())) {
        Parameter param;
        param.name = p.value("name", "");
        const auto type = ParseType(p.value("type", ""));
        if (param.name.empty() || !type) return fail("a parameter needs a name and a type");
        if (m.FindParameter(param.name) != nullptr) return fail("two parameters are named '" + param.name + "'");
        param.type = *type;
        param.texture = p.value("texture", "");
        param.group = p.value("group", "");
        if (p.contains("default") && !VecFrom(p["default"], param.default_value)) return fail("parameter '" + param.name + "' has a bad default");
        m.parameters.push_back(std::move(param));
    }
    for (const json& n : j.value("nodes", json::array())) {
        if (!n.is_object() || !n.value("id", json()).is_number_unsigned() || !n.value("type", json()).is_string()) {
            return fail("a node needs an id and a type");
        }
        Node node;
        node.id = n["id"].get<NodeId>();
        node.type = n["type"].get<std::string>();
        const json pos = n.value("pos", json::array({0, 0}));
        if (pos.is_array() && pos.size() == 2 && pos[0].is_number() && pos[1].is_number()) {
            node.x = pos[0].get<f32>();
            node.y = pos[1].get<f32>();
        }
        node.config = n.value("config", json::object());
        node.defaults = n.value("defaults", json::object());
        if (m.Find(node.id) != nullptr) return fail("two nodes have the id " + std::to_string(node.id));
        m.nodes.push_back(std::move(node));
    }
    for (const json& l : j.value("links", json::array())) {
        const json from = l.value("from", json()), to = l.value("to", json());
        if (!from.is_array() || !to.is_array() || from.size() != 2 || to.size() != 2 || !from[0].is_number_unsigned() ||
            !to[0].is_number_unsigned() || !from[1].is_string() || !to[1].is_string()) {
            return fail("a malformed link");
        }
        m.links.push_back({{from[0].get<NodeId>(), from[1].get<std::string>()}, {to[0].get<NodeId>(), to[1].get<std::string>()}});
    }
    out = std::move(m);
    return true;
}

bool SaveMaterial(const Material& m, const std::filesystem::path& path, std::string* error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) *error = "can't write " + path.string();
        return false;
    }
    file << MaterialToJson(m).dump(2) << '\n';
    return static_cast<bool>(file);
}

bool LoadMaterial(const std::filesystem::path& path, Material& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error != nullptr) *error = "can't read " + path.string();
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    const json j = json::parse(text.str(), nullptr, false);
    if (j.is_discarded()) {
        if (error != nullptr) *error = path.string() + " isn't valid JSON";
        return false;
    }
    return MaterialFromJson(j, out, error);
}

} // namespace aether::mat
