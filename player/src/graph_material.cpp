#include "aether/player/graph_material.h"

#include "aether/player/game.h"
#include "aether/renderer/material.h"
#include "aether/renderer/material_codegen.h"
#include "aether/renderer/material_instance.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <optional>
#include <vector>

namespace aether::player {
namespace {

// A float as HLSL source that stays a float ("1" would be an int).
std::string FloatLiteral(f32 value) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
    std::string out = text;
    if (out.find_first_of(".eEn") == std::string::npos) out += ".0";
    return out;
}

u32 ComponentCount(mat::PinType type) {
    switch (type) {
    case mat::PinType::Float2: return 2;
    case mat::PinType::Float3: return 3;
    case mat::PinType::Float4: return 4;
    default: return 1;
    }
}

const char* HlslType(mat::PinType type) {
    switch (type) {
    case mat::PinType::Float2: return "float2";
    case mat::PinType::Float3: return "float3";
    case mat::PinType::Float4: return "float4";
    default: return "float";
    }
}

std::string Constructor(mat::PinType type, const std::vector<std::string>& parts) {
    if (type == mat::PinType::Float || parts.size() == 1) return parts[0];
    std::string out = std::string(HlslType(type)) + "(";
    for (usize i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    return out + ")";
}

// The push-constant words a graph's parameters ride in: a draw's own colour (two), emissive (two) and factor (two) words, which a graph's draw does not read.
const char* const kWordNames[6] = {"g_PC.g_ColorPacked.x", "g_PC.g_ColorPacked.y", "g_PC.g_Emissive.x", "g_PC.g_Emissive.y", "g_PC.g_MaterialFactors0", "g_PC.g_MaterialFactors1"};

// The generated include with its constant buffer replaced: parameters that fit ride in the draw's push constants (macros reading them), the rest become constants
// holding the resolved values; and its defines written out ahead of it.
std::string Bake(const GamePackage& package, const mat::GeneratedMaterial& generated, const mat::ResolvedParameters& values, GraphBuild& build,
                 std::vector<std::string>& fixed) {
    std::string out;
    for (const std::string& define : generated.defines) {
        const usize equals = define.find('=');
        out += "#define " + (equals == std::string::npos ? define : define.substr(0, equals) + " " + define.substr(equals + 1)) + "\n";
    }
    std::string declarations;
    u32 next_word = 0;
    for (const mat::ParameterSlot& slot : generated.parameters.slots) {
        const mat::ResolvedParameter* resolved = values.Find(slot.name);
        const Vec4 v = resolved != nullptr ? resolved->value : Vec4(0, 0, 0, 0);
        const f32 components[4] = {v.x, v.y, v.z, v.w};
        const u32 count = ComponentCount(slot.type);
        if (next_word + count <= 6) {
            std::vector<std::string> parts;
            for (u32 c = 0; c < count; ++c) {
                parts.push_back(std::string("asfloat(") + kWordNames[next_word + c] + ")");
                build.words[next_word + c] = components[c];
            }
            declarations += "#define " + slot.identifier + " " + Constructor(slot.type, parts) + "\n";
            build.pushed.push_back({slot.name, next_word, count});
            next_word += count;
        } else {
            std::vector<std::string> parts;
            for (u32 c = 0; c < count; ++c) parts.push_back(FloatLiteral(components[c]));
            declarations += std::string("static const ") + HlslType(slot.type) + " " + slot.identifier + " = " + Constructor(slot.type, parts) + ";\n";
            fixed.push_back(slot.name);
        }
    }
    std::string hlsl = generated.hlsl;
    const usize begin = hlsl.find("cbuffer MaterialParams");
    if (begin != std::string::npos) {
        const usize end = hlsl.find("};", begin);
        if (end != std::string::npos) hlsl.replace(begin, end + 2 - begin, declarations);
    }
    if (!generated.textures.empty()) {
        // The sampler and the texture table are the scene's; a texture is the slot a draw passes in. Plain functions, not methods of a texture wrapper: the compiler
        // turns a method called on a constant struct into invalid SPIR-V.
        const usize sampler = hlsl.find("SamplerState MaterialSampler");
        if (sampler != std::string::npos) hlsl.erase(sampler, hlsl.find('\n', sampler) + 1 - sampler);
        const std::string helpers =
            "float4 GraphSample(uint slot, float2 uv) { return g_Textures[min((g_PC.g_MaterialTextures >> (8u * slot)) & 255u, 127u)].Sample(g_Sampler, uv); }\n"
            "float4 GraphSampleLevel(uint slot, float2 uv, float level) { return g_Textures[min((g_PC.g_MaterialTextures >> (8u * slot)) & 255u, 127u)].SampleLevel(g_Sampler, uv, level); }\n";
        const auto replace_all = [&](const std::string& from, const std::string& to) {
            for (usize at = hlsl.find(from); at != std::string::npos; at = hlsl.find(from, at + to.size())) hlsl.replace(at, from.size(), to);
        };
        for (const mat::TextureSlot& slot : generated.textures) {
            const mat::ResolvedParameter* resolved = values.Find(slot.name);
            std::string content_path;
            if (resolved != nullptr && !resolved->texture.empty()) {
                if (const GameManifest::Asset* asset = package.FindAssetByGuid(resolved->texture)) content_path = asset->path;
            }
            build.texture_paths.push_back(content_path);
            const std::string number = std::to_string(slot.slot) + "u";
            replace_all(slot.identifier + ".Sample(MaterialSampler, ", "GraphSample(" + number + ", ");
            replace_all(slot.identifier + ".SampleLevel(MaterialSampler, ", "GraphSampleLevel(" + number + ", ");
            const std::string declaration = "Texture2D " + slot.identifier + " :";
            const usize at = hlsl.find(declaration);
            if (at != std::string::npos) hlsl.erase(at, hlsl.find('\n', at) + 1 - at);
        }
        const usize anchor = hlsl.find("struct MaterialInputs");
        hlsl.insert(anchor == std::string::npos ? 0 : anchor, helpers + "\n");
    }
    return out + hlsl;
}

} // namespace

GraphMaterialReport CompileGraphMaterial(const GamePackage& package, const std::string& path, GraphBuild* build_out) {
    GraphMaterialReport report;
    report.path = path;
    mat::InstanceChain chain;
    std::string error;
    bool missing = false;
    const mat::AssetReader reader = [&](const std::string& file) -> std::optional<nlohmann::json> {
        std::vector<u8> bytes;
        if (!package.ReadContent(file, bytes)) {
            if (file == path) missing = true;
            return std::nullopt;
        }
        nlohmann::json json = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
        if (json.is_discarded()) return nlohmann::json("not json"); // a string: not a material or an instance, which the chain says
        return json;
    };
    if (!mat::LoadInstanceChain(path, reader, chain, &error)) {
        if (missing) {
            report.message = "Not in the package.";
        } else {
            report.found = true;
            report.message = "Not a readable material graph" + (error.empty() ? std::string() : ": " + error);
        }
        return report;
    }
    report.found = true;
    const mat::Material& material = chain.material;
    const mat::GeneratedMaterial generated = mat::GenerateHlsl(material);
    if (!generated.ok) {
        report.message = "The graph has errors";
        for (const mat::Diagnostic& diagnostic : generated.analysis.diagnostics) {
            if (diagnostic.severity == mat::Severity::Error) {
                report.message += ": " + diagnostic.message;
                break;
            }
        }
        return report;
    }
    report.compiles = true;
    report.live_nodes = generated.live_nodes;
    report.instructions = generated.instructions;
    report.textures = static_cast<u32>(generated.textures.size());
    report.parameters = generated.parameters.size;
    report.permutation_key = generated.permutation_key;
    const std::string summary = "Compiles: " + std::to_string(generated.live_nodes) + " nodes, " + std::to_string(generated.textures.size()) + " textures";
    // What the renderer can draw so far: an opaque, default-lit graph of constants, parameters and textures (no triplanar sampling, no vertex offset).
    if (material.shading != mat::ShadingModel::DefaultLit || material.blend != mat::BlendMode::Opaque) {
        report.message = summary + "; not drawn yet (only an opaque, default-lit graph is).";
    } else if (generated.textures.size() > 4) {
        report.message = summary + "; not drawn yet (more than four textures are not).";
    } else if (generated.hlsl.find("AetherTriplanar") != std::string::npos) {
        report.message = summary + "; not drawn yet (triplanar sampling is not).";
    } else if (generated.features.world_position_offset) {
        report.message = summary + "; not drawn yet (a graph that moves vertices is not).";
    } else {
        report.drawable = true;
        report.message = summary + "; drawn.";
        if (build_out != nullptr) {
            const mat::ResolvedParameters values = mat::ResolveParameters(material, chain.Pointers());
            GraphBuild build;
            build.shader = Bake(package, generated, values, build, report.fixed_parameters);
            *build_out = std::move(build);
        } else {
            GraphBuild scratch; // the report names the parameters that are held fixed, which is decided by the same packing
            const mat::ResolvedParameters values = mat::ResolveParameters(material, chain.Pointers());
            Bake(package, generated, values, scratch, report.fixed_parameters);
        }
    }
    return report;
}

} // namespace aether::player
