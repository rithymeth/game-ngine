#pragma once

#include "aether/core/base.h"

#include <string>

namespace aether::player {

class GamePackage;

// What the player makes of a material graph (.amat) in the package: read, checked, and turned into the shader the renderer's passes would call. The
// player does not draw with the graph yet; this says whether it could, and what it needs, so the editor can tell the person which materials are drawn
// and which are not, instead of showing the model's own surface under a graph's name without a word.
struct GraphMaterialReport {
    std::string path;
    bool found = false;     // the package has the file
    bool compiles = false;  // valid graph, shader generated
    bool drawable = false;  // the renderer can draw with it: an opaque, default-lit graph of constants and parameters (no textures yet)
    std::string message;    // why not, or a one-line summary
    u32 live_nodes = 0;     // nodes that reach an output the material uses
    u32 instructions = 0;   // locals the shader computes (a rough cost)
    u32 textures = 0;       // textures the graph samples
    u32 parameters = 0;     // bytes of non-texture parameters, in the constant buffer
    u64 permutation_key = 0; // equal keys are equal shaders
};

// Reads `path` (a .amat under Content/) from the package, checks it and generates its shader. A .amati (an instance of a graph) is reported as not supported yet.
// When the graph is drawable, `shader` (if given) gets the HLSL the scene shader includes: the graph's defines and EvaluateMaterial, with its parameters as
// constants (so an edit to the graph is a new shader, not a changed value).
GraphMaterialReport CompileGraphMaterial(const GamePackage& package, const std::string& path, std::string* shader = nullptr);

} // namespace aether::player
