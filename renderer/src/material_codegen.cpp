#include "aether/renderer/material_codegen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace aether::mat {

using json = nlohmann::json;

namespace {

// Unique HLSL identifiers for the parameters, in declaration order.
std::map<std::string, std::string> ParameterIdentifiers(const Material& m) {
    std::map<std::string, std::string> out;
    std::set<std::string> taken;
    for (const Parameter& p : m.parameters) {
        const std::string base = (p.type == PinType::Texture ? "T_" : "P_") + HlslIdentifier(p.name);
        std::string id = base;
        for (int n = 2; !taken.insert(id).second; ++n) id = base + "_" + std::to_string(n);
        out[p.name] = id;
    }
    return out;
}

std::string Number(f32 v) {
    if (!std::isfinite(v)) v = 0.0f;
    // The shortest text that reads back as the same float.
    char buf[32];
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*g", precision, static_cast<double>(v));
        if (std::strtof(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

std::string Literal(const Vec4& v, PinType type) {
    const u32 n = std::max(Components(type), 1u);
    if (n == 1) return Number(v.x);
    const f32 c[4] = {v.x, v.y, v.z, v.w};
    std::string s = std::string(TypeName(type)) + "(";
    for (u32 i = 0; i < n; ++i) s += (i ? ", " : "") + Number(c[i]);
    return s + ")";
}

// A node's per-pin default: a number broadcasts, an array fills in order.
bool ReadDefault(const json& j, Vec4& out) {
    if (j.is_number()) {
        const f32 v = j.get<f32>();
        out = Vec4(v, v, v, v);
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

std::string Convert(const std::string& expr, PinType from, PinType to) {
    if (from == to || to == PinType::Texture) return expr;
    const u32 a = Components(from), b = Components(to);
    if (a == 1) return "((" + std::string(TypeName(to)) + ")(" + expr + "))";
    static const char* kSwizzle[] = {"", "x", "xy", "xyz", "xyzw"};
    return "(" + expr + ")." + kSwizzle[b];
}

u64 Fnv1a(const std::string& s, u64 h = 1469598103934665603ull);

const char* kNoiseHelpers = R"(float3 AetherHash33(float3 p) {
    p = float3(dot(p, float3(127.1, 311.7, 74.7)), dot(p, float3(269.5, 183.3, 246.1)), dot(p, float3(113.5, 271.9, 124.6)));
    return -1.0 + 2.0 * frac(sin(p) * 43758.5453123);
}
float AetherGradient(float3 i, float3 f, float3 corner) {
    return dot(AetherHash33(i + corner), f - corner);
}
// Gradient noise, about -1..1.
float AetherGradientNoise(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(AetherGradient(i, f, float3(0, 0, 0)), AetherGradient(i, f, float3(1, 0, 0)), u.x),
                     lerp(AetherGradient(i, f, float3(0, 1, 0)), AetherGradient(i, f, float3(1, 1, 0)), u.x), u.y),
                lerp(lerp(AetherGradient(i, f, float3(0, 0, 1)), AetherGradient(i, f, float3(1, 0, 1)), u.x),
                     lerp(AetherGradient(i, f, float3(0, 1, 1)), AetherGradient(i, f, float3(1, 1, 1)), u.x), u.y), u.z);
}
float AetherFbm(float3 p, int octaves) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * AetherGradientNoise(p);
        p *= 2.0;
        amplitude *= 0.5;
    }
    return sum;
}
)";

const char* kTriplanarHelpers = R"(float3 AetherTriplanarWeights(float3 n, float sharpness) {
    float3 w = pow(abs(n), sharpness);
    return w / max(w.x + w.y + w.z, 1e-5);
}
float4 AetherTriplanar(Texture2D t, SamplerState s, float3 p, float3 n, float sharpness) {
    float3 w = AetherTriplanarWeights(n, sharpness);
    return t.Sample(s, p.yz) * w.x + t.Sample(s, p.xz) * w.y + t.Sample(s, p.xy) * w.z;
}
float4 AetherTriplanarLevel(Texture2D t, SamplerState s, float3 p, float3 n, float sharpness) {
    float3 w = AetherTriplanarWeights(n, sharpness);
    return t.SampleLevel(s, p.yz, 0) * w.x + t.SampleLevel(s, p.xz, 0) * w.y + t.SampleLevel(s, p.xy, 0) * w.z;
}
)";

