#include "aether/audio/mixer.h"

#include <algorithm>
#include <cmath>

namespace aether::audio {

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kSmoothSeconds = 0.005f; // volume changes glide over about this long

// Catmull-Rom (cubic Hermite) through four samples, at `t` between y1 and y2.
f32 Hermite(f32 y0, f32 y1, f32 y2, f32 y3, f32 t) {
    const f32 c1 = 0.5f * (y2 - y0);
    const f32 c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const f32 c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

} // namespace

Mixer::Mixer(u32 sample_rate) : sample_rate_(std::max(sample_rate, 1u)) {
    Bus master;
    master.name = "Master";
    buses_.push_back(std::move(master));
}

BusId Mixer::AddBus(const std::string& name, BusId parent) {
    if (parent >= buses_.size()) parent = kMasterBus;
    Bus b;
    b.name = name;
    b.parent = parent;
    buses_.push_back(std::move(b));
    return static_cast<BusId>(buses_.size() - 1);
}

void Mixer::AddDefaultBuses() {
    for (const char* name : {"Music", "SFX", "UI", "Voice"}) {
        if (FindBus(name) == kInvalidBus) AddBus(name);
    }
}

BusId Mixer::FindBus(const std::string& name) const {
    for (usize i = 0; i < buses_.size(); ++i) {
        if (buses_[i].name == name) return static_cast<BusId>(i);
    }
    return kInvalidBus;
}

bool Mixer::SetBusVolume(BusId bus, f32 db) {
    if (bus >= buses_.size()) return false;
    buses_[bus].volume_db = db;
    buses_[bus].gain = DbToGain(db);
    return true;
}

bool Mixer::SetBusMuted(BusId bus, bool muted) {
    if (bus >= buses_.size()) return false;
    buses_[bus].muted = muted;
    return true;
}

AudioEffect* Mixer::AddEffect(BusId bus, std::unique_ptr<AudioEffect> effect) {
    if (bus >= buses_.size() || !effect) return nullptr;
    buses_[bus].effects.push_back(std::move(effect));
    return buses_[bus].effects.back().get();
}

Mixer::Voice* Mixer::Find(VoiceId id) {
    for (Voice& v : voices_) {
        if (v.id == id) return &v;
    }
    return nullptr;
}
const Mixer::Voice* Mixer::Find(VoiceId id) const {
    for (const Voice& v : voices_) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

VoiceId Mixer::Play(const SoundWave* sound, const PlayParams& p) {
    if (sound == nullptr || sound->Frames() == 0 || sound->sample_rate == 0 || p.bus >= buses_.size()) return 0;
    Voice v;
    v.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    v.sound = sound;
    v.bus = p.bus;
    v.position = std::clamp(static_cast<f64>(p.start_time) * sound->sample_rate, 0.0, static_cast<f64>(sound->Frames()));
    v.pitch = std::max(p.pitch, 0.0f);
    v.gain = v.current_gain = DbToGain(p.volume_db);
    v.pan = std::clamp(p.pan, -1.0f, 1.0f);
    v.loop = p.loop;
    if (p.fade_in > 0.0f) {
        v.fade = 0.0f;
        v.fade_step = 1.0f / (p.fade_in * static_cast<f32>(sample_rate_));
    }
    voices_.push_back(v);
    return v.id;
}

bool Mixer::Stop(VoiceId id, f32 fade_out) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    if (fade_out <= 0.0f) {
        voices_.erase(voices_.begin() + (v - voices_.data()));
        return true;
    }
    v->stopping = true;
    v->fade_step = -std::max(v->fade, 1e-6f) / (fade_out * static_cast<f32>(sample_rate_));
    return true;
}

void Mixer::StopAll() { voices_.clear(); }

bool Mixer::SetVolume(VoiceId id, f32 db) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    v->gain = DbToGain(db);
    return true;
}

bool Mixer::SetPitch(VoiceId id, f32 pitch) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    v->pitch = std::max(pitch, 0.0f);
    return true;
}

bool Mixer::SetPan(VoiceId id, f32 pan) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    v->pan = std::clamp(pan, -1.0f, 1.0f);
    return true;
}

bool Mixer::IsPlaying(VoiceId id) const { return Find(id) != nullptr; }

f32 Mixer::PlaybackTime(VoiceId id) const {
    const Voice* v = Find(id);
    return v == nullptr ? -1.0f : static_cast<f32>(v->position / v->sound->sample_rate);
}

