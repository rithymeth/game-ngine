#pragma once

#include "aether/ui/widget.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace aether::ui {

// UI animations (Phase 18 step 4, docs/design/PHASE_SPECS.md §18.4):
// keyframe tracks on widget properties, played by an animator; tweens are
// one-track animations from the current value.

enum class Ease : u8 { Linear, Constant, EaseIn, EaseOut, EaseInOut, Back };
f32 ApplyEase(Ease ease, f32 t); // t in 0..1
const char* EaseName(Ease ease);
bool ParseEase(const std::string& name, Ease& out);

// Animatable properties: opacity; offset_x, offset_y, scale, scale_x,
// scale_y (the render transform); tint_r, tint_g, tint_b, tint_a (a Text's
// colour, an Image's, Border's or ProgressBar bar's tint); position_x,
// position_y, size_x, size_y (the slot: they move the layout); value (a
// Slider); percent (a ProgressBar).
bool SetProperty(Widget& widget, const std::string& property, f32 value);
bool GetProperty(const Widget& widget, const std::string& property, f32& out);
bool IsAnimatable(const std::string& property);
std::vector<std::string> AnimatableProperties(); // every name IsAnimatable accepts

struct UIKey {
    f32 time = 0.0f;
    f32 value = 0.0f;
    Ease ease = Ease::Linear; // how it moves from this key to the next
};

struct UITrack {
    std::string widget; // by name
    std::string property;
    std::vector<UIKey> keys; // by time
    f32 Evaluate(f32 time) const;
};

struct UIAnimation {
    std::string name;
    std::vector<UITrack> tracks;
    f32 Duration() const; // the last key's time
};

nlohmann::json AnimationToJson(const UIAnimation& animation);
bool AnimationFromJson(const nlohmann::json& j, UIAnimation& out, std::string* error = nullptr);

struct PlayOptions {
    f32 speed = 1.0f;
    u32 loops = 1;       // 0 = forever
    bool reverse = false; // from the end back to the start
    f32 start = 0.0f;    // seconds in
};

// Plays animations on a widget tree. Tracks find their widgets by name each
// tick (rows can come and go); several animations can play at once.
class UIAnimator {
public:
    explicit UIAnimator(Widget& root) : root_(root) {}

    void Add(UIAnimation animation); // replaces one with the same name
    const UIAnimation* Find(const std::string& name) const;
    // Missing widgets and unknown properties, for the designer and loading.
    std::vector<std::string> Validate(const UIAnimation& animation) const;

    bool Play(const std::string& name, const PlayOptions& options = {});
    void Stop(const std::string& name); // stays where it is
    void StopAll();
    bool IsPlaying(const std::string& name) const;
    f32 TimeOf(const std::string& name) const; // -1 when not playing

    // Tweens the property of a widget from its current value; returns the
    // tween's name (it plays like any other animation and replaces an earlier
    // tween of the same widget and property).
    std::string Tween(const std::string& widget, const std::string& property, f32 to, f32 duration, Ease ease = Ease::EaseOut);

    void Tick(f32 dt);
    // Animations that ended in the last Tick (not stopped ones).
    const std::vector<std::string>& Finished() const { return finished_; }

private:
    struct Playing {
        std::string name;
        PlayOptions options;
        f32 time = 0.0f;
        u32 loops_done = 0;
    };
    void Apply(const UIAnimation& a, f32 time);
    Widget& root_;
    std::vector<UIAnimation> animations_;
    std::vector<Playing> playing_;
    std::vector<std::string> finished_;
};

} // namespace aether::ui
