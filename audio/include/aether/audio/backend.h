#pragma once

#include "aether/audio/mixer.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aether::audio {

// Audio output (Phase 17 step 5, docs/design/PHASE_SPECS.md §17.5): a
// backend owns the thread that asks for audio, and calls back for each block
// of interleaved stereo it needs.
using RenderCallback = std::function<void(f32* stereo, u32 frames)>;

struct BackendConfig {
    u32 sample_rate = 48000;
    u32 period_frames = 480; // 10 ms at 48 kHz
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual const char* Name() const = 0;
    virtual bool Start(const BackendConfig& config, RenderCallback render, std::string* error = nullptr) = 0;
    virtual void Stop() = 0;
    virtual bool Running() const = 0;
    // Frames asked for since Start (from any thread).
    u64 FramesPulled() const { return pulled_.load(std::memory_order_relaxed); }

protected:
    std::atomic<u64> pulled_{0};
};

// No device: for servers, tests and machines without audio.
// - Real-time: a thread asks for one period every period's worth of time.
// - Manual: nothing happens until Pump, which renders on the calling thread
//   (tests drive it from a thread of their own).
// With `capture`, what it's given is kept for inspection.
class NullBackend final : public AudioBackend {
public:
    explicit NullBackend(bool realtime = true, bool capture = false) : realtime_(realtime), capture_(capture) {}
    ~NullBackend() override { Stop(); }
    const char* Name() const override { return "null"; }
    bool Start(const BackendConfig& config, RenderCallback render, std::string* error = nullptr) override;
    void Stop() override;
    bool Running() const override { return running_.load(); }
    // Manual mode: renders `frames` now (in periods). False when not running or real-time.
    bool Pump(u32 frames);
    // The captured output (copy; safe while running).
    std::vector<f32> Captured() const;

private:
    void Render(u32 frames);
    bool realtime_, capture_;
    BackendConfig config_;
    RenderCallback render_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<f32> block_;
    mutable std::mutex capture_mutex_;
    std::vector<f32> captured_;
};

// The platform's audio device through miniaudio: WASAPI, Core Audio, ALSA,
// PulseAudio and so on, chosen at run time. `null_device` uses miniaudio's
// own null device instead (no sound, same threading), for tests.
class MiniaudioBackend final : public AudioBackend {
public:
    explicit MiniaudioBackend(bool null_device = false);
    ~MiniaudioBackend() override;
    const char* Name() const override { return "miniaudio"; }
    bool Start(const BackendConfig& config, RenderCallback render, std::string* error = nullptr) override;
    void Stop() override;
    bool Running() const override { return running_; }
    // The device's name and its actual rate and period (after Start).
    const std::string& DeviceName() const { return device_name_; }
    u32 SampleRate() const { return rate_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool null_device_;
    bool running_ = false;
    std::string device_name_;
    u32 rate_ = 0;
};

// The mixer playing through a backend: switches the mixer to threaded and
// renders it from the backend's thread. Stopped (and the mixer back on one
// thread) when destroyed. The mixer must outlive it.
class AudioOutput {
public:
    AudioOutput(Mixer& mixer, std::unique_ptr<AudioBackend> backend) : mixer_(mixer), backend_(std::move(backend)) {}
    ~AudioOutput() { Stop(); }
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;
    bool Start(u32 period_frames = 480, std::string* error = nullptr);
    void Stop();
    bool Running() const { return backend_ != nullptr && backend_->Running(); }
    AudioBackend& Backend() { return *backend_; }

private:
    Mixer& mixer_;
    std::unique_ptr<AudioBackend> backend_;
};

// The device if there is one (miniaudio), otherwise a real-time null backend.
std::unique_ptr<AudioBackend> CreateDefaultBackend(u32 sample_rate = 48000);

} // namespace aether::audio
