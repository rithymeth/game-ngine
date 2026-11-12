#pragma once

#include "aether/core/base.h"
#include "aether/renderer/material_instance.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace aether::player {

// The player draws a model with the surface its glTF gives it. A `MaterialParameters` component on the entity (the one the inspector, Blueprints'
// Set Scalar / Vector / Texture Parameter and Luau already write) changes that surface for this entity alone, by these names:
//   BaseColor         a vector (rgb) and alpha: multiplies nothing, it replaces the model's colour factor
//   Metallic          a scalar 0..1
//   Roughness         a scalar 0..1
//   Emissive          a vector (rgb, linear, 0 up): replaces the emissive colour
//   BaseColorTexture  a texture parameter: the content path of an image in the package, drawn in place of the model's own base colour texture
// A name the entity does not set leaves the model's value; a value that is not a finite number is ignored (not clamped to something else).
inline constexpr const char* kOverrideBaseColor = "BaseColor";
inline constexpr const char* kOverrideMetallic = "Metallic";
inline constexpr const char* kOverrideRoughness = "Roughness";
inline constexpr const char* kOverrideEmissive = "Emissive";
inline constexpr const char* kOverrideBaseColorTexture = "BaseColorTexture";

struct SurfaceValues {
    f32 color[4] = {1, 1, 1, 1};
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    f32 emissive[3] = {0, 0, 0};
};

// Applies the overrides to `surface`. Returns the content path of a base colour texture override (null: the model's own), pointing into `overrides`.
inline const std::string* ApplyMaterialOverrides(const MaterialParameters& overrides, SurfaceValues& surface) {
    const std::string* texture = nullptr;
    const auto finite = [](f32 value) { return std::isfinite(value); };
    for (const MaterialParameterValue& entry : overrides.values) {
        if (entry.is_texture) {
            if (entry.name == kOverrideBaseColorTexture && !entry.texture.empty()) texture = &entry.texture;
            continue;
        }
        const Vec4& v = entry.value;
        if (entry.name == kOverrideBaseColor) {
            if (finite(v.x) && finite(v.y) && finite(v.z) && finite(v.w)) {
                surface.color[0] = std::max(v.x, 0.0f);
                surface.color[1] = std::max(v.y, 0.0f);
                surface.color[2] = std::max(v.z, 0.0f);
                surface.color[3] = std::clamp(v.w, 0.0f, 1.0f);
            }
        } else if (entry.name == kOverrideMetallic) {
            if (finite(v.x)) surface.metallic = std::clamp(v.x, 0.0f, 1.0f);
        } else if (entry.name == kOverrideRoughness) {
            if (finite(v.x)) surface.roughness = std::clamp(v.x, 0.0f, 1.0f);
        } else if (entry.name == kOverrideEmissive) {
            if (finite(v.x) && finite(v.y) && finite(v.z)) {
                surface.emissive[0] = std::max(v.x, 0.0f);
                surface.emissive[1] = std::max(v.y, 0.0f);
                surface.emissive[2] = std::max(v.z, 0.0f);
            }
        }
    }
    return texture;
}

} // namespace aether::player
