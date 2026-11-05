#pragma once

// @stability: experimental
//
// Platform-neutral haptic feedback requests. A host installs a callback that
// maps these semantic effects to its device (UIKit, Android Vibrator, a game
// pad, etc.). With no callback installed, requests safely report unavailable.

#include "aether/core/base.h"

#include <cmath>
#include <functional>
#include <string_view>
#include <utility>

namespace aether::input {

enum class HapticEffect : u8 {
    Selection,
    ImpactLight,
    ImpactMedium,
    ImpactHeavy,
    Success,
    Warning,
    Error,
    Count,
};

inline const char* HapticEffectName(HapticEffect effect) {
    switch (effect) {
    case HapticEffect::Selection: return "selection";
    case HapticEffect::ImpactLight: return "impact_light";
    case HapticEffect::ImpactMedium: return "impact_medium";
    case HapticEffect::ImpactHeavy: return "impact_heavy";
    case HapticEffect::Success: return "success";
    case HapticEffect::Warning: return "warning";
    case HapticEffect::Error: return "error";
    case HapticEffect::Count: break;
    }
    return "unknown";
}

inline bool HapticEffectFromName(std::string_view name, HapticEffect& out) {
    for (u8 value = 0; value < static_cast<u8>(HapticEffect::Count); ++value) {
        const auto effect = static_cast<HapticEffect>(value);
        if (name == HapticEffectName(effect)) {
            out = effect;
            return true;
        }
    }
    return false;
}

class Haptics {
public:
    using Handler = std::function<void(HapticEffect, f32 intensity)>;

    // The host owns platform setup. The handler runs synchronously on the
    // caller's thread; a backend requiring a particular thread must dispatch
    // there itself. Passing an empty handler disables output.
    void SetHandler(Handler handler) { handler_ = std::move(handler); }
    bool IsAvailable() const { return static_cast<bool>(handler_); }

    // Intensity is in [0, 1]. Invalid effects or intensity values are
    // rejected; false also means that no host backend is connected.
    bool Play(HapticEffect effect, f32 intensity = 1.0f) const {
        if (!handler_ || effect >= HapticEffect::Count || !std::isfinite(intensity) ||
            intensity < 0.0f || intensity > 1.0f) {
            return false;
        }
        handler_(effect, intensity);
        return true;
    }

private:
    Handler handler_;
};

} // namespace aether::input
