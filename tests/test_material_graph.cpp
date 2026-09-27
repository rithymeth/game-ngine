#include "aether/renderer/material.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::mat;

// Phase 15 step 1: the material model, node library, type inference and
// validation.

namespace {

// A textured, tinted, lit material: Albedo * Tint -> BaseColor, Roughness param.
Material LitMaterial() {
    Material m;
    m.parameters.push_back({"Albedo", PinType::Texture, {}, "0123456789abcdef", "Surface"});
    m.parameters.push_back({"Tint", PinType::Float3, {1, 0.8f, 0.6f, 0}, "", "Surface"});
    m.parameters.push_back({"Roughness", PinType::Float, {0.4f, 0, 0, 0}, "", ""});
    const NodeId out = m.Add("Material.Output", {}, 600, 0);
    const NodeId tex = m.Add("Param.Texture:Albedo");
    const NodeId sample = m.Add("Texture.Sample");
    const NodeId tint = m.Add("Param.Vector:Tint");
    const NodeId mul = m.Add("Math.Multiply");
    const NodeId rough = m.Add("Param.Scalar:Roughness");
    m.Connect(tex, "value", sample, "texture")
        .Connect(sample, "rgb", mul, "a")
        .Connect(tint, "value", mul, "b")
        .Connect(mul, "result", out, "BaseColor")
        .Connect(rough, "value", out, "Roughness");
    return m;
}

} // namespace

AETHER_TEST(MaterialGraph_TypesAndFits) {
    for (const PinType t : {PinType::Float, PinType::Float2, PinType::Float3, PinType::Float4, PinType::Texture}) {
        AETHER_CHECK(ParseType(TypeName(t)) == t);
    }
    AETHER_CHECK(!ParseType("double").has_value());
    AETHER_CHECK(Components(PinType::Float3) == 3 && Components(PinType::Texture) == 0 && FloatN(2) == PinType::Float2);

    AETHER_CHECK(CanConnect(PinType::Float3, PinType::Float3) == Fit::Yes);
    AETHER_CHECK(CanConnect(PinType::Float, PinType::Float4) == Fit::Broadcast);
    AETHER_CHECK(CanConnect(PinType::Float4, PinType::Float3) == Fit::Truncate);
    AETHER_CHECK(CanConnect(PinType::Float2, PinType::Float3) == Fit::No);
    AETHER_CHECK(CanConnect(PinType::Texture, PinType::Texture) == Fit::Yes);
    AETHER_CHECK(CanConnect(PinType::Texture, PinType::Float4) == Fit::No && CanConnect(PinType::Float, PinType::Texture) == Fit::No);
}

AETHER_TEST(MaterialGraph_LibraryResolvesNodes) {
    const Material m = LitMaterial();
    std::string error;
    const auto sample = ResolveNode(m, Node{1, "Texture.Sample"}, &error);
    AETHER_CHECK(sample && sample->Input("texture")->required && sample->Input("uv")->implicit == "uv0");
    AETHER_CHECK(sample->Output("rgb")->type == PinType::Float3 && sample->Output("nope") == nullptr);
    const auto tint = ResolveNode(m, Node{1, "Param.Vector:Tint"});
    AETHER_CHECK(tint && tint->title == "Tint" && tint->Output("value")->type == PinType::Float3);
    AETHER_CHECK(ResolveNode(m, Node{1, "Math.Lerp"})->generic);

    AETHER_CHECK(!ResolveNode(m, Node{1, "Math.Teleport"}, &error));
    AETHER_CHECK(!ResolveNode(m, Node{1, "Param.Scalar:Missing"}, &error));
    AETHER_CHECK(!ResolveNode(m, Node{1, "Param.Scalar:Tint"}, &error)); // a vector, not a scalar
    Node mask{1, "Math.ComponentMask"};
    mask.config = {{"channels", "rgx"}};
    AETHER_CHECK(!ResolveNode(m, mask, &error));
    mask.config = {{"channels", "gb"}};
    AETHER_CHECK(ResolveNode(m, mask)->Output("result")->type == PinType::Float2);
    Node uv{1, "Input.TexCoord"};
    uv.config = {{"index", 7}};
    AETHER_CHECK(!ResolveNode(m, uv, &error));
    Node constant{1, "Const.Float3"};
    constant.config = {{"value", "red"}};
    AETHER_CHECK(!ResolveNode(m, constant, &error));
    constant.config = {{"value", {1, 0, 0}}};
    AETHER_CHECK(ResolveNode(m, constant)->Output("value")->type == PinType::Float3);

    // The palette lists the library and the material's parameters.
    const std::vector<PaletteEntry> palette = ListNodeTypes(m);
    auto has = [&](const std::string& id) {
        return std::any_of(palette.begin(), palette.end(), [&](const PaletteEntry& e) { return e.id == id; });
    };
    AETHER_CHECK(has("Material.Output") && has("Math.Multiply") && has("Texture.Sample") && has("Coords.Panner"));
    AETHER_CHECK(has("Param.Texture:Albedo") && has("Param.Vector:Tint") && has("Param.Scalar:Roughness"));
    AETHER_CHECK(palette.size() >= 39);
}

