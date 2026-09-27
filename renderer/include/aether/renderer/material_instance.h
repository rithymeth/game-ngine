#pragma once

#include "aether/renderer/material_codegen.h"
#include "aether/reflection/reflection.h"

#include <functional>
#include <optional>

namespace aether::mat {

// Material instances (Phase 15 step 3, docs/design/PHASE_SPECS.md §15.4):
// parameter overrides on a parent material, or on another instance, that
// share the parent's compiled shader. Changing a value only repacks the
// parameter buffer; nothing is recompiled.

struct ParameterOverride {
    std::string name;
    Vec4 value{0, 0, 0, 0}; // scalar and vector parameters
    std::string texture;    // texture parameters: an asset GUID (text)
    bool is_texture = false;
};

struct MaterialInstance {
    std::string parent; // an .amat, or another .amati
    std::vector<ParameterOverride> overrides;

    const ParameterOverride* Find(const std::string& name) const;
    void SetValue(const std::string& name, const Vec4& value);
    void SetTexture(const std::string& name, const std::string& texture);
    bool Clear(const std::string& name); // false if it wasn't overridden
};

nlohmann::json InstanceToJson(const MaterialInstance& instance);
bool InstanceFromJson(const nlohmann::json& json, MaterialInstance& out, std::string* error = nullptr);
bool SaveInstance(const MaterialInstance& instance, const std::filesystem::path& path, std::string* error = nullptr);
bool LoadInstance(const std::filesystem::path& path, MaterialInstance& out, std::string* error = nullptr);

// --- Resolution --------------------------------------------------------------------
struct ResolvedParameter {
    std::string name;
    PinType type = PinType::Float;
    Vec4 value{0, 0, 0, 0};
    std::string texture;
    u32 source = 0; // 0: the material's default; n: the nth instance in the chain
};
struct ResolvedParameters {
    std::vector<ResolvedParameter> values; // in the material's declaration order
    const ResolvedParameter* Find(const std::string& name) const;
};
// The material's defaults with each instance's overrides applied in turn
// (parent first). Overrides that don't fit are skipped and reported: MI001
// no such parameter; MI002 a value for a texture or a texture for a value
// (warnings, so a renamed parameter doesn't break every instance).
ResolvedParameters ResolveParameters(const Material& material, const std::vector<const MaterialInstance*>& chain,
                                     std::vector<Diagnostic>* diagnostics = nullptr);

// Parent chains: reads each asset (a JSON document) through `read`, up to
// the material at the root. MI003 the chain loops; MI004 a parent is
// missing or isn't a material or instance; MI005 deeper than 16.
using AssetReader = std::function<std::optional<nlohmann::json>(const std::string& path)>;
struct InstanceChain {
    std::string material_path;
    Material material;
    std::vector<MaterialInstance> instances; // parent first; the asked-for instance last
    std::vector<const MaterialInstance*> Pointers() const;
};
bool LoadInstanceChain(const std::string& path, const AssetReader& read, InstanceChain& out, std::string* error = nullptr);

// The MaterialParams buffer's bytes (layout.size of them) and the texture
// GUID bound to each slot.
std::vector<u8> PackParameters(const ParameterLayout& layout, const ResolvedParameters& values);
std::vector<std::string> TextureBindings(const std::vector<TextureSlot>& slots, const ResolvedParameters& values);

// A runtime set of parameter values over a compiled material (Unreal's
// dynamic material instance): set values freely; the buffer is repacked on
// the next read, and Revision() tells the renderer to upload it again.
class ParameterBlock {
public:
    ParameterBlock() = default;
    ParameterBlock(ParameterLayout layout, std::vector<TextureSlot> textures, ResolvedParameters base);

    bool SetScalar(const std::string& name, f32 value);
    bool SetVector(const std::string& name, const Vec4& value);
    bool SetTexture(const std::string& name, const std::string& texture);
    bool Reset(const std::string& name); // back to the base value
    void ResetAll();
    std::optional<Vec4> Value(const std::string& name) const;
    const ResolvedParameters& Current() const { return current_; }

    const std::vector<u8>& Buffer() const;
    const std::vector<std::string>& Textures() const;
    u64 Revision() const { return revision_; }

private:
    ResolvedParameter* Find(const std::string& name);
    void Changed();

    ParameterLayout layout_;
    std::vector<TextureSlot> texture_slots_;
    ResolvedParameters base_, current_;
    u64 revision_ = 0;
    mutable bool dirty_ = true;
    mutable std::vector<u8> buffer_;
    mutable std::vector<std::string> textures_;
};

} // namespace aether::mat

namespace aether {

// Per-entity material overrides, set from the editor or from Blueprints
// (Set Scalar Parameter, ...). The renderer applies them over the mesh's
// material; `revision` changes with every edit.
struct MaterialParameterValue {
    std::string name;
    Vec4 value{0, 0, 0, 0};
    std::string texture;
    bool is_texture = false;
};
struct MaterialParameters {
    std::string material; // the .amat or .amati the mesh draws with ("": the model's own)
    std::vector<MaterialParameterValue> values;
    u32 revision = 0;

    void SetScalarParameter(const std::string& name, f32 value);
    void SetVectorParameter(const std::string& name, Vec3 value, f32 alpha);
    void SetTextureParameter(const std::string& name, const std::string& texture);
    f32 GetScalarParameter(const std::string& name) const;  // the override, or 0
    Vec3 GetVectorParameter(const std::string& name) const; // the override, or 0
    bool ClearParameter(const std::string& name);
    // Applies the overrides to a block; the ones that don't fit are skipped.
    void ApplyTo(mat::ParameterBlock& block) const;
};

} // namespace aether

AETHER_REFLECT(aether::MaterialParameterValue, 1,
    AETHER_FIELD(name, Field_EditAnywhere),
    AETHER_FIELD(value, Field_EditAnywhere),
    AETHER_FIELD(texture, Field_EditAnywhere, {.tooltip = "Texture parameters: the texture's asset GUID"}),
    AETHER_FIELD(is_texture, Field_EditAnywhere)
)
AETHER_REFLECT(aether::MaterialParameters, 1,
    AETHER_FIELD(material, Field_EditAnywhere, {.tooltip = "The .amat or .amati to draw with (empty: the model's own)"}),
    AETHER_FIELD(values, Field_EditAnywhere),
    AETHER_FIELD(revision, Field_None),
    AETHER_METHOD(SetScalarParameter, Fn_BlueprintCallable, {"name", "value"}),
    AETHER_METHOD(SetVectorParameter, Fn_BlueprintCallable, {"name", "value", "alpha"}),
    AETHER_METHOD(SetTextureParameter, Fn_BlueprintCallable, {"name", "texture"}),
    AETHER_METHOD(GetScalarParameter, Fn_BlueprintCallable | Fn_Pure, {"name"}),
    AETHER_METHOD(GetVectorParameter, Fn_BlueprintCallable | Fn_Pure, {"name"}),
    AETHER_METHOD(ClearParameter, Fn_BlueprintCallable, {"name"})
)
