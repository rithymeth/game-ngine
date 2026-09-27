#include "aether/audio/spatial.h"

#include <algorithm>
#include <cmath>

namespace aether::audio {

f32 Attenuate(const AttenuationSettings& s, f32 distance) {
    const f32 min_d = std::max(s.min_distance, 1e-4f);
    const f32 max_d = std::max(s.max_distance, min_d);
    const f32 d = std::clamp(distance, min_d, max_d);
    const f32 t = max_d > min_d ? (d - min_d) / (max_d - min_d) : 0.0f; // 0..1 between min and max
    switch (s.model) {
    case AttenuationModel::None: return 1.0f;
    case AttenuationModel::Inverse: return min_d / (min_d + std::max(s.rolloff, 0.0f) * (d - min_d));
    case AttenuationModel::Linear: return max_d > min_d ? 1.0f - t : 1.0f;
    case AttenuationModel::Logarithmic: return max_d > min_d ? 1.0f - std::log(d / min_d) / std::log(max_d / min_d) : 1.0f;
    case AttenuationModel::Custom: {
        if (s.curve.empty()) return 1.0f;
        if (t <= s.curve.front().first) return std::clamp(s.curve.front().second, 0.0f, 1.0f);
        for (usize i = 1; i < s.curve.size(); ++i) {
            const auto& [x1, y1] = s.curve[i];
            if (t <= x1) {
                const auto& [x0, y0] = s.curve[i - 1];
                const f32 u = x1 > x0 ? (t - x0) / (x1 - x0) : 1.0f;
                return std::clamp(y0 + (y1 - y0) * u, 0.0f, 1.0f);
            }
        }
        return std::clamp(s.curve.back().second, 0.0f, 1.0f);
    }
    }
    return 1.0f;
}

f32 AirAbsorptionCutoff(const AttenuationSettings& s, f32 distance) {
    if (s.lowpass_at_max_hz <= 0.0f || s.lowpass_at_max_hz >= kOpenCutoff) return kOpenCutoff;
    const f32 min_d = std::max(s.min_distance, 1e-4f);
    const f32 max_d = std::max(s.max_distance, min_d);
    if (max_d <= min_d) return distance > min_d ? s.lowpass_at_max_hz : kOpenCutoff;
    const f32 t = std::clamp((distance - min_d) / (max_d - min_d), 0.0f, 1.0f);
    // Interpolate in octaves so the sweep sounds even.
    return kOpenCutoff * std::pow(s.lowpass_at_max_hz / kOpenCutoff, t);
}

f32 PanFromListener(const Listener& l, const Vec3& source) {
    const Vec3 to = source - l.position;
    const f32 d = to.Length();
    if (d < 1e-4f) return 0.0f;
    const Vec3 right = l.forward.Cross(l.up).Normalized();
    return std::clamp(to.Dot(right) / d, -1.0f, 1.0f);
}

f32 DopplerRatio(const Listener& l, const Vec3& source, const Vec3& source_velocity, f32 factor, f32 c) {
    const Vec3 sl = l.position - source; // from source to listener
    const f32 d = sl.Length();
    if (d < 1e-4f || factor <= 0.0f || c <= 0.0f) return 1.0f;
    const f32 limit = c / factor * 0.99f;
    const f32 vls = std::clamp(sl.Dot(l.velocity) / d, -limit, limit);      // listener moving away from the source
    const f32 vss = std::clamp(sl.Dot(source_velocity) / d, -limit, limit); // source moving toward the listener
    return std::clamp((c - factor * vls) / (c - factor * vss), 0.25f, 4.0f);
}

} // namespace aether::audio
