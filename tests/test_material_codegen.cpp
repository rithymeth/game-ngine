#include "aether/renderer/material_codegen.h"
#include "test_framework.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace aether;
using namespace aether::mat;

// Phase 15 step 2: HLSL generation from material graphs.

namespace {

bool Contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

usize Count(const std::string& s, const std::string& part) {
    usize n = 0;
    for (usize at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) ++n;
    return n;
}

// Albedo * Tint -> BaseColor, Roughness param -> Roughness.
Material LitMaterial() {
    Material m;
    m.parameters.push_back({"Albedo", PinType::Texture, {}, "guid", ""});
    m.parameters.push_back({"Tint", PinType::Float3, {1, 0.8f, 0.6f, 0}, "", ""});
    m.parameters.push_back({"Roughness", PinType::Float, {0.4f, 0, 0, 0}, "", ""});
    const NodeId out = m.Add("Material.Output");
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

// Every node type in the library, wired into one valid material.
Material KitchenSink() {
    Material m;
    m.blend = BlendMode::Masked;
    m.parameters.push_back({"Albedo", PinType::Texture, {}, "", ""});
    m.parameters.push_back({"NormalMap", PinType::Texture, {}, "", ""});
    m.parameters.push_back({"Unused", PinType::Texture, {}, "", ""});
    m.parameters.push_back({"Tint", PinType::Float4, {1, 1, 1, 1}, "", ""});
    m.parameters.push_back({"Glow", PinType::Float, {2, 0, 0, 0}, "", ""});
    m.parameters.push_back({"Wind", PinType::Float2, {0.1f, 0.2f, 0, 0}, "", ""});
    const NodeId out = m.Add("Material.Output");
    const NodeId albedo = m.Add("Param.Texture:Albedo"), normal_map = m.Add("Param.Texture:NormalMap");
    const NodeId pan = m.Add("Coords.Panner");
    m.Connect(m.Add("Param.Vector:Wind"), "value", pan, "speed");
    const NodeId s1 = m.Add("Texture.Sample"), s2 = m.Add("Texture.Sample");
    m.Connect(albedo, "value", s1, "texture").Connect(pan, "uv", s1, "uv");
    m.Connect(normal_map, "value", s2, "texture").Connect(m.Add("Input.TexCoord", {{"index", 1}}), "uv", s2, "uv");
    const NodeId unpack = m.Add("Texture.NormalUnpack");
    m.Connect(s2, "rgba", unpack, "packed").Connect(unpack, "normal", out, "Normal");
    const NodeId tint_mul = m.Add("Math.Multiply");
    m.Connect(s1, "rgba", tint_mul, "a").Connect(m.Add("Param.Vector:Tint"), "value", tint_mul, "b");
    const NodeId lerp = m.Add("Math.Lerp");
    m.Connect(tint_mul, "result", lerp, "a").Connect(m.Add("Const.Float4", {{"value", {0.2f, 0.3f, 0.4f, 1}}}), "value", lerp, "b");
    const NodeId fres = m.Add("Shading.Fresnel");
    m.Connect(fres, "result", lerp, "alpha").Connect(lerp, "result", out, "BaseColor"); // float4 into float3: truncated
    // Emissive: saturate(sin(time) * glow) * normalize(world position) + abs(camera vector)
    const NodeId sin = m.Add("Math.Sin"), glow = m.Add("Math.Multiply"), sat = m.Add("Math.Saturate");
    m.Connect(m.Add("Input.Time"), "time", sin, "x").Connect(sin, "result", glow, "a").Connect(m.Add("Param.Scalar:Glow"), "value", glow, "b");
    m.Connect(glow, "result", sat, "x");
    const NodeId norm = m.Add("Math.Normalize"), scaled = m.Add("Math.Multiply"), abs = m.Add("Math.Abs"), add = m.Add("Math.Add");
    m.Connect(m.Add("Input.WorldPosition"), "position", norm, "x").Connect(norm, "result", scaled, "a").Connect(sat, "result", scaled, "b");
    m.Connect(m.Add("Input.CameraVector"), "direction", abs, "x").Connect(scaled, "result", add, "a").Connect(abs, "result", add, "b");
    m.Connect(add, "result", out, "Emissive");
    // Scalars through the rest of the math library.
    const NodeId mask = m.Add("Math.ComponentMask", {{"channels", "ga"}});
    m.Connect(s1, "rgba", mask, "x");
    const NodeId len = m.Add("Math.Length"), dot = m.Add("Math.Dot");
    m.Connect(mask, "result", len, "x").Connect(mask, "result", dot, "a").Connect(m.Add("Input.TexCoord"), "uv", dot, "b");
    NodeId chain = len;
    for (const char* op : {"Math.Sqrt", "Math.Cos", "Math.Frac", "Math.Floor", "Math.OneMinus"}) {
        const NodeId n = m.Add(op);
        m.Connect(chain, "result", n, "x");
        chain = n;
    }
    NodeId pair = chain;
    for (const char* op : {"Math.Subtract", "Math.Divide", "Math.Min", "Math.Max", "Math.Power"}) {
        const NodeId n = m.Add(op);
        m.Connect(pair, "result", n, "a").Connect(dot, "result", n, "b");
        pair = n;
    }
    const NodeId clamp = m.Add("Math.Clamp");
    m.Connect(pair, "result", clamp, "x").Connect(clamp, "result", out, "Roughness");
    const NodeId metal = m.Add("Math.ComponentMask", {{"channels", "r"}});
    m.Connect(m.Add("Const.Float", {{"value", 0.25f}}), "value", metal, "x").Connect(metal, "result", out, "Metallic");
    m.Connect(s1, "a", out, "OpacityMask");
    // A vertex-stage offset that samples a texture and appends.
    const NodeId s3 = m.Add("Texture.Sample"), append = m.Add("Math.Append");
    m.Connect(albedo, "value", s3, "texture").Connect(s3, "r", append, "a").Connect(m.Add("Const.Float2", {{"value", {0, 1}}}), "value", append, "b");
    m.Connect(append, "result", out, "WorldPositionOffset");
    return m;
}

// Compiles the include with a small pixel shader around it, when glslang is installed.
bool GlslangAvailable() {
    static const bool available = std::system("glslangValidator --version > /dev/null 2>&1") == 0;
    return available;
}
bool CompilesAsHlsl(const GeneratedMaterial& g, std::string& log) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aether_material_codegen";
    std::filesystem::create_directories(dir);
    const std::filesystem::path src = dir / "material.hlsl", out = dir / "log.txt";
    {
        std::ofstream f(src);
        f << g.hlsl
          << "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, float3 wp : TEXCOORD1) : SV_Target {\n"
             "    MaterialInputs i = (MaterialInputs)0;\n"
             "    i.uv[0] = uv; i.uv[1] = uv.yx; i.world_position = wp; i.vertex_normal = float3(0, 0, 1);\n"
             "    i.camera_vector = normalize(-wp); i.time = pos.x;\n"
             "    MaterialOutputs o = EvaluateMaterial(i);\n"
             "    float3 offset = EvaluateWorldPositionOffset(i);\n"
             "    return float4(o.base_color + o.emissive + o.normal * o.metallic * o.roughness * o.ao + offset,\n"
             "                  o.opacity * o.opacity_mask);\n"
             "}\n";
    }
    std::string defines;
    for (const std::string& d : g.defines) defines += " -D" + d;
    const std::string cmd = "glslangValidator -D -V -S frag -e main" + defines + " \"" + src.string() + "\" -o \"" +
                            (dir / "out.spv").string() + "\" > \"" + out.string() + "\" 2>&1";
    const bool ok = std::system(cmd.c_str()) == 0;
    std::ifstream l(out);
    log.assign(std::istreambuf_iterator<char>(l), std::istreambuf_iterator<char>());
    std::filesystem::remove_all(dir);
    return ok;
}

} // namespace

