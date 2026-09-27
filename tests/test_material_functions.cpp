#include "aether/renderer/material_codegen.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::mat;
using nlohmann::json;

// Phase 15 step 4: material functions, Custom HLSL, noise and triplanar mapping.

namespace {

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
usize Count(const std::string& s, const std::string& part) {
    usize n = 0;
    for (usize at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) ++n;
    return n;
}

// Tile(UV: float2, Scale: float = 2) -> Result: float2 = UV * Scale.
Material TileFunction() {
    Material f;
    f.is_function = true;
    f.description = "Scales texture coordinates.";
    const NodeId uv = f.Add("Function.Input", {{"name", "UV"}, {"type", "float2"}});
    const NodeId scale = f.Add("Function.Input", {{"name", "Scale"}, {"type", "float"}, {"default", 2}});
    const NodeId mul = f.Add("Math.Multiply");
    const NodeId out = f.Add("Function.Output", {{"name", "Result"}, {"type", "float2"}});
    f.Connect(uv, "value", mul, "a").Connect(scale, "value", mul, "b").Connect(mul, "result", out, "value");
    return f;
}

// Brightness(Color: float3) -> Luma: float3 (declared wider than the float it computes).
Material BrightnessFunction() {
    Material f;
    f.is_function = true;
    const NodeId color = f.Add("Function.Input", {{"name", "Color"}, {"type", "float3"}, {"default", {1, 1, 1}}});
    const NodeId dot = f.Add("Math.Dot");
    f.Find(dot)->defaults = {{"b", {0.2126, 0.7152, 0.0722}}};
    const NodeId out = f.Add("Function.Output", {{"name", "Luma"}, {"type", "float3"}});
    f.Connect(color, "value", dot, "a").Connect(dot, "result", out, "value");
    return f;
}

// Detail(UV) -> Result = Tile(Tile(UV, 4)): a function that calls another, twice.
Material DetailFunction() {
    Material f;
    f.is_function = true;
    const NodeId uv = f.Add("Function.Input", {{"name", "UV"}, {"type", "float2"}});
    const NodeId t1 = f.Add("Function.Call:Tile"), t2 = f.Add("Function.Call:Tile");
    f.Find(t1)->defaults = {{"Scale", 4}};
    const NodeId out = f.Add("Function.Output", {{"name", "Result"}, {"type", "float2"}});
    f.Connect(uv, "value", t1, "UV").Connect(t1, "Result", t2, "UV").Connect(t2, "Result", out, "value");
    return f;
}

std::shared_ptr<FunctionLibrary> Library() {
    auto lib = std::make_shared<FunctionLibrary>();
    lib->functions["Tile"] = TileFunction();
    lib->functions["Brightness"] = BrightnessFunction();
    lib->functions["Detail"] = DetailFunction();
    return lib;
}

bool CompilesAsHlsl(const GeneratedMaterial& g) {
    static const bool available = std::system("glslangValidator --version > /dev/null 2>&1") == 0;
    if (!available) return true;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_material_functions";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "m.hlsl");
        f << g.hlsl
          << "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, float3 wp : TEXCOORD1) : SV_Target {\n"
             "    MaterialInputs i = (MaterialInputs)0;\n"
             "    i.uv[0] = uv; i.uv[1] = uv; i.world_position = wp; i.vertex_normal = float3(0, 0, 1);\n"
             "    i.camera_vector = normalize(-wp); i.time = pos.x;\n"
             "    MaterialOutputs o = EvaluateMaterial(i);\n"
             "    return float4(o.base_color + o.emissive + o.normal * o.roughness + EvaluateWorldPositionOffset(i), o.opacity);\n"
             "}\n";
    }
    std::string defines;
    for (const std::string& d : g.defines) defines += " -D" + d;
    const std::string cmd = "glslangValidator -D -V -S frag -e main" + defines + " \"" + (dir / "m.hlsl").string() + "\" -o \"" +
                            (dir / "m.spv").string() + "\" > \"" + (dir / "log.txt").string() + "\" 2>&1";
    const bool ok = std::system(cmd.c_str()) == 0;
    if (!ok) {
        std::ifstream l(dir / "log.txt");
        std::printf("%s\n%s\n", g.hlsl.c_str(), std::string(std::istreambuf_iterator<char>(l), {}).c_str());
    }
    std::filesystem::remove_all(dir);
    return ok;
}

} // namespace