struct OutputPin {
    const char* pin;
    const char* field;
    PinType type;
};
constexpr OutputPin kSurfacePins[] = {
    {"BaseColor", "base_color", PinType::Float3}, {"Metallic", "metallic", PinType::Float},
    {"Roughness", "roughness", PinType::Float},   {"Normal", "normal", PinType::Float3},
    {"Emissive", "emissive", PinType::Float3},    {"AO", "ao", PinType::Float},
    {"Opacity", "opacity", PinType::Float},       {"OpacityMask", "opacity_mask", PinType::Float},
};

// Emits one function's body: the nodes that reach the pins it asks for.
struct Emitter {
    const Material& m;
    const Analysis& a;
    const std::map<NodeId, NodeSignature>& sigs;
    const std::map<std::pair<NodeId, std::string>, PinRef>& incoming;
    const std::map<std::string, std::string>& identifiers;
    bool vertex = false;
    MaterialFeatures& features;
    std::set<std::string>& textures;
    std::set<NodeId>& live;
    u32& instructions;
    std::map<std::string, std::string>& helpers; // helper function name -> its HLSL

    std::vector<std::string> lines;
    std::map<std::string, std::string> shared; // expression -> the local holding it
    std::map<NodeId, std::map<std::string, std::string>> done;

    std::string Local(PinType type, const std::string& expr) {
        auto it = shared.find(expr);
        if (it != shared.end()) return it->second;
        const std::string name = "_t" + std::to_string(++instructions);
        lines.push_back(std::string("    ") + TypeName(type) + " " + name + " = " + expr + ";");
        shared.emplace(expr, name);
        return name;
    }

    void UseTexCoord(u32 index) { features.texcoords = std::max(features.texcoords, index + 1); }

    // An input's value as `want`: its link, its implicit value or its constant.
    std::string Input(const Node& node, const PinDesc& p, PinType want) {
        auto link = incoming.find({node.id, p.name});
        if (link != incoming.end()) {
            const PinRef& from = link->second;
            return Convert(Value(from.node, from.pin), a.TypeOf(from), want);
        }
        if (p.implicit == "uv0") {
            UseTexCoord(0);
            return Convert("i.uv[0]", PinType::Float2, want);
        }
        if (p.implicit == "normal") {
            features.vertex_normal = true;
            return Convert("i.vertex_normal", PinType::Float3, want);
        }
        if (p.implicit == "world_position") {
            features.world_position = true;
            return Convert("i.world_position", PinType::Float3, want);
        }
        if (p.implicit == "time") {
            features.time = true;
            return Convert("i.time", PinType::Float, want);
        }
        Vec4 v = p.default_value;
        if (node.defaults.contains(p.name)) ReadDefault(node.defaults[p.name], v);
        return Literal(v, want);
    }

    // The width a generic node works at: its widest connected Any input.
    PinType GenericType(const Node& node, const NodeSignature& sig) {
        u32 width = 1;
        for (const PinDesc& p : sig.inputs) {
            auto link = incoming.find({node.id, p.name});
            if (p.type == PinType::Any && link != incoming.end()) width = std::max(width, Components(a.TypeOf(link->second)));
        }
        return FloatN(width);
    }
    // An input at its own type (Append, ComponentMask), Float when unconnected.
    PinType OwnType(const Node& node, const std::string& pin) {
        auto link = incoming.find({node.id, pin});
        return link == incoming.end() ? PinType::Float : a.TypeOf(link->second);
    }

    std::string Value(NodeId id, const std::string& pin) {
        auto it = done.find(id);
        if (it == done.end()) {
            Emit(id);
            it = done.find(id);
        }
        return it->second.at(pin);
    }