AETHER_TEST(MaterialGraph_AnalyzeInfersTypes) {
    Material m = LitMaterial();
    Analysis a = Analyze(m);
    AETHER_CHECK(a.Ok() && a.diagnostics.empty() && a.output == 1);
    AETHER_CHECK(a.TypeOf({5, "result"}) == PinType::Float3); // float3 * float3
    AETHER_CHECK(a.TypeOf({2, "value"}) == PinType::Texture);

    // float3 * float broadcasts to float3.
    m.links.erase(std::remove_if(m.links.begin(), m.links.end(), [](const Link& l) { return l.to == PinRef{5, "b"}; }), m.links.end());
    const NodeId half = m.Add("Const.Float", {{"value", 0.5f}});
    m.Connect(half, "value", 5, "b");
    a = Analyze(m);
    AETHER_CHECK(a.Ok() && a.TypeOf({5, "result"}) == PinType::Float3);

    // Append and ComponentMask.
    Material n;
    const NodeId out = n.Add("Material.Output");
    const NodeId uv = n.Add("Input.TexCoord");
    const NodeId t = n.Add("Input.Time");
    const NodeId append = n.Add("Math.Append");
    const NodeId mask = n.Add("Math.ComponentMask", {{"channels", "rg"}});
    const NodeId dot = n.Add("Math.Dot");
    n.Connect(uv, "uv", append, "a").Connect(t, "time", append, "b").Connect(append, "result", out, "BaseColor");
    n.Connect(append, "result", mask, "x").Connect(mask, "result", dot, "a").Connect(uv, "uv", dot, "b").Connect(dot, "result", out, "Roughness");
    a = Analyze(n);
    AETHER_CHECK(a.Ok());
    AETHER_CHECK(a.TypeOf({append, "result"}) == PinType::Float3 && a.TypeOf({mask, "result"}) == PinType::Float2);
    AETHER_CHECK(a.TypeOf({dot, "result"}) == PinType::Float);

    // Truncation is allowed with a warning: a float4 into BaseColor.
    Material w;
    const NodeId wo = w.Add("Material.Output");
    const NodeId c4 = w.Add("Const.Float4", {{"value", {1, 1, 1, 1}}});
    w.Connect(c4, "value", wo, "BaseColor");
    a = Analyze(w);
    AETHER_CHECK(a.Ok() && a.Has("MT010") && a.diagnostics[0].severity == Severity::Warning);
}

AETHER_TEST(MaterialGraph_AnalyzeReportsEachProblem) {
    // MT001: no output, or two.
    Material none;
    none.Add("Input.Time");
    AETHER_CHECK(Analyze(none).Has("MT001") && !Analyze(none).Ok());
    Material two;
    two.Add("Material.Output");
    two.Add("Material.Output");
    AETHER_CHECK(Analyze(two).Has("MT001"));

    // MT002: an unknown node type or a bad config.
    Material m = LitMaterial();
    m.Add("Math.Teleport");
    AETHER_CHECK(Analyze(m).Has("MT002"));
    m = LitMaterial();
    m.Add("Math.ComponentMask", {{"channels", ""}});
    AETHER_CHECK(Analyze(m).Has("MT002"));

    // MT003: float3 + float2 mixes widths; a texture into math; a float2 into BaseColor; a mask past the end.
    Material mix;
    const NodeId out = mix.Add("Material.Output");
    const NodeId pos = mix.Add("Input.WorldPosition");
    const NodeId uv = mix.Add("Input.TexCoord");
    const NodeId add = mix.Add("Math.Add");
    mix.Connect(pos, "position", add, "a").Connect(uv, "uv", add, "b").Connect(add, "result", out, "BaseColor");
    AETHER_CHECK(Analyze(mix).Has("MT003"));
    m = LitMaterial();
    const NodeId tex = m.Add("Param.Texture:Albedo");
    const NodeId sin = m.Add("Math.Sin");
    m.Connect(tex, "value", sin, "x");
    AETHER_CHECK(Analyze(m).Has("MT003"));
    Material narrow;
    const NodeId no = narrow.Add("Material.Output");
    narrow.Connect(narrow.Add("Input.TexCoord"), "uv", no, "BaseColor");
    AETHER_CHECK(Analyze(narrow).Has("MT003"));
    Material past;
    const NodeId po = past.Add("Material.Output");
    const NodeId pm = past.Add("Math.ComponentMask", {{"channels", "b"}});
    past.Connect(past.Add("Input.TexCoord"), "uv", pm, "x").Connect(pm, "result", po, "Metallic");
    AETHER_CHECK(Analyze(past).Has("MT003"));

    // MT004: a pin or node that doesn't exist.
    m = LitMaterial();
    m.Connect(4, "value", 1, "Sparkle");
    AETHER_CHECK(Analyze(m).Has("MT004"));
    m = LitMaterial();
    m.Connect(99, "value", 1, "Metallic");
    AETHER_CHECK(Analyze(m).Has("MT004"));
    m = LitMaterial();
    m.Connect(4, "colour", 1, "Emissive");
    AETHER_CHECK(Analyze(m).Has("MT004"));

    // MT005: a cycle.
    Material loop;
    const NodeId lo = loop.Add("Material.Output");
    const NodeId a1 = loop.Add("Math.Add"), a2 = loop.Add("Math.Sin");
    loop.Connect(a1, "result", a2, "x").Connect(a2, "result", a1, "a").Connect(a1, "result", lo, "Metallic");
    const Analysis looped = Analyze(loop);
    AETHER_CHECK(looped.Has("MT005") && !looped.Ok());

    // MT006: an unknown parameter or the wrong kind.
    m = LitMaterial();
    m.Add("Param.Scalar:Gloss");
    AETHER_CHECK(Analyze(m).Has("MT006") && !Analyze(m).Has("MT002"));
    m = LitMaterial();
    m.Add("Param.Texture:Tint");
    AETHER_CHECK(Analyze(m).Has("MT006"));

    // MT007: a texture sample with no texture.
    m = LitMaterial();
    m.Add("Texture.Sample");
    AETHER_CHECK(Analyze(m).Has("MT007"));

    // MT008: two links into one input.
    m = LitMaterial();
    m.Connect(6, "value", 1, "Metallic").Connect(6, "value", 1, "Metallic");
    AETHER_CHECK(Analyze(m).Has("MT008"));

    // MT009 (warnings): pins the settings ignore.
    m = LitMaterial();
    m.Connect(6, "value", 1, "Opacity");
    Analysis a = Analyze(m);
    AETHER_CHECK(a.Has("MT009") && a.Ok());
    m.blend = BlendMode::Translucent;
    AETHER_CHECK(!Analyze(m).Has("MT009"));
    m = LitMaterial();
    m.Connect(6, "value", 1, "OpacityMask");
    AETHER_CHECK(Analyze(m).Has("MT009"));
    m.blend = BlendMode::Masked;
    AETHER_CHECK(!Analyze(m).Has("MT009"));
    m = LitMaterial();
    m.shading = ShadingModel::Unlit;
    a = Analyze(m);
    AETHER_CHECK(a.Has("MT009") && a.Ok()); // Roughness is connected
    for (const Diagnostic& d : a.diagnostics) AETHER_CHECK(!d.message.empty());
}

