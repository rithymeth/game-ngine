#include "audio.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lanternwake {
namespace {

constexpr int kRate = 48000;
constexpr double kTau = 6.28318530717958647692;

float Sine(double phase) { return static_cast<float>(std::sin(phase)); }

float Noise(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state & 0xffffu) / 32767.5f - 1.0f;
}

float Envelope(float t, float duration, float attack, float release) {
    return std::clamp(t / attack, 0.0f, 1.0f) *
           std::clamp((duration - t) / release, 0.0f, 1.0f);
}

// A soft struck glass tone. Each call adds its own attack and tail to a
// SoundWave, so overlapping notes never require an allocation in the callback.
void Bell(aether::audio::SoundWave& wave, float at, float frequency,
          float duration, float gain, float pan = 0) {
    const int first = static_cast<int>(at * kRate);
    const int count = static_cast<int>(duration * kRate);
    for (int i = 0; i < count && first + i < static_cast<int>(wave.Frames()); ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float envelope = Envelope(t, duration, 0.012f, 0.2f) * std::exp(-3.2f * t / duration);
        const float sample = gain * envelope *
            (Sine(kTau * frequency * t) + 0.28f * Sine(kTau * frequency * 2.003 * t) +
             0.08f * Sine(kTau * frequency * 3.997 * t));
        const auto index = static_cast<std::size_t>(first + i) * wave.channels;
        if (wave.channels == 2) {
            wave.samples[index] += sample * (1.0f - pan * 0.45f);
            wave.samples[index + 1] += sample * (1.0f + pan * 0.45f);
        } else {
            wave.samples[index] += sample;
        }
    }
}

aether::audio::SoundWave Blank(const char* name, float seconds, int channels = 1) {
    aether::audio::SoundWave wave;
    wave.name = name;
    wave.sample_rate = kRate;
    wave.channels = static_cast<aether::u32>(channels);
    wave.samples.resize(static_cast<std::size_t>(seconds * kRate) * channels, 0.0f);
    return wave;
}

} // namespace

Audio::~Audio() { Stop(); }

void Audio::Compose() {
    // An original, deliberately sparse 24-second D-minor forest nocturne:
    // four breathing chords, quiet glass notes and a filtered air bed.
    ambience_ = Blank("Lanternwake / The Sleeping Grove", 24.0f, 2);
    constexpr float chords[4][3] = {
        {146.832f, 174.614f, 220.000f},
        {116.541f, 146.832f, 174.614f},
        {130.813f, 174.614f, 220.000f},
        {130.813f, 164.814f, 195.998f}
    };
    std::uint32_t random = 0x4c414e54u;
    float air = 0;
    for (std::size_t frame = 0; frame < ambience_.Frames(); ++frame) {
        const float t = static_cast<float>(frame) / kRate;
        const int chord = std::min(3, static_cast<int>(t / 6.0f));
        const float phrase = std::fmod(t, 6.0f) / 6.0f;
        const float breath = std::pow(std::sin(3.14159265358979323846f * phrase), 1.6f);
        const float edge = Envelope(t, 24.0f, 0.6f, 1.1f);
        float left = 0, right = 0;
        for (int note = 0; note < 3; ++note) {
            const double phase = kTau * chords[chord][note] * t;
            left += 0.022f * breath * (Sine(phase) + 0.2f * Sine(phase * 2.0));
            right += 0.022f * breath * (Sine(phase * 1.0008) + 0.2f * Sine(phase * 2.001));
        }
        air += 0.009f * (Noise(random) - air);
        const float breeze = air * 0.07f * (0.6f + 0.4f * Sine(kTau * t / 12.0f));
        ambience_.samples[frame * 2] = (left + breeze) * edge;
        ambience_.samples[frame * 2 + 1] = (right + breeze) * edge;
    }
    constexpr float melody[] = {587.330f, 880.000f, 698.456f, 523.251f,
                                587.330f, 783.991f, 698.456f, 440.000f};
    for (int note = 0; note < 8; ++note)
        Bell(ambience_, 0.8f + note * 2.7f, melody[note], 2.3f, 0.025f,
             note % 2 == 0 ? -0.6f : 0.6f);

    effects_[static_cast<int>(EventType::Start)] = Blank("The lantern wakes", 1.5f);
    auto& start = effects_[static_cast<int>(EventType::Start)];
    Bell(start, 0, 293.665f, 1.0f, 0.15f);
    Bell(start, 0.16f, 440.000f, 1.0f, 0.12f);
    Bell(start, 0.32f, 587.330f, 1.1f, 0.10f);

    effects_[static_cast<int>(EventType::Pickup)] = Blank("Ember glass", 0.6f);
    auto& pickup = effects_[static_cast<int>(EventType::Pickup)];
    Bell(pickup, 0, 880.0f, 0.5f, 0.14f);
    Bell(pickup, 0.07f, 1174.66f, 0.45f, 0.08f);

    effects_[static_cast<int>(EventType::Dash)] = Blank("Keeper step", 0.26f);
    effects_[static_cast<int>(EventType::Pulse)] = Blank("Lantern bloom", 0.9f);
    effects_[static_cast<int>(EventType::Hurt)] = Blank("Shadow touch", 0.45f);
    for (const auto type : {EventType::Dash, EventType::Pulse, EventType::Hurt}) {
        auto& wave = effects_[static_cast<int>(type)];
        double phase = 0;
        float filtered = 0;
        for (std::size_t i = 0; i < wave.samples.size(); ++i) {
            const float t = static_cast<float>(i) / kRate;
            const float p = t / wave.Duration();
            const bool pulse = type == EventType::Pulse;
            const float frequency = pulse ? 240.0f - p * 150.0f :
                                    type == EventType::Hurt ? 125.0f - p * 75.0f : 420.0f - p * 330.0f;
            phase += kTau * frequency / kRate;
            filtered += (Noise(random) - filtered) * (pulse ? 0.08f : 0.3f);
            const float envelope = Envelope(t, wave.Duration(), 0.012f, 0.1f) * std::exp(-3.0f * p);
            wave.samples[i] = envelope * (0.22f * Sine(phase) + filtered * (pulse ? 0.13f : 0.22f));
        }
    }

    effects_[static_cast<int>(EventType::ShrineLit)] = Blank("A shrine remembers", 2.0f);
    auto& shrine = effects_[static_cast<int>(EventType::ShrineLit)];
    for (int note = 0; note < 4; ++note)
        Bell(shrine, note * 0.12f, std::array<float, 4>{293.665f, 440.0f, 587.330f, 880.0f}[note],
             1.5f, 0.12f);

    effects_[static_cast<int>(EventType::Victory)] = Blank("Dawn through the arch", 3.4f);
    auto& victory = effects_[static_cast<int>(EventType::Victory)];
    for (int note = 0; note < 5; ++note)
        Bell(victory, note * 0.22f, std::array<float, 5>{293.665f, 349.228f, 440.0f, 587.330f, 880.0f}[note],
             2.3f, 0.13f);

    effects_[static_cast<int>(EventType::Defeat)] = Blank("The last ember", 2.0f);
    auto& defeat = effects_[static_cast<int>(EventType::Defeat)];
    Bell(defeat, 0, 220.0f, 1.7f, 0.15f);
    Bell(defeat, 0.24f, 174.614f, 1.6f, 0.12f);
    Bell(defeat, 0.5f, 146.832f, 1.4f, 0.13f);
}

