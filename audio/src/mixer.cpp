#include "aether/audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <limits>

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
    if (sound == nullptr || sound->Frames() == 0 || sound->sample_rate == 0) return 0;
    Voice v;
    v.sound = sound;
    return Start(std::move(v), p);
}

VoiceId Mixer::PlayStream(std::unique_ptr<AudioStream> stream, const PlayParams& p) {
    if (stream == nullptr || stream->Frames() == 0 || stream->SampleRate() == 0 || stream->Channels() < 1 || stream->Channels() > 2) return 0;
    Voice v;
    v.stream = std::move(stream);
    v.window_start = std::numeric_limits<i64>::min() / 2; // nothing decoded yet: the first Fill seeks
    return Start(std::move(v), p);
}

VoiceId Mixer::Start(Voice v, const PlayParams& p) {
    if (p.bus >= buses_.size()) return 0;
    v.id = next_id_++;
    if (next_id_ == 0) next_id_ = 1;
    v.bus = p.bus;
    v.position = std::clamp(static_cast<f64>(p.start_time) * Rate(v), 0.0, static_cast<f64>(Frames(v)));
    v.pitch = std::max(p.pitch, 0.0f);
    v.gain = v.current_gain = DbToGain(p.volume_db);
    v.pan = std::clamp(p.pan, -1.0f, 1.0f);
    v.loop = p.loop;
    v.delay_frames = static_cast<u64>(std::llround(std::max(p.delay, 0.0) * sample_rate_));
    v.priority = p.priority;
    v.virtual_mode = p.virtual_mode;
    v.spatial = p.spatial;
    v.occlusion = p.occlusion;
    v.position3d = p.position;
    v.velocity = p.velocity;
    v.spatial_blend = std::clamp(p.spatial_blend, 0.0f, 1.0f);
    v.doppler = std::max(p.doppler, 0.0f);
    v.attenuation = p.attenuation;
    if (p.fade_in > 0.0f) {
        v.fade = 0.0f;
        v.fade_step = 1.0f / (p.fade_in * static_cast<f32>(sample_rate_));
    }
    const VoiceId id = v.id;
    voices_.push_back(std::move(v));
    return id;
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

bool Mixer::SetPosition(VoiceId id, const Vec3& position, const Vec3& velocity) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    v->position3d = position;
    v->velocity = velocity;
    return true;
}

bool Mixer::SetOcclusion(VoiceId id, f32 amount) {
    Voice* v = Find(id);
    if (v == nullptr) return false;
    v->occlusion_target = std::clamp(amount, 0.0f, 1.0f);
    return true;
}

void Mixer::UpdateOcclusion() {
    if (!occlusion_query) return;
    for (Voice& v : voices_) {
        if (v.spatial && v.occlusion) v.occlusion_target = std::clamp(occlusion_query(listener_.position, v.position3d), 0.0f, 1.0f);
    }
}

bool Mixer::GetVoiceInfo(VoiceId id, VoiceInfo& out) const {
    const Voice* v = Find(id);
    if (v == nullptr) return false;
    out = v->info;
    out.is_virtual = v->is_virtual;
    return true;
}

usize Mixer::RealVoiceCount() const {
    return static_cast<usize>(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.is_virtual; }));
}

bool Mixer::IsPlaying(VoiceId id) const { return Find(id) != nullptr; }

f32 Mixer::PlaybackTime(VoiceId id) const {
    const Voice* v = Find(id);
    if (v == nullptr) return -1.0f;
    const f64 n = static_cast<f64>(Frames(*v));
    const f64 at = v->stream && v->loop ? std::fmod(v->position, n) : v->position; // streams count past loops
    return static_cast<f32>(at / Rate(*v));
}

void Mixer::Spatialize(Voice& v, u32 frames, const std::vector<f32>& bus_gains) {
    const f32 rate = static_cast<f32>(sample_rate_);
    VoiceInfo& info = v.info;
    // Occlusion follows its target smoothly (or jumps there on the first block).
    const f32 follow = !v.started || occlusion_smoothing <= 0.0f ? 1.0f : 1.0f - std::exp(-static_cast<f32>(frames) / (occlusion_smoothing * rate));
    info.occlusion += (v.occlusion_target - info.occlusion) * follow;
    const f32 occlusion_gain = DbToGain(occlusion_volume_db * info.occlusion);
    const f32 occlusion_cutoff = kOpenCutoff * std::pow(std::clamp(occlusion_lowpass_hz, 10.0f, kOpenCutoff) / kOpenCutoff, info.occlusion);
    if (v.spatial) {
        const f32 blend = v.spatial_blend;
        info.distance = (v.position3d - listener_.position).Length();
        info.attenuation = 1.0f + (Attenuate(v.attenuation, info.distance) - 1.0f) * blend;
        info.pan = v.pan + (PanFromListener(listener_, v.position3d) - v.pan) * blend;
        const f32 doppler = v.doppler > 0.0f ? DopplerRatio(listener_, v.position3d, v.velocity, v.doppler, speed_of_sound) : 1.0f;
        info.pitch = v.pitch * (1.0f + (doppler - 1.0f) * blend);
        const f32 air = kOpenCutoff * std::pow(AirAbsorptionCutoff(v.attenuation, info.distance) / kOpenCutoff, blend);
        info.lowpass_hz = std::min(air, occlusion_cutoff);
    } else {
        info.distance = 0.0f;
        info.attenuation = 1.0f;
        info.pan = v.pan;
        info.pitch = v.pitch;
        info.lowpass_hz = occlusion_cutoff;
    }
    v.spatial_gain = info.attenuation * occlusion_gain;
    info.audibility = v.gain * v.spatial_gain * bus_gains[v.bus];
    if (!v.started) v.current_gain = v.gain * v.spatial_gain; // no glide from full volume on the first block
}

