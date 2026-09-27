#pragma once

#include "aether/core/base.h"
#include "aether/math/vec.h"

#include <utility>
#include <vector>

namespace aether::audio {

// 3D audio maths (Phase 17 step 2, docs/design/PHASE_SPECS.md §17.2):
// distance attenuation, panning from the listener's point of view, doppler
// and distance filtering. The mixer applies these per voice; they're plain
// functions so the editor can draw the curves.

enum class AttenuationModel : u8 {
    None,        // no distance falloff
    Inverse,     // min / (min + rolloff * (d - min)), the physical 1/d law
    Linear,      // 1 at min_distance to 0 at max_distance
    Logarithmic, // 1 - ln(d / min) / ln(max / min): falls fast, then slowly
    Custom,      // `curve`, piecewise linear
};

struct AttenuationSettings {
    AttenuationModel model = AttenuationModel::Inverse;
    f32 min_distance = 1.0f;  // full volume inside this
    f32 max_distance = 50.0f; // no further falloff past this (Linear, Logarithmic and Custom reach their end)
    f32 rolloff = 1.0f;       // Inverse only
    // Custom: (distance from min to max as 0..1, gain) points, sorted by distance.
    std::vector<std::pair<f32, f32>> curve;
    // Air absorption: a low-pass whose cutoff falls from open (at min_distance)
    // to this (at max_distance). 0 turns it off.
    f32 lowpass_at_max_hz = 0.0f;
};

// The gain (0..1) at a distance.
f32 Attenuate(const AttenuationSettings& settings, f32 distance);
// The air-absorption cutoff at a distance; kOpenCutoff when there is none.
f32 AirAbsorptionCutoff(const AttenuationSettings& settings, f32 distance);
constexpr f32 kOpenCutoff = 20000.0f;

struct Listener {
    Vec3 position;
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 velocity; // units per second, for doppler
};

// -1 (hard left) .. 1 (hard right): how far the source is to the listener's right.
f32 PanFromListener(const Listener& listener, const Vec3& source);

// The pitch ratio from relative motion along the line between source and
// listener (the OpenAL formula). Speeds are clamped below the speed of
// sound and the result to 1/4..4.
f32 DopplerRatio(const Listener& listener, const Vec3& source, const Vec3& source_velocity, f32 doppler_factor = 1.0f,
                 f32 speed_of_sound = 343.0f);

} // namespace aether::audio
