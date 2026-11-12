#include "aether/player/graph_material.h"

#include "aether/player/game.h"
#include "aether/renderer/material.h"
#include "aether/renderer/material_codegen.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
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

std::string ParameterLiteral(mat::PinType type, const Vec4& v) {
    switch (type) {
    case mat::PinType::Float2: return "float2(" + FloatLiteral(v.x) + ", " + FloatLiteral(v.y) + ")";
    case mat::PinType::Float3: return "float3(" + FloatLiteral(v.x) + ", " + FloatLiteral(v.y) + ", " + FloatLiteral(v.z) + ")";
    case mat::PinType::Float4: return "float4(" + FloatLiteral(v.x) + ", " + FloatLiteral(v.y) + ", " + FloatLiteral(v.z) + ", " + FloatLiteral(v.w) + ")";
    default: return FloatLiteral(v.x);
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

// The generated include with its constant buffer replaced by constants holding the graph's own default values, and its defines written out ahead of it.
std::string BakeParameters(const mat::Material& material, const mat::GeneratedMaterial& generated) {
    std::string out;
    for (const std::string& define : generated.defines) {
        const usize equals = define.find('=');
        out += "#define " + (equals == std::string::npos ? define : define.substr(0, equals) + " " + define.substr(equals + 1)) + "\n";
    }
    std::string constants;
    for (const mat::ParameterSlot& slot : generated.parameters.slots) {
        const mat::Parameter* parameter = material.FindParameter(slot.name);
        constants += std::string("static const ") + HlslType(slot.type) + " " + slot.identifier + " = " +
                     ParameterLiteral(slot.type, parameter != nullptr ? parameter->default_value : Vec4(0, 0, 0, 0)) + ";\n";
    }
    std::string hlsl = generated.hlsl;
    const usize begin = hlsl.find("cbuffer MaterialParams");
    if (begin != std::string::npos) {
        const usize end = hlsl.find("};", begin);
        if (end != std::string::npos) hlsl.replace(begin, end + 2 - begin, constants);
    }
    return out + hlsl;
}

} // namespace

GraphMaterialReport CompileGraphMaterial(const GamePackage& package, const std::string& path, std::string* shader) {
    GraphMaterialReport report;
    report.path = path;
    const auto ends_with = [&](const char* suffix) {
        const std::string s(suffix);
        return path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    if (ends_with(".amati")) {
        report.message = "A material instance is not drawn yet (only a graph, .amat, is read).";
        return report;
    }
    std::vector<u8> bytes;
    if (!package.ReadContent(path, bytes)) {
        report.message = "Not in the package.";
        return report;
    }
    report.found = true;
    const nlohmann::json json = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, /*allow_exceptions=*/false);
    mat::Material material;
    std::string error;
    if (json.is_discarded() || !mat::MaterialFromJson(json, material, &error)) {
        report.message = "Not a readable material graph" + (error.empty() ? std::string() : ": " + error);
        return report;
    }
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
    // What the renderer can draw so far: an opaque, default-lit graph of constants and parameters (no textures, no vertex offset).
    if (material.shading != mat::ShadingModel::DefaultLit || material.blend != mat::BlendMode::Opaque) {
        report.message = summary + "; not drawn yet (only an opaque, default-lit graph is).";
    } else if (!generated.textures.empty()) {
        report.message = summary + "; not drawn yet (a graph that samples textures is not).";
    } else if (generated.features.world_position_offset) {
        report.message = summary + "; not drawn yet (a graph that moves vertices is not).";
    } else {
        report.drawable = true;
        report.message = summary + "; drawn.";
        if (shader != nullptr) *shader = BakeParameters(material, generated);
    }
    return report;
}

} // namespace aether::player