AETHER_TEST(MaterialGraph_JsonRoundTrip) {
    Material m = LitMaterial();
    m.blend = BlendMode::Masked;
    m.two_sided = true;
    m.opacity_mask_clip = 0.5f;
    m.Find(3)->defaults = {{"uv", {0.5f, 0.5f}}};
    m.Find(4)->x = 12.5f;
    const nlohmann::json j = MaterialToJson(m);
    Material back;
    std::string error;
    AETHER_CHECK(MaterialFromJson(j, back, &error));
    AETHER_CHECK(back.blend == BlendMode::Masked && back.two_sided && back.opacity_mask_clip == 0.5f);
    AETHER_CHECK(back.parameters.size() == 3 && back.parameters[0].texture == "0123456789abcdef" && back.parameters[1].group == "Surface");
    AETHER_CHECK(back.parameters[1].default_value.y == 0.8f && back.parameters[1].type == PinType::Float3);
    AETHER_CHECK(back.nodes.size() == m.nodes.size() && back.links.size() == m.links.size());
    AETHER_CHECK(back.Find(4)->x == 12.5f && back.Find(3)->defaults == m.Find(3)->defaults);
    AETHER_CHECK(MaterialToJson(back) == j);
    AETHER_CHECK(Analyze(back).Ok());

    // Bad files.
    AETHER_CHECK(!MaterialFromJson(nlohmann::json{{"$type", "Blueprint"}}, back, &error));
    nlohmann::json newer = j;
    newer["$version"] = 999;
    AETHER_CHECK(!MaterialFromJson(newer, back, &error) && error.find("newer") != std::string::npos);
    nlohmann::json bad = j;
    bad["blend"] = "Glitter";
    AETHER_CHECK(!MaterialFromJson(bad, back, &error));
    bad = j;
    bad["nodes"].push_back(bad["nodes"][0]); // a duplicate id
    AETHER_CHECK(!MaterialFromJson(bad, back, &error));
    bad = j;
    bad["parameters"].push_back(bad["parameters"][0]); // a duplicate name
    AETHER_CHECK(!MaterialFromJson(bad, back, &error));
    bad = j;
    bad["links"].push_back({{"from", 3}});
    AETHER_CHECK(!MaterialFromJson(bad, back, &error));

    // Files.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_material_test";
    std::filesystem::create_directories(dir);
    AETHER_CHECK(SaveMaterial(m, dir / "M_Lit.amat", &error));
    Material loaded;
    AETHER_CHECK(LoadMaterial(dir / "M_Lit.amat", loaded, &error) && MaterialToJson(loaded) == j);
    AETHER_CHECK(!LoadMaterial(dir / "missing.amat", loaded, &error));
    { std::ofstream(dir / "junk.amat") << "{ not json"; }
    AETHER_CHECK(!LoadMaterial(dir / "junk.amat", loaded, &error));
    std::filesystem::remove_all(dir);
}
