#pragma once

#include "aether/renderer/material.h"

#include <string>
#include <vector>

namespace aether::mat {

// HLSL generation (Phase 15 step 2, docs/design/PHASE_SPECS.md §15.3): a
// valid material graph becomes a shader include that the renderer's passes
// call. Only nodes that reach a pin the material's settings use are
// emitted, identical expressions are computed once, and parameters are
// packed into a constant buffer by HLSL's 16-byte rules.

// Where a parameter lives in the MaterialParams constant buffer.
struct ParameterSlot {
    std::string name;       // the parameter's name
    std::string identifier; // its HLSL name: "P_Base_Tint"
    PinType type = PinType::Float;
    u32 offset = 0;         // bytes from the start of the buffer
    u32 size = 0;           // 4, 8, 12 or 16
};
struct TextureSlot {
    std::string name, identifier; // "Albedo", "T_Albedo"
    u32 slot = 0;                 // register(tN, space1)
};

// The buffer layout for every non-texture parameter, in the order that
// packs tightest: widest first, each into the first gap it fits without
// crossing a 16-byte boundary. It depends only on the parameters, not on
// the graph, so instances (step 3) share it.
struct ParameterLayout {
    std::vector<ParameterSlot> slots;
    u32 size = 0; // bytes, a multiple of 16 (0 when there are no parameters)
    const ParameterSlot* Find(const std::string& name) const;
};
ParameterLayout LayoutParameters(const Material& material);

// What a material's shader needs from the pass, so passes compile only the
// interpolators and features it uses.
struct MaterialFeatures {
    u32 texcoords = 0; // how many UV sets (0..4)
    bool world_position = false;
    bool vertex_normal = false;
    bool camera_vector = false;
    bool time = false;
    bool world_position_offset = false; // the vertex stage adds an offset
    bool alpha_test = false;            // Masked
    bool translucent = false;           // Translucent or Additive
    bool operator==(const MaterialFeatures& o) const;
};

struct GeneratedMaterial {
    bool ok = false;
    Analysis analysis;        // the diagnostics when it failed
    std::string hlsl;         // the include: resources, EvaluateMaterial, EvaluateWorldPositionOffset
    std::vector<std::string> defines; // "MATERIAL_BLEND_MASKED=1", ...
    ParameterLayout parameters;
    std::vector<TextureSlot> textures; // only the textures the graph samples
    MaterialFeatures features;
    // Identifies the compiled shader: equal keys mean equal code and
    // settings, so the permutation cache (step 6) can share them.
    u64 permutation_key = 0;
    u32 live_nodes = 0;   // nodes that reach a used output
    u32 instructions = 0; // locals emitted (a rough cost, for the stats panel)
};
GeneratedMaterial GenerateHlsl(const Material& material);

// A name made safe for HLSL: letters, digits and underscores.
std::string HlslIdentifier(const std::string& name);

} // namespace aether::mat
