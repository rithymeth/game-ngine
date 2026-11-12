#pragma once

#include "aether/core/base.h"

#include <string>
#include <vector>

namespace aether::player {

class GamePackage;

// What the player makes of a material graph (.amat), or an instance of one (.amati), in the package: read, checked, and turned into the shader the renderer's
// passes call. This says whether the renderer can draw with it and what it needs, so the editor can tell the person which materials are drawn and which are not,
// instead of showing the model's own surface under a graph's name without a word.
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
    std::vector<std::string> fixed_parameters; // parameters the shader holds as constants: a draw cannot give them its own value (see GraphBuild)
};

// How a drawable graph (or instance) is handed to the renderer.
//  - `shader`: the HLSL the scene shader includes (the graph's defines and EvaluateMaterial).
//  - Textures are sampled through the scene's bindless table by the slots a draw passes in its push constants (the four 8-bit fields a model's own textures use,
//    so at most four): `texture_paths` is the content path of each texture's value in slot order (empty for none), for the renderer to load.
//  - Up to six 32-bit floats of parameters travel in the push constants of every draw (the fields a model's own colour, emissive and factors use, which a graph
//    does not read), so one shader serves every value: `pushed` says which parameter each word run holds, and `words` are the values the graph or instance
//    gives them. A draw may replace them by name. Parameters that do not fit are held in the shader as constants (`fixed_parameters` in the report).
struct GraphBuild {
    struct Pushed {
        std::string name;
        u32 first = 0;  // the first of its words
        u32 count = 0;  // 1 to 4
    };
    std::string shader;
    std::vector<std::string> texture_paths;
    std::vector<Pushed> pushed;
    f32 words[6] = {0, 0, 0, 0, 0, 0};
};

// Reads `path` from the package (a .amat, or a .amati and the chain of parents above it), checks it and generates its shader. `build` is filled only for a
// graph the renderer can draw.
GraphMaterialReport CompileGraphMaterial(const GamePackage& package, const std::string& path, GraphBuild* build = nullptr);

} // namespace aether::player
