#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <vector>

// Image-based lighting from the sky (render roadmap step 3). An environment is an equirectangular (latitude-longitude) image of the
// radiance arriving from every direction, linear and unclamped: a SkyLight's .hdr (or LDR) image, or, without one, a procedural sky.
// From it the player builds, on the CPU once per environment:
//  - a prefiltered mip chain (RGBA16F): level 0 the sky itself (the background and mirror reflections), level i the sky convolved with
//    the GGX lobe of roughness i / (levels - 1), split-sum style (Karis 2013), for glossy reflections;
//  - nine spherical-harmonic coefficients of the irradiance (Ramamoorthi and Hanrahan 2001), for diffuse light.
// The scene shader reads both through the frame data; the radiance is scaled by the SkyLight's colour times intensity there, so a
// change of tint or intensity costs nothing.
//
// Directions: u = 0.5 looks down -Z, u grows toward +X (atan2(x, -z)); v = 0 is straight up (+Y), v = 1 straight down.

namespace aether::player {

struct EnvironmentImage {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> rgb; // width * height texels, three floats each, top row first
};

// The texel direction at (u, v) in [0, 1]^2, and back: the convention above (the shader's EnvironmentUv matches DirectionToEquirect).
Vec3 EquirectToDirection(f32 u, f32 v);
void DirectionToEquirect(const Vec3& direction, f32& u, f32& v);

// The sky without an image: a zenith-to-horizon gradient over a darker ground, about 0.35 bright on average (as bright as the flat
// ambient term the scene shader used before), width x width / 2 texels.
EnvironmentImage MakeProceduralSky(u32 width = 256);

// An equirectangular image as an environment: `rgb` linear radiance, `rotation_degrees` turning it about +Y (positive turns the sky
// from -Z toward +X). Non-finite and negative values become 0, values over 65 000 clamp (half-float range). Wrong sizes give an empty image.
EnvironmentImage MakeEnvironment(u32 width, u32 height, const std::vector<f32>& rgb, f32 rotation_degrees = 0.0f);
// From RGBA8 sRGB pixels (an LDR sky: PNG, JPG): decoded to linear.
EnvironmentImage MakeEnvironmentFromSrgb(u32 width, u32 height, const std::vector<u8>& rgba8, f32 rotation_degrees = 0.0f);

struct PrefilteredEnvironment {
    u32 width = 0; // level 0
    u32 height = 0;
    u32 mip_count = 0;
    std::vector<u16> halves;  // every level, level 0 first, RGBA16F (IDevice::CreateHalfFloatTexture's layout)
    f32 irradiance_sh[9][4]{}; // the irradiance's SH coefficients (rgb; w unused), cosine lobe folded in: E(n) = sum c_i Y_i(n)
};

struct PrefilterSettings {
    u32 max_width = 1024;    // level 0 is the source resampled to at most this wide (a power of two), and at least 64
    u32 smallest_width = 8;  // the chain stops at this width (8 x 4)
    u32 samples = 64;        // GGX samples per texel of each level after the first
};

// Builds the mip chain and the SH coefficients. An empty source gives an empty result (mip_count 0).
PrefilteredEnvironment PrefilterEnvironment(const EnvironmentImage& source, const PrefilterSettings& settings = {});

// The irradiance the SH coefficients give for normal n (what the shader computes), for tests.
Vec3 EvaluateIrradiance(const f32 (&sh)[9][4], const Vec3& n);

// IEEE half from float (round to nearest even; overflow to infinity, NaN kept) and back.
u16 FloatToHalf(f32 value);
f32 HalfToFloat(u16 value);

} // namespace aether::player
