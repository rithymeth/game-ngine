#pragma once

#include "aether/core/base.h"

#include <vector>
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
    bool drawable = false;  // the renderer can draw with it: an opaque, default-lit graph of constants, parameters and textures (no triplanar sampling yet)
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
// A graph's texture parameters are sampled through the scene's bindless texture table, by the slots the draw passes in its push constants (the same four 8-bit
// fields a model's own textures use, so at most four): `texture_paths`, if given, gets the content path of each texture's default in slot order (empty for a
// texture with none), for the renderer to load and pass.
GraphMaterialReport CompileGraphMaterial(const GamePackage& package, const std::string& path, std::string* shader = nullptr, std::vector<std::string>* texture_paths = nullptr);

} // namespace aether::player
