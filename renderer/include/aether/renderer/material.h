#pragma once

#include "aether/math/math.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace aether::mat {

// Materials (Phase 15 step 1, docs/design/PHASE_SPECS.md §15): node graphs
// with typed pins that compile to HLSL. This is the model, the node
// library, type inference and validation.

enum class ShadingModel : u8 { DefaultLit, Unlit };
enum class BlendMode : u8 { Opaque, Masked, Translucent, Additive };

// Pin types. Generic math pins are Any until inference gives them a type.
enum class PinType : u8 { None, Float, Float2, Float3, Float4, Texture, Any };
const char* TypeName(PinType type); // "float", "float2", ..., "texture"
std::optional<PinType> ParseType(const std::string& name);
u32 Components(PinType type); // 1..4, 0 for texture/none
PinType FloatN(u32 components);

struct Parameter {
    std::string name;
    PinType type = PinType::Float; // Float..Float4 or Texture
    Vec4 default_value{0, 0, 0, 0};
    std::string texture; // Texture: the default texture's asset GUID (text)
    std::string group;
};

using NodeId = u32;
struct PinRef {
    NodeId node = 0;
    std::string pin;
    bool operator==(const PinRef& o) const { return node == o.node && pin == o.pin; }
};
struct Link {
    PinRef from, to; // an output to an input
};
struct Node {
    NodeId id = 0;
    std::string type; // "Math.Multiply", "Param.Scalar:Roughness", "Material.Output"
    f32 x = 0, y = 0;
    nlohmann::json config = nlohmann::json::object();   // Constant values, TexCoord index, ComponentMask channels
    nlohmann::json defaults = nlohmann::json::object(); // unconnected inputs' constants: pin -> number or [x, y, z, w]
};

struct Material {
    ShadingModel shading = ShadingModel::DefaultLit;
    BlendMode blend = BlendMode::Opaque;
    bool two_sided = false;
    f32 opacity_mask_clip = 0.3333f; // Masked: pixels below this are discarded
    std::vector<Parameter> parameters;
    std::vector<Node> nodes;
    std::vector<Link> links;

    const Parameter* FindParameter(const std::string& name) const;
    const Node* Find(NodeId id) const;
    Node* Find(NodeId id);
    NodeId NextId() const;
    NodeId Add(const std::string& type, nlohmann::json config = nlohmann::json::object(), f32 x = 0, f32 y = 0);
    Material& Connect(NodeId from, const std::string& from_pin, NodeId to, const std::string& to_pin);
};

// --- Node library ------------------------------------------------------------------
struct PinDesc {
    std::string name;
    PinType type = PinType::Float;
    Vec4 default_value{0, 0, 0, 0}; // inputs: used when unconnected
    bool required = false;          // must be connected (textures)
    // Unconnected, it's a per-pixel value instead of a constant: "uv0" (the
    // first texture coordinate), "normal" (the vertex normal).
    std::string implicit;
};
struct NodeSignature {
    std::string title;
    std::string category;
    std::vector<PinDesc> inputs, outputs;
    // Generic nodes: every Any pin (inputs and outputs) takes the widest
    // connected input type; a Float input broadcasts to it.
    bool generic = false;
    const PinDesc* Input(const std::string& name) const;
    const PinDesc* Output(const std::string& name) const;
};
// A node's pins from its type and config, or nullopt with a message.
std::optional<NodeSignature> ResolveNode(const Material& material, const Node& node, std::string* error = nullptr);
struct PaletteEntry {
    std::string id, title, category;
};
std::vector<PaletteEntry> ListNodeTypes(const Material& material); // parameters included

// --- Validation and types ------------------------------------------------------------
enum class Severity : u8 { Error, Warning };
struct Diagnostic {
    std::string code; // MT001...
    Severity severity = Severity::Error;
    NodeId node = 0;
    std::string pin;
    std::string message;
};
struct Analysis {
    std::vector<Diagnostic> diagnostics;
    usize errors = 0;
    NodeId output = 0;
    // Each connected output pin's inferred type.
    std::vector<std::pair<PinRef, PinType>> types;
    bool Ok() const { return errors == 0; }
    bool Has(const std::string& code) const;
    PinType TypeOf(const PinRef& output_pin) const;
};
// MT001 no Material Output / more than one; MT002 unknown node type or bad
// config; MT003 incompatible types on a link; MT004 a link to a pin that
// doesn't exist; MT005 cycle; MT006 unknown parameter; MT007 required input
// unconnected; MT008 input with two links; MT009 (warning) output pin the
// material's settings ignore (Opacity on an Opaque material).
Analysis Analyze(const Material& material);

// Whether an output of `from` can feed an input of `to`: same type, a
// float broadcast, or truncation to fewer components (with a warning, MT010).
enum class Fit : u8 { No, Yes, Broadcast, Truncate };
Fit CanConnect(PinType from, PinType to);

// --- Files ---------------------------------------------------------------------------
nlohmann::json MaterialToJson(const Material& material);
bool MaterialFromJson(const nlohmann::json& json, Material& out, std::string* error = nullptr);
bool SaveMaterial(const Material& material, const std::filesystem::path& path, std::string* error = nullptr);
bool LoadMaterial(const std::filesystem::path& path, Material& out, std::string* error = nullptr);

} // namespace aether::mat