    void Emit(NodeId id) {
        live.insert(id);
        const Node& node = *m.Find(id);
        const NodeSignature& sig = sigs.at(id);
        std::map<std::string, std::string>& out = done[id];
        const std::string& t = node.type;
        auto in = [&](const char* pin, PinType want) { return Input(node, *sig.Input(pin), want); };

        if (t.rfind("Const.", 0) == 0) {
            Vec4 v{0, 0, 0, 0};
            if (node.config.contains("value")) ReadDefault(node.config["value"], v);
            out["value"] = Literal(v, sig.outputs[0].type);
        } else if (t.rfind("Param.", 0) == 0) {
            const std::string name = t.substr(t.find(':') + 1);
            out["value"] = identifiers.at(name);
            if (t.rfind("Param.Texture:", 0) == 0) textures.insert(name);
        } else if (t == "Texture.Sample") {
            const std::string tex = in("texture", PinType::Texture), uv = in("uv", PinType::Float2);
            const std::string v = Local(PinType::Float4, vertex ? tex + ".SampleLevel(MaterialSampler, " + uv + ", 0)"
                                                                : tex + ".Sample(MaterialSampler, " + uv + ")");
            out["rgba"] = v;
            out["rgb"] = v + ".rgb";
            for (const char* c : {"r", "g", "b", "a"}) out[c] = v + "." + c;
        } else if (t == "Input.TexCoord") {
            const u32 index = node.config.value("index", 0u);
            UseTexCoord(index);
            out["uv"] = "i.uv[" + std::to_string(index) + "]";
        } else if (t == "Input.Time") {
            features.time = true;
            out["time"] = "i.time";
        } else if (t == "Input.WorldPosition") {
            features.world_position = true;
            out["position"] = "i.world_position";
        } else if (t == "Input.VertexNormal") {
            features.vertex_normal = true;
            out["normal"] = "i.vertex_normal";
        } else if (t == "Input.CameraVector") {
            features.camera_vector = true;
            out["direction"] = "i.camera_vector";
        } else if (t == "Math.Append") {
            const PinType ta = OwnType(node, "a"), tb = OwnType(node, "b");
            const PinType result = FloatN(Components(ta) + Components(tb));
            out["result"] = Local(result, std::string(TypeName(result)) + "(" + in("a", ta) + ", " + in("b", tb) + ")");
        } else if (t == "Math.ComponentMask") {
            const std::string channels = node.config.value("channels", "r");
            const PinType tx = OwnType(node, "x"), result = FloatN(static_cast<u32>(channels.size()));
            const std::string x = in("x", tx);
            // A float has only r: masking it is a broadcast.
            out["result"] = Components(tx) == 1 ? Convert(x, PinType::Float, result) : Local(result, "(" + x + ")." + channels);
        } else if (t == "Shading.Fresnel") {
            features.camera_vector = true;
            const std::string n = in("normal", PinType::Float3), e = in("exponent", PinType::Float);
            out["result"] = Local(PinType::Float, "pow(1.0 - saturate(dot(normalize(" + n + "), i.camera_vector)), " + e + ")");
        } else if (t == "Coords.Panner") {
            const std::string uv = in("uv", PinType::Float2), speed = in("speed", PinType::Float2), time = in("time", PinType::Float);
            out["uv"] = Local(PinType::Float2, "(" + uv + " + " + speed + " * " + time + ")");
        } else if (t == "Texture.NormalUnpack") {
            out["normal"] = Local(PinType::Float3, "((" + in("packed", PinType::Float4) + ").xyz * 2.0 - 1.0)");
        } else if (t == "Utility.Reroute") {
            out["value"] = in("value", sig.outputs[0].type);
        } else if (t == "Math.Noise") {
            helpers.emplace("AetherNoise", kNoiseHelpers);
            const std::string p = "(" + in("position", PinType::Float3) + " * " + in("scale", PinType::Float) + ")";
            const u32 octaves = node.config.value("octaves", 1u);
            out["result"] = Local(PinType::Float, octaves == 1 ? "AetherGradientNoise(" + p + ")"
                                                               : "AetherFbm(" + p + ", " + std::to_string(octaves) + ")");
        } else if (t == "Texture.Triplanar") {
            helpers.emplace("AetherTriplanar", kTriplanarHelpers);
            const std::string v = Local(PinType::Float4, std::string(vertex ? "AetherTriplanarLevel(" : "AetherTriplanar(") +
                                                             in("texture", PinType::Texture) + ", MaterialSampler, " +
                                                             in("position", PinType::Float3) + " * " + in("scale", PinType::Float) + ", " +
                                                             in("normal", PinType::Float3) + ", " + in("sharpness", PinType::Float) + ")");
            out["rgba"] = v;
            out["rgb"] = v + ".rgb";
        } else if (t == "Custom.HLSL") {
            // Each distinct body and signature becomes one helper function.
            std::string params, args;
            for (const PinDesc& p : sig.inputs) {
                params += std::string(params.empty() ? "" : ", ") + (p.type == PinType::Texture ? "Texture2D" : TypeName(p.type)) + " " + p.name;
                args += (args.empty() ? "" : ", ") + in(p.name.c_str(), p.type);
            }
            const std::string code = node.config.value("code", "return 0.0;");
            const std::string head = std::string(TypeName(sig.outputs[0].type)) + " %s(" + params + ")";
            char name[40];
            std::snprintf(name, sizeof name, "Custom_%016llx", static_cast<unsigned long long>(Fnv1a(head + "\n" + code)));
            std::string text = head;
            text.replace(text.find("%s"), 2, name);
            helpers.emplace(name, text + " {\n" + code + "\n}\n");
            out["result"] = Local(sig.outputs[0].type, std::string(name) + "(" + args + ")");
        } else if (t.rfind("Math.", 0) == 0) {
            const PinType g = GenericType(node, sig);
            std::vector<std::string> args;
            for (const PinDesc& p : sig.inputs) args.push_back(Input(node, p, p.type == PinType::Any ? g : p.type));
            const std::string op = t.substr(5);
            static const std::map<std::string, std::string> kInfix = {{"Add", "+"}, {"Subtract", "-"}, {"Multiply", "*"}, {"Divide", "/"}};
            static const std::map<std::string, std::string> kCall = {
                {"Min", "min"},     {"Max", "max"},     {"Power", "pow"},   {"Abs", "abs"},         {"Sqrt", "sqrt"},
                {"Sin", "sin"},     {"Cos", "cos"},     {"Frac", "frac"},   {"Floor", "floor"},     {"Saturate", "saturate"},
                {"Lerp", "lerp"},   {"Clamp", "clamp"}, {"Dot", "dot"},     {"Length", "length"},   {"Normalize", "normalize"},
            };
            std::string expr;
            if (auto infix = kInfix.find(op); infix != kInfix.end()) {
                expr = "(" + args[0] + " " + infix->second + " " + args[1] + ")";
            } else if (op == "OneMinus") {
                expr = "(1.0 - " + args[0] + ")";
            } else {
                expr = kCall.at(op) + "(";
                for (usize i = 0; i < args.size(); ++i) expr += (i ? ", " : "") + args[i];
                expr += ")";
            }
            out["result"] = Local(sig.outputs[0].type == PinType::Any ? g : sig.outputs[0].type, expr);
        }
    }
};