void Mixer::AssignChannels() {
    const f32 threshold = DbToGain(virtual_threshold_db);
    std::vector<Voice*> order;
    order.reserve(voices_.size());
    for (Voice& v : voices_) {
        if (v.info.audibility < threshold) {
            v.is_virtual = true;
        } else {
            order.push_back(&v);
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const Voice* a, const Voice* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        return a->info.audibility > b->info.audibility; // ties keep the older voice (stable)
    });
    for (usize i = 0; i < order.size(); ++i) order[i]->is_virtual = i >= max_voices_;
}

bool Mixer::AdvanceVirtual(Voice& v, u32 frames) {
    const u64 wait = std::min<u64>(v.delay_frames, frames);
    v.delay_frames -= wait;
    frames -= static_cast<u32>(wait);
    const f64 n = static_cast<f64>(Frames(v));
    if (v.virtual_mode == VirtualMode::Continue) {
        v.position += static_cast<f64>(v.info.pitch) * Rate(v) / sample_rate_ * frames;
        if (v.position >= n) {
            if (!v.loop) return false;
            v.position = std::fmod(v.position, n); // a stream seeks when it's heard again
        }
    }
    v.fade = std::clamp(v.fade + v.fade_step * static_cast<f32>(frames), 0.0f, 1.0f);
    if (v.fade_step > 0.0f && v.fade >= 1.0f) v.fade_step = 0.0f;
    return !(v.stopping && v.fade <= 0.0f);
}

void Mixer::Fill(Voice& v, i64 from, i64 to) {
    AudioStream& s = *v.stream;
    const u32 ch = s.Channels();
    const i64 n = static_cast<i64>(s.Frames());
    i64 end = v.window_start + static_cast<i64>(v.window.size() / ch);
    if (from < v.window_start || from > end) {
        // Somewhere new (the start, back from virtual, a restart): seek.
        v.window.clear();
        v.window_start = end = from;
        if (from < 0) {
            v.window.assign(static_cast<usize>(-from) * ch, 0.0f); // before the start is silence
            end = 0;
        }
        s.Seek(static_cast<u64>(v.loop ? end % n : std::min(end, n)));
    } else if (from > v.window_start) {
        v.window.erase(v.window.begin(), v.window.begin() + static_cast<std::ptrdiff_t>((from - v.window_start) * ch));
        v.window_start = from;
    }
    constexpr i64 kChunk = 4096; // decode ahead in chunks, not per block
    int dry = 0;                 // reads that returned nothing, to stop on a broken stream
    while (end < to) {
        const i64 want = std::max(to - end, kChunk);
        const usize old = v.window.size();
        v.window.resize(old + static_cast<usize>(want) * ch);
        const u32 got = s.Read(v.window.data() + old, static_cast<u32>(want));
        v.window.resize(old + static_cast<usize>(got) * ch);
        end += got;
        if (static_cast<i64>(got) < want) {
            dry = got == 0 ? dry + 1 : 0;
            if (v.loop && dry < 2) {
                s.Seek(0);
                continue;
            }
            v.window.resize(v.window.size() + static_cast<usize>(std::max<i64>(to - end, 0)) * ch, 0.0f); // past the end is silence
            break;
        }
    }
}

