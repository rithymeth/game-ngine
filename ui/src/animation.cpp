#include "aether/ui/animation.h"

#include "aether/ui/basic.h"
#include "aether/ui/controls.h"
#include "aether/ui/panels.h"

#include <algorithm>
#include <cmath>

namespace aether::ui {

namespace {
constexpr const char* kEaseNames[] = {"Linear", "Constant", "EaseIn", "EaseOut", "EaseInOut", "Back"};
constexpr const char* kProperties[] = {"opacity", "offset_x",   "offset_y",   "scale",  "scale_x", "scale_y", "tint_r", "tint_g", "tint_b",
                                       "tint_a",  "position_x", "position_y", "size_x", "size_y",  "value",   "percent"};

// The colour a widget's tint channels change.
Color* TintOf(Widget& w) {
    if (auto* t = dynamic_cast<Text*>(&w)) return &t->color;
    if (auto* i = dynamic_cast<Image*>(&w)) return &i->brush.tint;
    if (auto* b = dynamic_cast<Border*>(&w)) return &b->background.tint;
    if (auto* p = dynamic_cast<ProgressBar*>(&w)) return &p->bar.tint;
    return nullptr;
}
} // namespace

f32 ApplyEase(Ease e, f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (e) {
    case Ease::Linear: return t;
    case Ease::Constant: return t < 1.0f ? 0.0f : 1.0f;
    case Ease::EaseIn: return t * t * t;
    case Ease::EaseOut: return 1.0f - std::pow(1.0f - t, 3.0f);
    case Ease::EaseInOut: return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
    case Ease::Back: {
        // Overshoots a little past the end, then settles (a pop).
        constexpr f32 c1 = 1.70158f, c3 = c1 + 1.0f;
        return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
    }
    }
    return t;
}

const char* EaseName(Ease e) { return kEaseNames[static_cast<usize>(e)]; }

bool ParseEase(const std::string& name, Ease& out) {
    for (usize i = 0; i < std::size(kEaseNames); ++i) {
        if (name == kEaseNames[i]) return out = static_cast<Ease>(i), true;
    }
    return false;
}

bool IsAnimatable(const std::string& p) { return std::find(std::begin(kProperties), std::end(kProperties), p) != std::end(kProperties); }

bool SetProperty(Widget& w, const std::string& p, f32 v) {
    if (p == "opacity") return w.opacity = std::clamp(v, 0.0f, 1.0f), true;
    if (p == "offset_x") return w.render_offset.x = v, true;
    if (p == "offset_y") return w.render_offset.y = v, true;
    if (p == "scale") return w.render_scale = {v, v}, true;
    if (p == "scale_x") return w.render_scale.x = v, true;
    if (p == "scale_y") return w.render_scale.y = v, true;
    if (p == "position_x") return w.slot.position.x = v, true;
    if (p == "position_y") return w.slot.position.y = v, true;
    if (p == "size_x") return w.slot.size.x = v, true;
    if (p == "size_y") return w.slot.size.y = v, true;
    if (p.rfind("tint_", 0) == 0) {
        Color* c = TintOf(w);
        if (c == nullptr) return false;
        const char ch = p.back();
        (ch == 'r' ? c->r : ch == 'g' ? c->g : ch == 'b' ? c->b : c->a) = v;
        return true;
    }
    if (p == "value") {
        if (auto* s = dynamic_cast<Slider*>(&w)) return s->value = v, true;
        return false;
    }
    if (p == "percent") {
        if (auto* b = dynamic_cast<ProgressBar*>(&w)) return b->percent = v, true;
        return false;
    }
    return false;
}

bool GetProperty(const Widget& w, const std::string& p, f32& out) {
    if (p == "opacity") return out = w.opacity, true;
    if (p == "offset_x") return out = w.render_offset.x, true;
    if (p == "offset_y") return out = w.render_offset.y, true;
    if (p == "scale" || p == "scale_x") return out = w.render_scale.x, true;
    if (p == "scale_y") return out = w.render_scale.y, true;
    if (p == "position_x") return out = w.slot.position.x, true;
    if (p == "position_y") return out = w.slot.position.y, true;
    if (p == "size_x") return out = w.slot.size.x, true;
    if (p == "size_y") return out = w.slot.size.y, true;
    if (p.rfind("tint_", 0) == 0) {
        const Color* c = TintOf(const_cast<Widget&>(w));
        if (c == nullptr) return false;
        const char ch = p.back();
        return out = ch == 'r' ? c->r : ch == 'g' ? c->g : ch == 'b' ? c->b : c->a, true;
    }
    if (const auto* s = dynamic_cast<const Slider*>(&w); s != nullptr && p == "value") return out = s->value, true;
    if (const auto* b = dynamic_cast<const ProgressBar*>(&w); b != nullptr && p == "percent") return out = b->percent, true;
    return false;
}

f32 UITrack::Evaluate(f32 t) const {
    if (keys.empty()) return 0.0f;
    if (t <= keys.front().time) return keys.front().value;
    for (usize i = 1; i < keys.size(); ++i) {
        if (t < keys[i].time) {
            const UIKey& a = keys[i - 1];
            const UIKey& b = keys[i];
            const f32 u = b.time > a.time ? (t - a.time) / (b.time - a.time) : 1.0f;
            return a.value + (b.value - a.value) * ApplyEase(a.ease, u);
        }
    }
    return keys.back().value;
}

f32 UIAnimation::Duration() const {
    f32 d = 0.0f;
    for (const UITrack& t : tracks) {
        if (!t.keys.empty()) d = std::max(d, t.keys.back().time);
    }
    return d;
}

nlohmann::json AnimationToJson(const UIAnimation& a) {
    nlohmann::json tracks = nlohmann::json::array();
    for (const UITrack& t : a.tracks) {
        nlohmann::json keys = nlohmann::json::array();
        for (const UIKey& k : t.keys) {
            nlohmann::json key = {k.time, k.value};
            if (k.ease != Ease::Linear) key.push_back(EaseName(k.ease));
            keys.push_back(key);
        }
        tracks.push_back({{"widget", t.widget}, {"property", t.property}, {"keys", keys}});
    }
    return {{"name", a.name}, {"tracks", tracks}};
}

bool AnimationFromJson(const nlohmann::json& j, UIAnimation& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = "animation '" + j.value("name", std::string()) + "': " + m;
        return false;
    };
    UIAnimation a;
    a.name = j.value("name", std::string());
    if (a.name.empty()) return fail("it needs a name");
    for (const auto& tj : j.value("tracks", nlohmann::json::array())) {
        UITrack t;
        t.widget = tj.value("widget", std::string());
        t.property = tj.value("property", std::string());
        if (!IsAnimatable(t.property)) return fail("'" + t.property + "' can't be animated");
        for (const auto& kj : tj.value("keys", nlohmann::json::array())) {
            if (!kj.is_array() || kj.size() < 2) return fail("a key is [time, value(, ease)]");
            UIKey k{kj[0].get<f32>(), kj[1].get<f32>(), Ease::Linear};
            if (kj.size() > 2 && !ParseEase(kj[2].get<std::string>(), k.ease)) return fail("unknown ease '" + kj[2].get<std::string>() + "'");
            t.keys.push_back(k);
        }
        std::stable_sort(t.keys.begin(), t.keys.end(), [](const UIKey& x, const UIKey& y) { return x.time < y.time; });
        a.tracks.push_back(std::move(t));
    }
    out = std::move(a);
    return true;
}