bool Mixer::MixVoice(Voice& v, f32* out, u32 frames) {
    const SoundWave& s = *v.sound;
    const i64 n = static_cast<i64>(s.Frames());
    const u32 ch = s.channels;
    const f64 step = static_cast<f64>(v.pitch) * s.sample_rate / sample_rate_;
    const f32 smooth = 1.0f - std::exp(-1.0f / (kSmoothSeconds * static_cast<f32>(sample_rate_)));
    // Pan: equal power for mono, balance for stereo.
    f32 pan_l, pan_r;
    if (ch == 1) {
        const f32 angle = (v.pan + 1.0f) * kPi / 4.0f;
        pan_l = std::cos(angle), pan_r = std::sin(angle);
    } else {
        pan_l = v.pan > 0.0f ? 1.0f - v.pan : 1.0f;
        pan_r = v.pan < 0.0f ? 1.0f + v.pan : 1.0f;
    }
    auto at = [&](i64 frame, u32 c) -> f32 {
        if (v.loop) frame = ((frame % n) + n) % n;
        else if (frame < 0 || frame >= n) return 0.0f;
        return s.samples[static_cast<usize>(frame) * ch + c];
    };
    for (u32 i = 0; i < frames; ++i) {
        if (v.position >= static_cast<f64>(n)) {
            if (!v.loop) return false;
            v.position = std::fmod(v.position, static_cast<f64>(n));
        }
        const i64 base = static_cast<i64>(v.position);
        const f32 t = static_cast<f32>(v.position - static_cast<f64>(base));
        v.current_gain += (v.gain - v.current_gain) * smooth;
        v.fade = std::clamp(v.fade + v.fade_step, 0.0f, 1.0f);
        if (v.fade_step > 0.0f && v.fade >= 1.0f) v.fade_step = 0.0f;
        const f32 g = v.current_gain * v.fade;
        const f32 left = Hermite(at(base - 1, 0), at(base, 0), at(base + 1, 0), at(base + 2, 0), t);
        const f32 right = ch == 1 ? left : Hermite(at(base - 1, 1), at(base, 1), at(base + 1, 1), at(base + 2, 1), t);
        out[2 * i] += left * g * pan_l;
        out[2 * i + 1] += right * g * pan_r;
        v.position += step;
        if (v.loop && v.position >= static_cast<f64>(n)) v.position = std::fmod(v.position, static_cast<f64>(n)); // times stay inside the sound
        if (v.stopping && v.fade <= 0.0f) return false;
    }
    return true;
}

void Mixer::Render(f32* out, u32 frames) {
    for (Bus& b : buses_) b.buffer.assign(static_cast<usize>(frames) * 2, 0.0f);
    for (usize i = 0; i < voices_.size();) {
        Voice& v = voices_[i];
        if (MixVoice(v, buses_[v.bus].buffer.data(), frames)) {
            ++i;
        } else {
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    // Children have higher ids than their parents: walk backwards so each bus is done before its parent.
    for (usize idx = buses_.size(); idx-- > 0;) {
        Bus& b = buses_[idx];
        f32* data = b.buffer.data();
        for (auto& e : b.effects) {
            if (!e->bypass) e->Process(data, frames, sample_rate_);
        }
        const f32 target = b.muted ? 0.0f : b.gain;
        BusMeter meter;
        f64 sum[2] = {0, 0};
        for (u32 i = 0; i < frames; ++i) {
            const f32 g = b.current_gain + (target - b.current_gain) * static_cast<f32>(i + 1) / static_cast<f32>(frames);
            for (int c = 0; c < 2; ++c) {
                f32& x = data[2 * i + c];
                x *= g;
                meter.peak[c] = std::max(meter.peak[c], std::fabs(x));
                sum[c] += static_cast<f64>(x) * x;
            }
        }
        b.current_gain = target;
        for (int c = 0; c < 2; ++c) meter.rms[c] = frames > 0 ? static_cast<f32>(std::sqrt(sum[c] / frames)) : 0.0f;
        b.meter = meter;
        if (idx != kMasterBus) {
            f32* parent = buses_[b.parent].buffer.data();
            for (usize i = 0; i < static_cast<usize>(frames) * 2; ++i) parent[i] += data[i];
        }
    }
    std::copy(buses_[kMasterBus].buffer.begin(), buses_[kMasterBus].buffer.end(), out);
}

} // namespace aether::audio
