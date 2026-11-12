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
//   BaseColorTexture, NormalTexture, MetallicRoughnessTexture, OcclusionTexture, EmissiveTexture
//                     texture parameters: the content path of an image in the package, drawn in place of the model's own texture in that slot (the metallic-roughness
//                     image has roughness in green and metallic in blue, the occlusion image reads red, the normal map is tangent space, the emissive image multiplies
//                     the Emissive colour, so give that a colour too)
// A name the entity does not set leaves the model's value; a value that is not a finite number is ignored (not clamped to something else).
inline constexpr const char* kOverrideBaseColor = "BaseColor";
inline constexpr const char* kOverrideMetallic = "Metallic";
inline constexpr const char* kOverrideRoughness = "Roughness";
inline constexpr const char* kOverrideEmissive = "Emissive";
inline constexpr const char* kOverrideBaseColorTexture = "BaseColorTexture";
inline constexpr const char* kOverrideNormalTexture = "NormalTexture";
inline constexpr const char* kOverrideMetallicRoughnessTexture = "MetallicRoughnessTexture";
inline constexpr const char* kOverrideOcclusionTexture = "OcclusionTexture";
inline constexpr const char* kOverrideEmissiveTexture = "EmissiveTexture";

// The texture slots a material has, in the order of kOverrideTextureNames.
enum class TextureSlot : u8 { BaseColor, Normal, MetallicRoughness, Occlusion, Emissive };
inline constexpr usize kTextureSlotCount = 5;
inline constexpr const char* kOverrideTextureNames[kTextureSlotCount] = {kOverrideBaseColorTexture, kOverrideNormalTexture, kOverrideMetallicRoughnessTexture,
                                                                         kOverrideOcclusionTexture, kOverrideEmissiveTexture};
// The slot a texture parameter name stands for, or -1 for a name that is not one.
inline int TextureSlotOfName(const std::string& name) {
    for (usize i = 0; i < kTextureSlotCount; ++i) {
        if (name == kOverrideTextureNames[i]) return static_cast<int>(i);
    }
    return -1;
}

// The paths of the images an entity draws in place of the model's, by slot (null: the model's own), pointing into the component.
struct TextureOverrides {
    const std::string* path[kTextureSlotCount] = {};
    const std::string* For(TextureSlot slot) const { return path[static_cast<usize>(slot)]; }
};

struct SurfaceValues {
    f32 color[4] = {1, 1, 1, 1};
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    f32 emissive[3] = {0, 0, 0};
};

// Applies the overrides to `surface`, and returns the images to draw in place of the model's.
inline TextureOverrides ApplyMaterialOverrides(const MaterialParameters& overrides, SurfaceValues& surface) {
    TextureOverrides textures;
    const auto finite = [](f32 value) { return std::isfinite(value); };
    for (const MaterialParameterValue& entry : overrides.values) {
        if (entry.is_texture) {
            const int slot = TextureSlotOfName(entry.name);
            if (slot >= 0 && !entry.texture.empty()) textures.path[slot] = &entry.texture;
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
    return textures;
}

} // namespace aether::player