// --- The animator --------------------------------------------------------------------------------

void UIAnimator::Add(UIAnimation a) {
    for (UIAnimation& existing : animations_) {
        if (existing.name == a.name) {
            existing = std::move(a);
            return;
        }
    }
    animations_.push_back(std::move(a));
}

const UIAnimation* UIAnimator::Find(const std::string& name) const {
    for (const UIAnimation& a : animations_) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

std::vector<std::string> UIAnimator::Validate(const UIAnimation& a) const {
    std::vector<std::string> problems;
    for (const UITrack& t : a.tracks) {
        Widget* w = root_.Find(t.widget);
        f32 probe = 0.0f;
        if (w == nullptr) problems.push_back(a.name + ": no widget named '" + t.widget + "'");
        else if (!GetProperty(*w, t.property, probe)) problems.push_back(a.name + ": a " + w->TypeName() + " has no '" + t.property + "'");
        if (t.keys.empty()) problems.push_back(a.name + ": the " + t.widget + "." + t.property + " track has no keys");
    }
    return problems;
}

bool UIAnimator::Play(const std::string& name, const PlayOptions& options) {
    const UIAnimation* a = Find(name);
    if (a == nullptr) return false;
    Stop(name);
    Playing p{name, options, std::clamp(options.start, 0.0f, a->Duration()), 0};
    if (options.reverse && options.start == 0.0f) p.time = a->Duration();
    playing_.push_back(p);
    Apply(*a, p.time); // the first frame shows at once
    return true;
}

void UIAnimator::Stop(const std::string& name) {
    std::erase_if(playing_, [&](const Playing& p) { return p.name == name; });
}

void UIAnimator::StopAll() { playing_.clear(); }

bool UIAnimator::IsPlaying(const std::string& name) const {
    return std::any_of(playing_.begin(), playing_.end(), [&](const Playing& p) { return p.name == name; });
}

f32 UIAnimator::TimeOf(const std::string& name) const {
    for (const Playing& p : playing_) {
        if (p.name == name) return p.time;
    }
    return -1.0f;
}

std::string UIAnimator::Tween(const std::string& widget, const std::string& property, f32 to, f32 duration, Ease ease) {
    const std::string name = "tween:" + widget + "." + property;
    Widget* w = root_.Find(widget);
    f32 from = to;
    if (w != nullptr) GetProperty(*w, property, from);
    UIAnimation a;
    a.name = name;
    a.tracks.push_back({widget, property, {{0.0f, from, ease}, {std::max(duration, 0.0f), to, Ease::Linear}}});
    Add(std::move(a));
    Play(name);
    return name;
}

void UIAnimator::Apply(const UIAnimation& a, f32 time) {
    for (const UITrack& t : a.tracks) {
        if (Widget* w = root_.Find(t.widget)) SetProperty(*w, t.property, t.Evaluate(time));
    }
}

void UIAnimator::Tick(f32 dt) {
    finished_.clear();
    for (usize i = 0; i < playing_.size();) {
        Playing& p = playing_[i];
        const UIAnimation* a = Find(p.name);
        if (a == nullptr) {
            playing_.erase(playing_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const f32 d = a->Duration();
        const f32 step = dt * std::max(p.options.speed, 0.0f);
        bool done = false;
        if (d <= 0.0f) {
            done = true;
        } else if (!p.options.reverse) {
            p.time += step;
            while (p.time >= d && !done) {
                ++p.loops_done;
                if (p.options.loops != 0 && p.loops_done >= p.options.loops) done = true, p.time = d;
                else p.time -= d;
            }
        } else {
            p.time -= step;
            while (p.time <= 0.0f && !done) {
                ++p.loops_done;
                if (p.options.loops != 0 && p.loops_done >= p.options.loops) done = true, p.time = 0.0f;
                else p.time += d;
            }
        }
        Apply(*a, p.time);
        if (done) {
            finished_.push_back(p.name);
            playing_.erase(playing_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

} // namespace aether::ui