bool Audio::Start(std::string& error) {
    if (device_ != 0) return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        error = SDL_GetError();
        return false;
    }
    Compose();
    SDL_AudioSpec requested{};
    requested.freq = kRate;
    requested.format = AUDIO_F32SYS;
    requested.channels = 2;
    requested.samples = 1024;
    requested.callback = &Audio::Callback;
    requested.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &requested, nullptr, 0);
    if (device_ == 0) {
        error = SDL_GetError();
        return false;
    }
    SDL_PauseAudioDevice(device_, 0);
    return true;
}

void Audio::Stop() {
    if (device_ == 0) return;
    SDL_CloseAudioDevice(device_);
    device_ = 0;
    voices_.fill({});
}

void Audio::Wake() {
    if (device_ != 0) SDL_PauseAudioDevice(device_, 0);
}

void Audio::SetMuted(bool muted) {
    if (device_ != 0) SDL_LockAudioDevice(device_);
    muted_ = muted;
    if (device_ != 0) SDL_UnlockAudioDevice(device_);
}

void Audio::HandleEvents(const std::vector<Event>& events) {
    if (device_ == 0 || events.empty()) return;
    SDL_LockAudioDevice(device_);
    for (const Event& event : events) {
        auto available = std::find_if(voices_.begin(), voices_.end(), [](const Voice& voice) {
            return voice.wave == nullptr;
        });
        if (available == voices_.end()) available = voices_.begin();
        const auto index = static_cast<std::size_t>(event.type);
        if (index >= effects_.size()) continue;
        *available = {&effects_[index], 0, 0.72f,
                      std::clamp((event.x - kWidth * 0.5f) / kWidth, -0.5f, 0.5f)};
    }
    SDL_UnlockAudioDevice(device_);
}

void Audio::Callback(void* userdata, std::uint8_t* bytes, int byte_count) {
    std::memset(bytes, 0, static_cast<std::size_t>(byte_count));
    static_cast<Audio*>(userdata)->Mix(reinterpret_cast<float*>(bytes), byte_count / (2 * sizeof(float)));
}

void Audio::Mix(float* output, int frames) {
    const auto ambient_frames = ambience_.Frames();
    for (int frame = 0; frame < frames; ++frame) {
        master_ += ((muted_ ? 0.0f : 0.72f) - master_) * 0.002f;
        float left = 0, right = 0;
        if (ambient_frames != 0) {
            left = ambience_.samples[ambience_frame_ * 2];
            right = ambience_.samples[ambience_frame_ * 2 + 1];
            ambience_frame_ = (ambience_frame_ + 1) % ambient_frames;
        }
        for (Voice& voice : voices_) {
            if (voice.wave == nullptr) continue;
            if (voice.cursor >= voice.wave->Frames()) {
                voice.wave = nullptr;
                continue;
            }
            const float sample = voice.wave->samples[voice.cursor++] * voice.volume;
            left += sample * (1.0f - voice.pan);
            right += sample * (1.0f + voice.pan);
        }
        // Soft limiting preserves overlapping shrine and pickup tails.
        output[frame * 2] = left * master_ / (1.0f + std::abs(left));
        output[frame * 2 + 1] = right * master_ / (1.0f + std::abs(right));
    }
}

} // namespace lanternwake