AETHER_TEST(MaterialFunctions_InterfacesAndValidation) {
    Material tile = TileFunction();
    AETHER_CHECK(Analyze(tile).Ok());
    AETHER_CHECK(!GenerateHlsl(tile).ok); // a function isn't a material on its own

    Material m;
    m.functions = Library();
    const std::optional<NodeSignature> call = ResolveNode(m, Node{1, "Function.Call:Tile"});
    AETHER_CHECK(call && call->title == "Tile" && call->inputs.size() == 2 && call->outputs.size() == 1);
    AETHER_CHECK(call->Input("UV")->type == PinType::Float2 && call->Input("Scale")->default_value.x == 2.0f);
    AETHER_CHECK(call->Output("Result")->type == PinType::Float2);

    // Palettes: functions show their calls; function graphs show the interface nodes.
    auto has = [](const std::vector<PaletteEntry>& p, const std::string& id) {
        return std::any_of(p.begin(), p.end(), [&](const PaletteEntry& e) { return e.id == id; });
    };
    const std::vector<PaletteEntry> material_palette = ListNodeTypes(m);
    AETHER_CHECK(has(material_palette, "Function.Call:Detail") && has(material_palette, "Material.Output"));
    AETHER_CHECK(!has(material_palette, "Function.Input") && has(material_palette, "Custom.HLSL"));
    AETHER_CHECK(has(material_palette, "Math.Noise") && has(material_palette, "Texture.Triplanar") && has(material_palette, "Utility.Reroute"));
    const std::vector<PaletteEntry> function_palette = ListNodeTypes(tile);
    AETHER_CHECK(has(function_palette, "Function.Input") && has(function_palette, "Function.Output") && !has(function_palette, "Material.Output"));

    // MT011: a function's interface.
    Material no_output = tile;
    no_output.nodes.erase(no_output.nodes.begin() + 3);
    no_output.links.pop_back();
    AETHER_CHECK(Analyze(no_output).Has("MT011"));
    Material twice = tile;
    twice.Add("Function.Input", {{"name", "UV"}, {"type", "float3"}});
    AETHER_CHECK(Analyze(twice).Has("MT011"));
    Material with_output = tile;
    with_output.Add("Material.Output");
    AETHER_CHECK(Analyze(with_output).Has("MT011"));
    // MT002: interface nodes outside a function.
    Material stray;
    stray.Add("Material.Output");
    stray.Add("Function.Input");
    AETHER_CHECK(Analyze(stray).Has("MT002"));

    // MT012: unknown functions and recursion; MT013: a broken function.
    Material unknown;
    unknown.functions = Library();
    unknown.Add("Material.Output");
    unknown.Add("Function.Call:Nope");
    AETHER_CHECK(Analyze(unknown).Has("MT012"));
    auto looping = std::make_shared<FunctionLibrary>(*Library());
    Material a = TileFunction(), b = TileFunction();
    a.Add("Function.Call:B");
    b.Add("Function.Call:A");
    looping->functions["A"] = a;
    looping->functions["B"] = b;
    Material caller;
    caller.functions = looping;
    caller.Add("Material.Output");
    caller.Add("Function.Call:A");
    const Analysis loop = Analyze(caller);
    AETHER_CHECK(loop.Has("MT012") && !GenerateHlsl(caller).ok);
    auto broken_lib = std::make_shared<FunctionLibrary>();
    Material broken = TileFunction();
    broken.Add("Math.Teleport");
    broken_lib->functions["Broken"] = broken;
    Material uses_broken;
    uses_broken.functions = broken_lib;
    uses_broken.Add("Material.Output");
    uses_broken.Add("Function.Call:Broken");
    AETHER_CHECK(Analyze(uses_broken).Has("MT013"));

    // Functions save as MaterialFunction.
    const json j = MaterialToJson(tile);
    AETHER_CHECK(j["$type"] == "MaterialFunction" && j["description"] == "Scales texture coordinates." && !j.contains("blend"));
    Material back;
    std::string error;
    AETHER_CHECK(MaterialFromJson(j, back, &error) && back.is_function && MaterialToJson(back) == j);
}

AETHER_TEST(MaterialFunctions_InlineKeepsTypesAndDefaults) {
    Material m;
    m.functions = Library();
    const NodeId out = m.Add("Material.Output");
    const NodeId detail = m.Add("Function.Call:Detail");
    m.Connect(m.Add("Input.TexCoord"), "uv", detail, "UV");
    const NodeId append = m.Add("Math.Append");
    m.Connect(detail, "Result", append, "a").Connect(append, "result", out, "BaseColor");
    // Brightness declares a float3 output fed by a float: masking its g channel still works.
    const NodeId bright = m.Add("Function.Call:Brightness"), mask = m.Add("Math.ComponentMask", {{"channels", "g"}});
    m.Connect(bright, "Luma", mask, "x").Connect(mask, "result", out, "Roughness");
    AETHER_CHECK(Analyze(m).Ok());

    Material flat;
    std::string error;
    AETHER_CHECK(InlineFunctions(m, flat, &error));
    AETHER_CHECK(std::none_of(flat.nodes.begin(), flat.nodes.end(), [](const Node& n) { return n.type.rfind("Function.", 0) == 0; }));
    AETHER_CHECK(Analyze(flat).Ok());
    usize reroutes = 0;
    for (const Node& n : flat.nodes) reroutes += n.type == "Utility.Reroute";
    AETHER_CHECK(reroutes == 2 + 2 * 3 + 2); // Detail (2), Tile twice (3 each), Brightness (2)

    const GeneratedMaterial g = GenerateHlsl(m);
    AETHER_CHECK(g.ok && g.features.texcoords == 1);
    AETHER_CHECK(Contains(g.hlsl, "float2 _t1 = (i.uv[0] * ((float2)(4.0)));"));          // the call's default Scale = 4
    AETHER_CHECK(Contains(g.hlsl, "float2 _t2 = (_t1 * ((float2)(2.0)));"));                   // the function's own default Scale = 2
    AETHER_CHECK(Contains(g.hlsl, "dot(float3(1.0, 1.0, 1.0), float3(0.2126, 0.7152, 0.0722))")); // Color's default
    AETHER_CHECK(Contains(g.hlsl, "float _t5 = (((float3)(_t4))).g;"));  // the float widened to Luma's float3, then masked
    AETHER_CHECK(CompilesAsHlsl(g));
    // Inlining leaves the original alone.
    AETHER_CHECK(m.nodes.size() == 6 && std::any_of(m.nodes.begin(), m.nodes.end(), [](const Node& n) { return n.type == "Function.Call:Detail"; }));
}