u64 Fnv1a(const std::string& s, u64 h) {
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace

std::string HlslIdentifier(const std::string& name) {
    std::string out;
    for (const char c : name) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out.empty() ? "_" : out;
}

bool MaterialFeatures::operator==(const MaterialFeatures& o) const {
    return texcoords == o.texcoords && world_position == o.world_position && vertex_normal == o.vertex_normal &&
           camera_vector == o.camera_vector && time == o.time && world_position_offset == o.world_position_offset &&
           alpha_test == o.alpha_test && translucent == o.translucent;
}

const ParameterSlot* ParameterLayout::Find(const std::string& name) const {
    for (const ParameterSlot& s : slots) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

ParameterLayout LayoutParameters(const Material& m) {
    const std::map<std::string, std::string> ids = ParameterIdentifiers(m);
    std::vector<const Parameter*> order;
    for (const Parameter& p : m.parameters) {
        if (p.type != PinType::Texture && Components(p.type) > 0) order.push_back(&p);
    }
    std::stable_sort(order.begin(), order.end(), [](const Parameter* x, const Parameter* y) { return Components(x->type) > Components(y->type); });
    ParameterLayout layout;
    std::vector<u32> fill; // bytes used in each 16-byte register
    for (const Parameter* p : order) {
        const u32 size = Components(p->type) * 4;
        usize reg = 0;
        while (reg < fill.size() && fill[reg] + size > 16) ++reg;
        if (reg == fill.size()) fill.push_back(0);
        layout.slots.push_back({p->name, ids.at(p->name), p->type, static_cast<u32>(reg * 16 + fill[reg]), size});
        fill[reg] += size;
    }
    layout.size = static_cast<u32>(fill.size() * 16);
    return layout;
}

GeneratedMaterial GenerateHlsl(const Material& material) {
    GeneratedMaterial g;
    g.analysis = Analyze(material);
    if (!g.analysis.Ok() || material.is_function) return g;
    // Functions are inlined; the flat graph is what's emitted.
    Material m;
    std::string inline_error;
    if (!InlineFunctions(material, m, &inline_error)) {
        g.analysis.diagnostics.push_back({"MT012", Severity::Error, 0, "", inline_error});
        ++g.analysis.errors;
        return g;
    }
    const Analysis flat = Analyze(m);
    if (!flat.Ok()) {
        g.analysis = flat;
        return g;
    }

    std::map<NodeId, NodeSignature> sigs;
    for (const Node& n : m.nodes) sigs.emplace(n.id, *ResolveNode(m, n));
    std::map<std::pair<NodeId, std::string>, PinRef> incoming;
    for (const Link& l : m.links) incoming.emplace(std::make_pair(l.to.node, l.to.pin), l.from);
    const std::map<std::string, std::string> ids = ParameterIdentifiers(m);
    g.parameters = LayoutParameters(m);

    const bool unlit = m.shading == ShadingModel::Unlit;
    g.features.alpha_test = m.blend == BlendMode::Masked;
    g.features.translucent = m.blend == BlendMode::Translucent || m.blend == BlendMode::Additive;
    auto pin_used = [&](const std::string& pin) {
        if (pin == "Opacity") return g.features.translucent;
        if (pin == "OpacityMask") return g.features.alpha_test;
        if (unlit) return pin == "Emissive";
        return true;
    };
    const NodeId out = flat.output;
    const NodeSignature& out_sig = sigs.at(out);
    std::set<std::string> textures;
    std::set<NodeId> live;
    u32 instructions = 0;
    std::map<std::string, std::string> helpers;

    // The pixel stage: the surface.
    Emitter pixel{m, flat, sigs, incoming, ids, false, g.features, textures, live, instructions, helpers, {}, {}, {}};
    std::vector<std::string> assigns;
    for (const OutputPin& p : kSurfacePins) {
        const PinDesc& desc = *out_sig.Input(p.pin);
        const std::string value = pin_used(p.pin) ? pixel.Input(*m.Find(out), desc, p.type) : Literal(desc.default_value, p.type);
        assigns.push_back(std::string("    o.") + p.field + " = " + value + ";");
    }
    // The vertex stage: the world position offset, when connected.
    Emitter vertex{m, flat, sigs, incoming, ids, true, g.features, textures, live, instructions, helpers, {}, {}, {}};
    std::string wpo = "float3(0.0, 0.0, 0.0)";
    if (incoming.count({out, "WorldPositionOffset"})) {
        g.features.world_position_offset = true;
        wpo = vertex.Input(*m.Find(out), *out_sig.Input("WorldPositionOffset"), PinType::Float3);
    }
    live.erase(out);
    g.live_nodes = static_cast<u32>(live.size());
    g.instructions = instructions;

    u32 slot = 0;
    for (const Parameter& p : m.parameters) {
        if (p.type == PinType::Texture && textures.count(p.name)) g.textures.push_back({p.name, ids.at(p.name), slot++});
    }

    std::string h;
    h += "// Generated by Aether from a material graph. Do not edit.\n";
    h += "#ifndef AETHER_MATERIAL_HLSL\n#define AETHER_MATERIAL_HLSL\n\n";
    if (g.parameters.size > 0) {
        h += "cbuffer MaterialParams : register(b0, space1) {\n";
        for (const ParameterSlot& s : g.parameters.slots) {
            static const char* kComponent = "xyzw";
            h += std::string("    ") + TypeName(s.type) + " " + s.identifier + " : packoffset(c" + std::to_string(s.offset / 16) + "." +
                 kComponent[(s.offset % 16) / 4] + ");\n";
        }
        h += "};\n\n";
    }
    for (const TextureSlot& t : g.textures) h += "Texture2D " + t.identifier + " : register(t" + std::to_string(t.slot) + ", space1);\n";
    if (!g.textures.empty()) h += "SamplerState MaterialSampler : register(s0, space1);\n\n";
    h += "struct MaterialInputs {\n"
         "    float2 uv[4];\n"
         "    float3 world_position;\n"
         "    float3 vertex_normal;\n"
         "    float3 camera_vector; // from the surface toward the camera, normalized\n"
         "    float time;\n"
         "};\n\n"
         "struct MaterialOutputs {\n"
         "    float3 base_color;\n"
         "    float metallic;\n"
         "    float roughness;\n"
         "    float3 normal; // tangent space\n"
         "    float3 emissive;\n"
         "    float ao;\n"
         "    float opacity;\n"
         "    float opacity_mask;\n"
         "};\n\n";
    // Library helpers (noise, triplanar) first, then Custom nodes' functions.
    for (const auto& [name, text] : helpers) {
        if (name.rfind("Aether", 0) == 0) h += text + "\n";
    }
    for (const auto& [name, text] : helpers) {
        if (name.rfind("Custom_", 0) == 0) h += text + "\n";
    }
    h += "MaterialOutputs EvaluateMaterial(MaterialInputs i) {\n    MaterialOutputs o;\n";
    for (const std::string& l : pixel.lines) h += l + "\n";
    for (const std::string& l : assigns) h += l + "\n";
    h += "    return o;\n}\n\n";
    h += "float3 EvaluateWorldPositionOffset(MaterialInputs i) {\n";
    for (const std::string& l : vertex.lines) h += l + "\n";
    h += "    return " + wpo + ";\n}\n\n#endif\n";
    g.hlsl = std::move(h);

    static const char* kBlend[] = {"OPAQUE", "MASKED", "TRANSLUCENT", "ADDITIVE"};
    g.defines.push_back(unlit ? "MATERIAL_SHADING_UNLIT=1" : "MATERIAL_SHADING_DEFAULT_LIT=1");
    g.defines.push_back(std::string("MATERIAL_BLEND_") + kBlend[static_cast<int>(m.blend)] + "=1");
    if (m.two_sided) g.defines.push_back("MATERIAL_TWO_SIDED=1");
    if (g.features.alpha_test) g.defines.push_back("MATERIAL_OPACITY_MASK_CLIP=" + Number(m.opacity_mask_clip));
    if (g.features.world_position_offset) g.defines.push_back("MATERIAL_WORLD_POSITION_OFFSET=1");
    g.defines.push_back("MATERIAL_TEXCOORDS=" + std::to_string(g.features.texcoords));
    if (g.features.world_position) g.defines.push_back("MATERIAL_USES_WORLD_POSITION=1");
    if (g.features.vertex_normal) g.defines.push_back("MATERIAL_USES_VERTEX_NORMAL=1");
    if (g.features.camera_vector) g.defines.push_back("MATERIAL_USES_CAMERA_VECTOR=1");
    if (g.features.time) g.defines.push_back("MATERIAL_USES_TIME=1");

    u64 key = Fnv1a(g.hlsl);
    for (const std::string& d : g.defines) key = Fnv1a(d + "\n", key);
    g.permutation_key = key;
    g.ok = true;
    return g;
}

} // namespace aether::mat
