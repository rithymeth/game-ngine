#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

#include <string>

namespace aether {

// Rendering components (Phase 14 step 1). Colors are linear RGB. The
// renderer points each light along its entity's -Z (forward), like the camera.

struct DirectionalLight {
    Vec3 color{1, 1, 1};
    f32 intensity = 3.0f; // multiplier; physical units come with the exposure work
    bool cast_shadows = true;
};

struct PointLight {
    Vec3 color{1, 1, 1};
    f32 intensity = 10.0f;
    f32 range = 10.0f; // light fades to zero here
    bool cast_shadows = false;
};

struct SpotLight {
    Vec3 color{1, 1, 1};
    f32 intensity = 10.0f;
    f32 range = 10.0f;
    f32 inner_angle_degrees = 20.0f; // full intensity inside
    f32 outer_angle_degrees = 30.0f; // zero outside
    bool cast_shadows = false;
};

// Light from the sky: image-based lighting (diffuse and reflections) and the background. `environment` is an equirectangular
// (latitude-longitude, 2:1) image in the package, a Radiance .hdr (linear, unclamped) or an LDR image (sRGB); empty uses a
// procedural sky. Its radiance is scaled by colour times intensity. `environment_rotation_degrees` turns it about +Y.
struct SkyLight {
    Vec3 color{1, 1, 1};
    f32 intensity = 1.0f;
    std::string environment;
    f32 environment_rotation_degrees = 0.0f;
};

enum class Tonemapper : u8 { ACES, AgX, None };

// Post-processing settings. A global volume applies everywhere; a bounded one
// (a box of `extents` around the entity) applies inside it, fading over
// `blend_distance` outside. Higher `priority` volumes are applied later.
struct PostProcessVolume {
    bool global = true;
    Vec3 extents{5, 5, 5};
    f32 blend_distance = 1.0f;
    f32 weight = 1.0f;
    i32 priority = 0;
    f32 exposure_compensation = 0.0f; // EV
    f32 bloom_intensity = 0.05f;
    f32 vignette = 0.0f;
    f32 saturation = 1.0f;
    Tonemapper tonemapper = Tonemapper::ACES;
    f32 fog_density = 0.0f; // 1 / m: distance haze toward the sky's colour, 1 - exp(-density * distance); 0 is off
};

// A decal (Phase 39.4): a box, `size` metres across, at the entity's transform, that paints `texture` onto whatever opaque surfaces are inside it,
// projected along the box's local Z. Where a surface turns edge-on to that axis the decal fades (its |cos| between angle_fade_end and
// angle_fade_start). Drawn after the opaque scene, over the surfaces' lit colour, unlit (colour is a tint; there is no lighting on the decal itself yet).
struct Decal {
    std::string texture;           // a texture in the package; empty draws a flat tint
    Vec3 size{1, 1, 1};            // the box's full width, height and depth, in metres
    Vec3 color{1, 1, 1};           // linear tint, multiplied into the texture
    f32 opacity = 1.0f;            // 0..1, times the texture's alpha
    f32 angle_fade_start = 0.5f;   // full strength where |surface normal . projection axis| is at least this
    f32 angle_fade_end = 0.1f;     // gone where it is at most this
    i32 sort_order = 0;            // lower draws first; ties draw far to near
    bool enabled = true;
};

// Registers the components with the ECS (idempotent), so scenes load them.
void RegisterRenderComponents();

} // namespace aether

AETHER_REFLECT(aether::DirectionalLight, 1,
    AETHER_FIELD(color, Field_EditAnywhere),
    AETHER_FIELD(intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100000.0}),
    AETHER_FIELD(cast_shadows, Field_EditAnywhere)
)
AETHER_REFLECT(aether::PointLight, 1,
    AETHER_FIELD(color, Field_EditAnywhere),
    AETHER_FIELD(intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100000.0}),
    AETHER_FIELD(range, Field_EditAnywhere, {.range_min = 0.01, .range_max = 10000.0, .units = "m"}),
    AETHER_FIELD(cast_shadows, Field_EditAnywhere)
)
AETHER_REFLECT(aether::SpotLight, 1,
    AETHER_FIELD(color, Field_EditAnywhere),
    AETHER_FIELD(intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 100000.0}),
    AETHER_FIELD(range, Field_EditAnywhere, {.range_min = 0.01, .range_max = 10000.0, .units = "m"}),
    AETHER_FIELD(inner_angle_degrees, Field_EditAnywhere, {.range_min = 0.0, .range_max = 89.0, .units = "deg"}),
    AETHER_FIELD(outer_angle_degrees, Field_EditAnywhere, {.range_min = 0.0, .range_max = 89.0, .units = "deg"}),
    AETHER_FIELD(cast_shadows, Field_EditAnywhere)
)
AETHER_REFLECT(aether::SkyLight, 1,
    AETHER_FIELD(color, Field_EditAnywhere),
    AETHER_FIELD(intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0}),
    AETHER_FIELD(environment, Field_EditAnywhere, {.tooltip = "An equirectangular sky image in the package (.hdr or LDR); empty uses a procedural sky"}),
    AETHER_FIELD(environment_rotation_degrees, Field_EditAnywhere, {.tooltip = "Turns the sky image about the up axis", .range_min = -360.0, .range_max = 360.0, .units = "deg"})
)
AETHER_ENUM(aether::Tonemapper, 1, AETHER_ENUM_VALUE(ACES), AETHER_ENUM_VALUE(AgX), AETHER_ENUM_VALUE(None))
AETHER_REFLECT(aether::PostProcessVolume, 1,
    AETHER_FIELD(global, Field_EditAnywhere, {.tooltip = "Applies everywhere, not just inside the box"}),
    AETHER_FIELD(extents, Field_EditAnywhere, {.tooltip = "Half size of the box (bounded volumes)", .units = "m"}),
    AETHER_FIELD(blend_distance, Field_EditAnywhere, {.tooltip = "Fades in over this distance outside the box", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(weight, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(priority, Field_EditAnywhere, {.tooltip = "Higher priorities are applied later (win)"}),
    AETHER_FIELD(exposure_compensation, Field_EditAnywhere, {.range_min = -10.0, .range_max = 10.0, .units = "EV"}),
    AETHER_FIELD(bloom_intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 10.0}),
    AETHER_FIELD(vignette, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(saturation, Field_EditAnywhere, {.range_min = 0.0, .range_max = 2.0}),
    AETHER_FIELD(tonemapper, Field_EditAnywhere),
    AETHER_FIELD(fog_density, Field_EditAnywhere, {.tooltip = "Distance haze toward the sky colour: 1 - exp(-density * distance)", .range_min = 0.0, .range_max = 1.0, .units = "1/m"})
)
AETHER_REFLECT(aether::Decal, 1,
    AETHER_FIELD(texture, Field_EditAnywhere, {.tooltip = "A texture in the package; empty draws a flat tint"}),
    AETHER_FIELD(size, Field_EditAnywhere, {.tooltip = "The box's full size; the decal projects along its local Z", .range_min = 0.001, .range_max = 10000.0, .units = "m"}),
    AETHER_FIELD(color, Field_EditAnywhere),
    AETHER_FIELD(opacity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(angle_fade_start, Field_EditAnywhere, {.tooltip = "Full strength at or above this |cos| between the surface normal and the projection", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(angle_fade_end, Field_EditAnywhere, {.tooltip = "Gone at or below this |cos|", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(sort_order, Field_EditAnywhere, {.tooltip = "Lower draws first"}),
    AETHER_FIELD(enabled, Field_EditAnywhere)
)