AETHER_TEST(MaterialCodegen_ParametersPackBy16ByteRules) {
    Material m;
    m.parameters.push_back({"A", PinType::Float, {}, "", ""});
    m.parameters.push_back({"B", PinType::Float3, {}, "", ""});
    m.parameters.push_back({"Tex", PinType::Texture, {}, "", ""});
    m.parameters.push_back({"C", PinType::Float2, {}, "", ""});
    m.parameters.push_back({"D", PinType::Float4, {}, "", ""});
    m.parameters.push_back({"E", PinType::Float, {}, "", ""});
    const ParameterLayout layout = LayoutParameters(m);
    AETHER_CHECK(layout.slots.size() == 5 && layout.size == 48);
    AETHER_CHECK(layout.Find("D")->offset == 0 && layout.Find("B")->offset == 16);
    AETHER_CHECK(layout.Find("C")->offset == 32);                 // 12 + 8 would cross into the next register
    AETHER_CHECK(layout.Find("A")->offset == 28);                 // fills B's register
    AETHER_CHECK(layout.Find("E")->offset == 40);                 // after C
    AETHER_CHECK(layout.Find("Tex") == nullptr && layout.Find("B")->size == 12);
    for (const ParameterSlot& s : layout.slots) AETHER_CHECK(s.offset / 16 == (s.offset + s.size - 1) / 16); // never straddles
    AETHER_CHECK(LayoutParameters(Material{}).size == 0);

    // Identifiers are safe and unique.
    Material names;
    names.parameters.push_back({"Base Tint", PinType::Float3, {}, "", ""});
    names.parameters.push_back({"Base_Tint", PinType::Float3, {}, "", ""});
    names.parameters.push_back({"Base-Tint", PinType::Texture, {}, "", ""});
    const ParameterLayout n = LayoutParameters(names);
    AETHER_CHECK(n.slots[0].identifier == "P_Base_Tint" && n.slots[1].identifier == "P_Base_Tint_2");
    AETHER_CHECK(HlslIdentifier("a-b c.d") == "a_b_c_d" && HlslIdentifier("") == "_");
}