bool Mixer::MixVoice(Voice& v, f32* out, u32 frames, f32 ramp_from, f32 ramp_to) {
    // A delayed start: silence until it's due.
    const u32 wait = static_cast<u32>(std::min<u64>(v.delay_frames, frames));
    v.delay_frames -= wait;
    const i64 n = static_cast<i64>(Frames(v));
    const u32 ch = Channels(v);
    const f64 step = static_cast<f64>(v.info.pitch) * Rate(v) / sample_rate_;
    const bool streamed = v.stream != nullptr;
    if (streamed && wait < frames) {
        const i64 from = static_cast<i64>(std::floor(v.position)) - 1;
        Fill(v, from, static_cast<i64>(std::floor(v.position + step * (frames - wait))) + 3);
    }
    const i64 window_frames = streamed ? static_cast<i64>(v.window.size() / ch) : 0;
    const f32 smooth = 1.0f - std::exp(-1.0f / (kSmoothSeconds * static_cast<f32>(sample_rate_)));
    const f32 target = v.gain * v.spatial_gain;
    // One-pole low-pass for distance and occlusion; fully open passes the input through.
    const f32 lp = v.info.lowpass_hz >= kOpenCutoff ? 1.0f : 1.0f - std::exp(-2.0f * kPi * v.info.lowpass_hz / static_cast<f32>(sample_rate_));
    // Pan: equal power for mono, balance for stereo.
    f32 pan_l, pan_r;
    const f32 pan = std::clamp(v.info.pan, -1.0f, 1.0f);
    if (ch == 1) {
        const f32 angle = (pan + 1.0f) * kPi / 4.0f;
        pan_l = std::cos(angle), pan_r = std::sin(angle);
    } else {
        pan_l = pan > 0.0f ? 1.0f - pan : 1.0f;
        pan_r = pan < 0.0f ? 1.0f + pan : 1.0f;
    }
    auto at = [&](i64 frame, u32 c) -> f32 {
        if (streamed) {
            const i64 k = frame - v.window_start;
            return k >= 0 && k < window_frames ? v.window[static_cast<usize>(k) * ch + c] : 0.0f;
        }
        if (v.loop) frame = ((frame % n) + n) % n;
        else if (frame < 0 || frame >= n) return 0.0f;
        return v.sound->samples[static_cast<usize>(frame) * ch + c];
    };
    for (u32 i = wait; i < frames; ++i) {
        if (v.position >= static_cast<f64>(n) && !(streamed && v.loop)) {
            if (!v.loop) return false;
            v.position = std::fmod(v.position, static_cast<f64>(n));
        }
        const i64 base = static_cast<i64>(v.position);
        const f32 t = static_cast<f32>(v.position - static_cast<f64>(base));
        v.current_gain += (target - v.current_gain) * smooth;
        v.fade = std::clamp(v.fade + v.fade_step, 0.0f, 1.0f);
        if (v.fade_step > 0.0f && v.fade >= 1.0f) v.fade_step = 0.0f;
        const f32 ramp = ramp_from + (ramp_to - ramp_from) * static_cast<f32>(i + 1) / static_cast<f32>(frames);
        const f32 g = v.current_gain * v.fade * ramp;
        f32 left = Hermite(at(base - 1, 0), at(base, 0), at(base + 1, 0), at(base + 2, 0), t);
        f32 right = ch == 1 ? left : Hermite(at(base - 1, 1), at(base, 1), at(base + 1, 1), at(base + 2, 1), t);
        v.lowpass[0] += lp * (left - v.lowpass[0]);
        v.lowpass[1] += lp * (right - v.lowpass[1]);
        left = v.lowpass[0], right = v.lowpass[1];
        out[2 * i] += left * g * pan_l;
        out[2 * i + 1] += right * g * pan_r;
        v.position += step;
        if (v.loop && !streamed && v.position >= static_cast<f64>(n)) v.position = std::fmod(v.position, static_cast<f64>(n)); // times stay inside the sound
        if (v.stopping && v.fade <= 0.0f) return false;
    }
    return true;
}

void Mixer::Render(f32* out, u32 frames) {
    for (Bus& b : buses_) b.buffer.assign(static_cast<usize>(frames) * 2, 0.0f);
    // Each bus's gain all the way to Master, for audibility. Parents come first.
    std::vector<f32> bus_gains(buses_.size(), 1.0f);
    for (usize i = 0; i < buses_.size(); ++i) {
        const f32 own = buses_[i].muted ? 0.0f : buses_[i].gain;
        bus_gains[i] = i == kMasterBus ? own : own * bus_gains[buses_[i].parent];
    }
    for (Voice& v : voices_) Spatialize(v, frames, bus_gains);
    AssignChannels();
    for (usize i = 0; i < voices_.size();) {
        Voice& v = voices_[i];
        if (!v.started) v.was_virtual = v.is_virtual; // a new voice starts where it's put, without a ramp
        v.started = true;
        bool keep = true;
        if (!v.is_virtual) {
            if (v.was_virtual) {
                // Back from virtual: fade in over this block.
                if (v.virtual_mode == VirtualMode::Restart) v.position = 0.0;
                v.current_gain = v.gain * v.spatial_gain;
                v.lowpass[0] = v.lowpass[1] = 0.0f;
                keep = MixVoice(v, buses_[v.bus].buffer.data(), frames, 0.0f, 1.0f);
            } else {
                keep = MixVoice(v, buses_[v.bus].buffer.data(), frames, 1.0f, 1.0f);
            }
        } else if (!v.was_virtual) {
            // Just lost its channel: fade out over this block.
            keep = MixVoice(v, buses_[v.bus].buffer.data(), frames, 1.0f, 0.0f) && v.virtual_mode != VirtualMode::Stop;
        } else {
            keep = v.virtual_mode != VirtualMode::Stop && AdvanceVirtual(v, frames);
        }
        v.was_virtual = v.is_virtual;
        if (keep) {
            ++i;
        } else {
            voices_.erase(voices_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    frames_rendered_ += frames;
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
