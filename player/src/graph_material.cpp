#include "aether/player/graph_material.h"

#include "aether/player/game.h"
#include "aether/renderer/material.h"
#include "aether/renderer/material_codegen.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace aether::player {

GraphMaterialReport CompileGraphMaterial(const GamePackage& package, const std::string& path) {
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
    report.message = "Compiles: " + std::to_string(generated.live_nodes) + " nodes, " + std::to_string(generated.textures.size()) + " textures; not drawn yet.";
    return report;
}

} // namespace aether::player