AETHER_TEST(MaterialCodegen_GeneratesTheSurface) {
    const GeneratedMaterial g = GenerateHlsl(LitMaterial());
    AETHER_CHECK(g.ok && g.live_nodes == 5);
    AETHER_CHECK(Contains(g.hlsl, "float3 P_Tint : packoffset(c0.x);") && Contains(g.hlsl, "float P_Roughness : packoffset(c0.w);"));
    AETHER_CHECK(Contains(g.hlsl, "Texture2D T_Albedo : register(t0, space1);"));
    AETHER_CHECK(Contains(g.hlsl, "float4 _t1 = T_Albedo.Sample(MaterialSampler, i.uv[0]);"));
    AETHER_CHECK(Contains(g.hlsl, "float3 _t2 = (_t1.rgb * P_Tint);"));
    AETHER_CHECK(Contains(g.hlsl, "o.base_color = _t2;") && Contains(g.hlsl, "o.roughness = P_Roughness;"));
    AETHER_CHECK(Contains(g.hlsl, "o.metallic = 0.0;") && Contains(g.hlsl, "o.normal = float3(0.0, 0.0, 1.0);"));
    AETHER_CHECK(Contains(g.hlsl, "return float3(0.0, 0.0, 0.0);")); // no offset
    AETHER_CHECK(g.textures.size() == 1 && g.textures[0].identifier == "T_Albedo" && g.instructions == 2);
    AETHER_CHECK(g.features.texcoords == 1 && !g.features.time && !g.features.world_position_offset);
    AETHER_CHECK(g.defines == (std::vector<std::string>{"MATERIAL_SHADING_DEFAULT_LIT=1", "MATERIAL_BLEND_OPAQUE=1", "MATERIAL_TEXCOORDS=1"}));

    // Broadcasts, truncation and per-node defaults.
    Material m;
    const NodeId out = m.Add("Material.Output");
    const NodeId add = m.Add("Math.Add");
    m.Find(add)->defaults = {{"b", 0.25f}};
    m.Connect(m.Add("Input.Time"), "time", add, "a").Connect(add, "result", out, "BaseColor");
    m.Connect(m.Add("Const.Float4", {{"value", {1, 2, 3, 4}}}), "value", out, "Emissive");
    const GeneratedMaterial b = GenerateHlsl(m);
    AETHER_CHECK(b.ok && Contains(b.hlsl, "float _t1 = (i.time + 0.25);") && Contains(b.hlsl, "o.base_color = ((float3)(_t1));"));
    AETHER_CHECK(Contains(b.hlsl, "o.emissive = (float4(1.0, 2.0, 3.0, 4.0)).xyz;") && b.features.time && b.parameters.size == 0);
    AETHER_CHECK(!Contains(b.hlsl, "cbuffer") && !Contains(b.hlsl, "SamplerState"));

    // An invalid material doesn't generate.
    Material bad = LitMaterial();
    bad.Add("Texture.Sample");
    const GeneratedMaterial none = GenerateHlsl(bad);
    AETHER_CHECK(!none.ok && none.hlsl.empty() && none.analysis.Has("MT007"));
}

