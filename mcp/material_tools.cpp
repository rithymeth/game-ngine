#include "material_tools.h"

#include "project_host.h"

#include "aether/renderer/material.h"
#include "aether/renderer/material_codegen.h"
#include "aether/renderer/material_instance.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <fstream>
#include <sstream>

namespace aether::mcp {

namespace {

using namespace detail;
namespace mt = aether::mat;

// JSON from `<def_key>` (an object or its text) or `<file_key>` (under Content/ of the open project, or absolute).
Json ReadJsonArg(AssetHost& host, const Json& args, const char* def_key, const char* file_key) {
    if (args.contains(def_key)) {
        const Json& d = args[def_key];
        if (d.is_object()) return d;
        if (!d.is_string()) throw ToolError(std::string("\"") + def_key + "\" must be a JSON object or a string of JSON");
        const Json parsed = Json::parse(d.get<std::string>(), nullptr, false);
        if (parsed.is_discarded()) throw ToolError(std::string("\"") + def_key + "\" is not valid JSON");
        return parsed;
    }
    if (args.contains(file_key)) {
        const std::string name = RequireString(args, file_key);
        fs::path path(name);
        if (!path.is_absolute()) path = ContentFile(host.Require(), name);
        std::ifstream in(path, std::ios::binary);
        if (!in) throw ToolError("Can't read " + path.string());
        std::stringstream ss;
        ss << in.rdbuf();
        const Json parsed = Json::parse(ss.str(), nullptr, false);
        if (parsed.is_discarded()) throw ToolError(name + " is not valid JSON");
        return parsed;
    }
    throw ToolError(std::string("Give \"") + def_key + "\" (as JSON) or \"" + file_key + "\" (a file under Content/)");
}

mt::Material ParseMaterial(const Json& j, const char* what = "material") {
    mt::Material m;
    std::string error;
    if (!mt::MaterialFromJson(j, m, &error)) throw ToolError(std::string("Not a valid ") + what + ": " + error);
    return m;
}

// The material, with `functions` ({name: definition}) as the library its Function.Call nodes resolve against.
mt::Material ReadMaterial(AssetHost& host, const Json& args) {
    mt::Material m = ParseMaterial(ReadJsonArg(host, args, "definition", "file"));
    if (args.contains("functions")) {
        if (!args["functions"].is_object()) throw ToolError("\"functions\" must be an object of name -> material function");
        auto library = std::make_shared<mt::FunctionLibrary>();
        for (const auto& item : args["functions"].items()) library->functions[item.key()] = ParseMaterial(item.value(), "material function");
        m.functions = library;
    }
    return m;
}

const char* SeverityName(mt::Severity s) { return s == mt::Severity::Error ? "error" : "warning"; }

Json DiagnosticJson(const mt::Diagnostic& d) {
    Json j = {{"code", d.code}, {"severity", SeverityName(d.severity)}, {"message", d.message}};
    if (d.node != 0) j["node"] = d.node;
    if (!d.pin.empty()) j["pin"] = d.pin;
    return j;
}

Json DiagnosticsJson(const mt::Analysis& a) {
    Json out = Json::array();
    for (const mt::Diagnostic& d : a.diagnostics) out.push_back(DiagnosticJson(d));
    return out;
}

Json PinsJson(const std::vector<mt::PinDesc>& pins) {
    Json out = Json::array();
    for (const mt::PinDesc& p : pins) {
        Json j = {{"name", p.name}, {"type", mt::TypeName(p.type)}};
        if (p.required) j["required"] = true;
        if (!p.implicit.empty()) j["implicit"] = p.implicit;
        out.push_back(j);
    }
    return out;
}

const char* BlendName(mt::BlendMode b) {
    switch (b) {
    case mt::BlendMode::Opaque: return "Opaque";
    case mt::BlendMode::Masked: return "Masked";
    case mt::BlendMode::Translucent: return "Translucent";
    case mt::BlendMode::Additive: return "Additive";
    }
    return "Opaque";
}

Json Vec4Json(const Vec4& v) { return Json::array({v.x, v.y, v.z, v.w}); }

} // namespace

void RegisterMaterialTools(McpServer& server, std::shared_ptr<AssetHost> host) {
    server.AddTool(
        {"mat_node_types",
         "The node library a material graph is built from: every node type's id (\"Math.Multiply\", \"Material.Output\" ...), title and category, with its input and output pins "
         "(name, type float / float2 / float3 / float4 / texture, required, implicit) where the type alone fixes them. Filter with `category` or `search` (matches id or title). "
         "Give `definition` / `file` to include that material's own parameter nodes (\"Param.Scalar:Roughness\") in the list.",
         Schema({{"category", {{"type", "string"}}},
                 {"search", {{"type", "string"}}},
                 {"definition", {{"description", "A material whose parameters join the list: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}}}}),
         [host](const Json& args) -> Json {
             mt::Material material;
             if (args.contains("definition") || args.contains("file")) material = ReadMaterial(*host, args);
             const std::string category = OptString(args, "category");
             std::string search = OptString(args, "search");
             std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
             Json nodes = Json::array();
             std::set<std::string> categories;
             for (const mt::PaletteEntry& e : mt::ListNodeTypes(material)) {
                 categories.insert(e.category);
                 if (!category.empty() && e.category != category) continue;
                 if (!search.empty()) {
                     std::string hay = e.id + " " + e.title;
                     std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                     if (hay.find(search) == std::string::npos) continue;
                 }
                 Json j = {{"id", e.id}, {"title", e.title}, {"category", e.category}};
                 mt::Node probe;
                 probe.type = e.id;
                 if (const std::optional<mt::NodeSignature> sig = mt::ResolveNode(material, probe)) {
                     j["inputs"] = PinsJson(sig->inputs);
                     j["outputs"] = PinsJson(sig->outputs);
                     if (sig->generic) j["generic"] = true;
                 }
                 nodes.push_back(j);
             }
             return {{"count", nodes.size()}, {"categories", Json(std::vector<std::string>(categories.begin(), categories.end()))}, {"nodes", nodes}};
         }});

    server.AddTool(
        {"mat_validate",
         "Check a material graph with the renderer's own analysis: MT001 no / several Material Outputs, MT002 unknown node type or bad config, MT003 incompatible types on a link, "
         "MT004 a link to a pin that doesn't exist, MT005 a cycle, MT006 unknown parameter, MT007 a required input unconnected, MT008 an input with two links, MT009 an output the "
         "material's settings ignore, MT010 truncation, MT011..MT013 function problems. Returns each as {code, severity, node, pin, message}, the inferred type of every connected "
         "output pin, and a summary (shading model, blend mode, parameters, node count). Function.Call nodes need `functions` ({name: material function definition}). "
         "Give `definition` or `file`.",
         Schema({{"definition", {{"description", "The material: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}}},
                 {"functions", {{"type", "object"}, {"description", "name -> material function (.amf) definition, for Function.Call nodes"}}}}),
         [host](const Json& args) -> Json {
             const mt::Material m = ReadMaterial(*host, args);
             const mt::Analysis a = mt::Analyze(m);
             Json types = Json::array();
             for (const auto& t : a.types) types.push_back({{"node", t.first.node}, {"pin", t.first.pin}, {"type", mt::TypeName(t.second)}});
             Json params = Json::array();
             for (const mt::Parameter& p : m.parameters) params.push_back({{"name", p.name}, {"type", mt::TypeName(p.type)}});
             return {{"ok", a.Ok()},
                     {"errors", a.errors},
                     {"diagnostics", DiagnosticsJson(a)},
                     {"types", types},
                     {"summary", {{"function", m.is_function}, {"shading", m.shading == mt::ShadingModel::Unlit ? "Unlit" : "DefaultLit"}, {"blend", BlendName(m.blend)},
                                  {"two_sided", m.two_sided}, {"nodes", m.nodes.size()}, {"links", m.links.size()}, {"parameters", params}}}};
         }});

    server.AddTool(
        {"mat_compile",
         "Compile a material graph to the HLSL include the renderer's passes call, on the CPU (no shader is built). Returns whether it compiled (else the diagnostics), the shader "
         "defines, the MaterialParams constant buffer layout (each parameter's HLSL name, byte offset and size, and the buffer size), the textures it samples (name, register slot), "
         "the features it needs (UV sets, world position, normal, camera vector, time, alpha test, translucent), the number of live nodes, a rough instruction count, and the "
         "permutation key (equal keys mean equal code and settings). The HLSL text is included unless `hlsl` is false (it can be long). Function.Call nodes need `functions`. "
         "Give `definition` or `file`.",
         Schema({{"definition", {{"description", "The material: a JSON object, or its text"}}},
                 {"file", {{"type", "string"}}},
                 {"functions", {{"type", "object"}}},
                 {"hlsl", {{"type", "boolean"}, {"description", "Include the HLSL text (default true)"}}}}),
         [host](const Json& args) -> Json {
             const mt::Material m = ReadMaterial(*host, args);
             const mt::GeneratedMaterial g = mt::GenerateHlsl(m);
             Json out = {{"ok", g.ok}, {"diagnostics", DiagnosticsJson(g.analysis)}};
             if (!g.ok) return out;
             Json slots = Json::array();
             for (const mt::ParameterSlot& s : g.parameters.slots) {
                 slots.push_back({{"name", s.name}, {"identifier", s.identifier}, {"type", mt::TypeName(s.type)}, {"offset", s.offset}, {"size", s.size}});
             }
             Json textures = Json::array();
             for (const mt::TextureSlot& t : g.textures) textures.push_back({{"name", t.name}, {"identifier", t.identifier}, {"slot", t.slot}});
             out["defines"] = g.defines;
             out["parameter_buffer"] = {{"bytes", g.parameters.size}, {"slots", slots}};
             out["textures"] = textures;
             out["features"] = {{"texcoords", g.features.texcoords},
                                {"world_position", g.features.world_position},
                                {"vertex_normal", g.features.vertex_normal},
                                {"camera_vector", g.features.camera_vector},
                                {"time", g.features.time},
                                {"world_position_offset", g.features.world_position_offset},
                                {"alpha_test", g.features.alpha_test},
                                {"translucent", g.features.translucent}};
             out["live_nodes"] = g.live_nodes;
             out["instructions"] = g.instructions;
             out["permutation_key"] = std::to_string(g.permutation_key);
             out["hlsl_bytes"] = g.hlsl.size();
             if (OptBool(args, "hlsl", true)) out["hlsl"] = g.hlsl;
             return out;
         }});

    server.AddTool(
        {"mat_instance_resolve",
         "Resolve a material instance chain to the parameter values a draw would use: the material's defaults with each instance's overrides applied in turn. Give the material as "
         "`material` / `material_file` and `instances` (an array of instance definitions, parent first, the one to resolve last). Returns every parameter with its final value (or "
         "texture) and which level set it (0 = the material's default, n = the nth instance), and the diagnostics: MI001 an override for a parameter the material doesn't have, "
         "MI002 a value for a texture parameter or the reverse (both are skipped, as at runtime). Also the packed MaterialParams buffer size.",
         Schema({{"material", {{"description", "The material: a JSON object, or its text"}}},
                 {"material_file", {{"type", "string"}}},
                 {"instances", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
                {"instances"}),
         [host](const Json& args) -> Json {
             const mt::Material material = ParseMaterial(ReadJsonArg(*host, args, "material", "material_file"));
             if (!args["instances"].is_array()) throw ToolError("\"instances\" must be an array of instance definitions, parent first");
             std::vector<mt::MaterialInstance> instances;
             for (const Json& j : args["instances"]) {
                 mt::MaterialInstance inst;
                 std::string error;
                 if (!mt::InstanceFromJson(j, inst, &error)) throw ToolError("Not a valid material instance: " + error);
                 instances.push_back(std::move(inst));
             }
             std::vector<const mt::MaterialInstance*> chain;
             for (const mt::MaterialInstance& i : instances) chain.push_back(&i);
             std::vector<mt::Diagnostic> diagnostics;
             const mt::ResolvedParameters resolved = mt::ResolveParameters(material, chain, &diagnostics);
             Json values = Json::array();
             for (const mt::ResolvedParameter& r : resolved.values) {
                 Json j = {{"name", r.name}, {"type", mt::TypeName(r.type)}, {"source", r.source}};
                 if (r.type == mt::PinType::Texture) j["texture"] = r.texture;
                 else j["value"] = Vec4Json(r.value);
                 values.push_back(j);
             }
             Json diags = Json::array();
             for (const mt::Diagnostic& d : diagnostics) diags.push_back(DiagnosticJson(d));
             const mt::ParameterLayout layout = mt::LayoutParameters(material);
             return {{"parameters", values}, {"diagnostics", diags}, {"buffer_bytes", layout.size}};
         }});
}

} // namespace aether::mcp
