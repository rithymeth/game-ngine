#include "aether/save/settings.h"

#include <algorithm>

namespace aether::save {

namespace {

void FixVolume(f32& v, const char* name, std::vector<std::string>* warnings) {
    if (std::isnan(v)) {
        v = 1.0f;
        if (warnings) warnings->push_back(std::string("the ") + name + " volume wasn't a number: reset to 1");
    } else if (v < 0.0f || v > 1.0f) {
        v = std::clamp(v, 0.0f, 1.0f);
        if (warnings) warnings->push_back(std::string("the ") + name + " volume was outside 0 to 1: clamped");
    }
}

void FixSize(u32& v, const char* name, std::vector<std::string>* warnings) {
    const u32 fixed = std::clamp<u32>(v, 320, 16384);
    if (fixed != v) {
        v = fixed;
        if (warnings) warnings->push_back(std::string("the ") + name + " was outside 320 to 16384: clamped");
    }
}

} // namespace

void Validate(GameSettings& s, std::vector<std::string>* warnings) {
    FixVolume(s.master, "master", warnings);
    FixVolume(s.music, "music", warnings);
    FixVolume(s.sfx, "sfx", warnings);
    FixVolume(s.voice, "voice", warnings);
    FixSize(s.width, "width", warnings);
    FixSize(s.height, "height", warnings);
    if (s.language.empty() || s.language.size() > 16) {
        s.language = "en";
        if (warnings) warnings->push_back("the language was empty or too long: reset to en");
    }
    if (s.quality.empty() || s.quality.size() > 64) {
        s.quality = "High";
        if (warnings) warnings->push_back("the quality preset name was empty or too long: reset to High");
    }
}

f32 BusVolumeDb(f32 linear) {
    if (!(linear > 0.0f)) return -80.0f;
    return std::max(20.0f * std::log10(std::min(linear, 1.0f)), -80.0f);
}

} // namespace aether::save