AETHER_TEST(MaterialCodegen_EliminatesDeadNodesAndSharesWork) {
    // Unconnected nodes, and pins the settings ignore, emit nothing.
    Material m = LitMaterial();
    const NodeId stray = m.Add("Math.Sin");
    m.Connect(m.Add("Input.WorldPosition"), "position", stray, "x");
    const NodeId opacity = m.Add("Math.Cos");
    m.Connect(m.Add("Input.Time"), "time", opacity, "x").Connect(opacity, "result", 1, "Opacity"); // Opaque: ignored
    GeneratedMaterial g = GenerateHlsl(m);
    AETHER_CHECK(g.ok && g.live_nodes == 5 && !Contains(g.hlsl, "sin(") && !Contains(g.hlsl, "cos(") && !g.features.world_position);
    AETHER_CHECK(Contains(g.hlsl, "o.opacity = 1.0;"));
    m.blend = BlendMode::Translucent;
    g = GenerateHlsl(m);
    AETHER_CHECK(Contains(g.hlsl, "cos(i.time)") && g.features.translucent && g.live_nodes == 7);

    // Unlit: only Emissive (the roughness parameter keeps its buffer slot).
    Material unlit = LitMaterial();
    unlit.shading = ShadingModel::Unlit;
    g = GenerateHlsl(unlit);
    AETHER_CHECK(g.ok && !Contains(g.hlsl, "o.roughness = P_Roughness") && g.textures.empty() && g.live_nodes == 0);
    AETHER_CHECK(g.parameters.Find("Roughness") != nullptr && Contains(g.defines[0], "UNLIT"));

    // Two identical multiplies, and a sample used three times: each computed once.
    Material shared;
    shared.parameters.push_back({"Tex", PinType::Texture, {}, "", ""});
    const NodeId out = shared.Add("Material.Output");
    const NodeId tex = shared.Add("Param.Texture:Tex"), sample = shared.Add("Texture.Sample");
    shared.Connect(tex, "value", sample, "texture");
    const NodeId m1 = shared.Add("Math.Multiply"), m2 = shared.Add("Math.Multiply");
    for (const NodeId n : {m1, m2}) shared.Connect(sample, "r", n, "a").Connect(sample, "g", n, "b");
    shared.Connect(m1, "result", out, "Metallic").Connect(m2, "result", out, "Roughness").Connect(sample, "rgb", out, "BaseColor");
    g = GenerateHlsl(shared);
    AETHER_CHECK(g.ok && g.instructions == 2 && Count(g.hlsl, ".Sample(") == 1 && Count(g.hlsl, " * ") == 1);
    AETHER_CHECK(Contains(g.hlsl, "o.metallic = _t2;") && Contains(g.hlsl, "o.roughness = _t2;") && g.live_nodes == 4);
}

AETHER_TEST(MaterialCodegen_VertexStageAndPermutationKeys) {
    const GeneratedMaterial g = GenerateHlsl(KitchenSink());
    AETHER_CHECK(g.ok);
    for (const Diagnostic& d : g.analysis.diagnostics) AETHER_CHECK(d.severity == Severity::Warning);
    AETHER_CHECK(Contains(g.hlsl, "T_Albedo.SampleLevel(MaterialSampler, i.uv[0], 0)")); // the vertex stage has no derivatives
    AETHER_CHECK(Contains(g.hlsl, "T_Albedo.Sample(MaterialSampler, _t"));                // the panned one
    AETHER_CHECK(Contains(g.hlsl, "T_NormalMap.Sample(MaterialSampler, i.uv[1])"));
    AETHER_CHECK(g.textures.size() == 2 && g.textures[1].name == "NormalMap" && g.textures[1].slot == 1); // Unused has no slot
    AETHER_CHECK(g.features.texcoords == 2 && g.features.time && g.features.world_position && g.features.camera_vector);
    AETHER_CHECK(g.features.vertex_normal && g.features.world_position_offset && g.features.alpha_test);
    AETHER_CHECK(std::find(g.defines.begin(), g.defines.end(), "MATERIAL_OPACITY_MASK_CLIP=0.3333") != g.defines.end());
    AETHER_CHECK(std::find(g.defines.begin(), g.defines.end(), "MATERIAL_WORLD_POSITION_OFFSET=1") != g.defines.end());

    // The key follows the code and settings, not the layout or parameter values.
    Material m = KitchenSink();
    const u64 key = GenerateHlsl(m).permutation_key;
    AETHER_CHECK(key == g.permutation_key && key != 0);
    m.nodes[3].x += 100;
    m.parameters[3].default_value = Vec4(0, 0, 0, 1);
    AETHER_CHECK(GenerateHlsl(m).permutation_key == key);
    m.two_sided = true;
    AETHER_CHECK(GenerateHlsl(m).permutation_key != key);
    m.two_sided = false;
    m.nodes.back().config["value"] = {0, 2};
    AETHER_CHECK(GenerateHlsl(m).permutation_key != key);
    AETHER_CHECK(GenerateHlsl(LitMaterial()).permutation_key != key);
}

AETHER_TEST(MaterialCodegen_OutputCompilesWithGlslang) {
    if (!GlslangAvailable()) {
        std::printf("  (glslangValidator not found: skipping the HLSL compile check)\n");
        return;
    }
    Material unlit = LitMaterial();
    unlit.shading = ShadingModel::Unlit;
    Material translucent = KitchenSink();
    translucent.blend = BlendMode::Translucent;
    Material empty;
    empty.Add("Material.Output");
    for (const Material& m : {LitMaterial(), KitchenSink(), unlit, translucent, empty}) {
        const GeneratedMaterial g = GenerateHlsl(m);
        std::string log;
        AETHER_CHECK(g.ok && CompilesAsHlsl(g, log));
        if (std::getenv("AETHER_DUMP_HLSL") || Contains(log, "ERROR")) std::printf("%s\n%s\n", g.hlsl.c_str(), log.c_str());
    }
}