AETHER_TEST(MaterialFunctions_CustomNoiseAndTriplanar) {
    Material m;
    m.parameters.push_back({"Rock", PinType::Texture, {}, "", ""});
    const NodeId out = m.Add("Material.Output");
    const json custom = {{"title", "Tint Shift"},
                         {"inputs", {{{"name", "color"}, {"type", "float3"}}, {{"name", "amount"}, {"type", "float"}}}},
                         {"output", "float3"},
                         {"code", "return color * (1.0 + amount);"}};
    const NodeId c1 = m.Add("Custom.HLSL", custom), c2 = m.Add("Custom.HLSL", custom);
    const NodeId rock = m.Add("Param.Texture:Rock"), tri = m.Add("Texture.Triplanar");
    m.Connect(rock, "value", tri, "texture").Connect(tri, "rgb", c1, "color").Connect(c1, "result", out, "BaseColor");
    const NodeId noise = m.Add("Math.Noise", {{"octaves", 4}});
    m.Connect(noise, "result", c1, "amount").Connect(m.Add("Input.VertexNormal"), "normal", c2, "color").Connect(c2, "result", out, "Emissive");
    const NodeId single = m.Add("Math.Noise");
    m.Connect(single, "result", out, "Roughness");
    // Triplanar in the vertex stage too.
    const NodeId tri_v = m.Add("Texture.Triplanar");
    m.Connect(rock, "value", tri_v, "texture").Connect(tri_v, "rgb", out, "WorldPositionOffset");
    const Analysis a = Analyze(m);
    AETHER_CHECK(a.Ok() && ResolveNode(m, *m.Find(c1))->title == "Tint Shift");

    const GeneratedMaterial g = GenerateHlsl(m);
    AETHER_CHECK(g.ok && g.features.world_position && g.features.vertex_normal);
    AETHER_CHECK(Count(g.hlsl, "float3 Custom_") == 1 && Contains(g.hlsl, "(float3 color, float amount) {\nreturn color * (1.0 + amount);\n}"));
    AETHER_CHECK(Count(g.hlsl, "float AetherGradientNoise(float3 p)") == 1 && Contains(g.hlsl, "AetherFbm((i.world_position * 1.0), 4)"));
    AETHER_CHECK(Contains(g.hlsl, "AetherGradientNoise((i.world_position * 1.0))"));
    AETHER_CHECK(Contains(g.hlsl, "AetherTriplanar(T_Rock, MaterialSampler, i.world_position * 1.0, i.vertex_normal, 4.0)"));
    AETHER_CHECK(Contains(g.hlsl, "AetherTriplanarLevel(T_Rock, MaterialSampler,"));
    AETHER_CHECK(CompilesAsHlsl(g));

    // Bad configs: MT002.
    auto bad = [&](const json& config) {
        Material b;
        b.Add("Material.Output");
        b.Add("Custom.HLSL", config);
        return Analyze(b).Has("MT002");
    };
    AETHER_CHECK(bad({{"inputs", {{{"name", "i"}, {"type", "float"}}}}}));       // shadows the material inputs
    AETHER_CHECK(bad({{"inputs", {{{"name", "2x"}, {"type", "float"}}}}}));
    AETHER_CHECK(bad({{"inputs", {{{"name", "a"}, {"type", "float"}}, {{"name", "a"}, {"type", "float2"}}}}}));
    AETHER_CHECK(bad({{"inputs", {{{"name", "a"}, {"type", "double"}}}}}));
    AETHER_CHECK(bad({{"output", "texture"}}) && bad({{"code", "  \n"}}) && !bad(json::object()));
    Material octaves;
    octaves.Add("Material.Output");
    octaves.Add("Math.Noise", {{"octaves", 9}});
    AETHER_CHECK(Analyze(octaves).Has("MT002"));
    Material unsampled;
    unsampled.Add("Material.Output");
    unsampled.Add("Texture.Triplanar");
    AETHER_CHECK(Analyze(unsampled).Has("MT007"));
}
