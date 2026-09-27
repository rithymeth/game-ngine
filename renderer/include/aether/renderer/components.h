#pragma once

#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

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

// Ambient light from the sky (the IBL's intensity until sky capture arrives).
struct SkyLight {
    Vec3 color{1, 1, 1};
    f32 intensity = 1.0f;
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
    AETHER_FIELD(intensity, Field_EditAnywhere, {.range_min = 0.0, .range_max = 1000.0})
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
    AETHER_FIELD(tonemapper, Field_EditAnywhere)
)
