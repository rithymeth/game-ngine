#include "aether/audio/backend.h"

#include "miniaudio_config.h"

#include <miniaudio.h>

#include <chrono>

namespace aether::audio {

// --- Null ---------------------------------------------------------------------------------------

bool NullBackend::Start(const BackendConfig& config, RenderCallback render, std::string* error) {
    if (running_.load()) {
        if (error != nullptr) *error = "already running";
        return false;
    }
    config_ = config;
    config_.period_frames = std::max(config.period_frames, 1u);
    render_ = std::move(render);
    running_ = true;
    if (realtime_) {
        thread_ = std::thread([this] {
            using clock = std::chrono::steady_clock;
            const auto period = std::chrono::duration<f64>(static_cast<f64>(config_.period_frames) / std::max(config_.sample_rate, 1u));
            auto next = clock::now();
            while (running_.load()) {
                Render(config_.period_frames);
                next += std::chrono::duration_cast<clock::duration>(period);
                std::this_thread::sleep_until(next);
            }
        });
    }
    return true;
}

void NullBackend::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

bool NullBackend::Pump(u32 frames) {
    if (realtime_ || !running_.load()) return false;
    while (frames > 0) {
        const u32 n = std::min(frames, config_.period_frames);
        Render(n);
        frames -= n;
    }
    return true;
}

void NullBackend::Render(u32 frames) {
    block_.resize(static_cast<usize>(frames) * 2);
    render_(block_.data(), frames);
    pulled_.fetch_add(frames, std::memory_order_relaxed);
    if (capture_) {
        std::lock_guard<std::mutex> lock(capture_mutex_); // (not the mixer's path: only for inspection)
        captured_.insert(captured_.end(), block_.begin(), block_.end());
    }
}

std::vector<f32> NullBackend::Captured() const {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    return captured_;
}

// --- miniaudio ------------------------------------------------------------------------------------

struct MiniaudioBackend::Impl {
    ma_context context{};
    ma_device device{};
    bool context_ready = false, device_ready = false;
    RenderCallback render;
    MiniaudioBackend* owner = nullptr;

    static void Data(ma_device* device, void* output, const void*, ma_uint32 frames) {
        Impl* self = static_cast<Impl*>(device->pUserData);
        self->render(static_cast<f32*>(output), frames);
        self->owner->pulled_.fetch_add(frames, std::memory_order_relaxed);
    }
};

MiniaudioBackend::MiniaudioBackend(bool null_device) : impl_(std::make_unique<Impl>()), null_device_(null_device) { impl_->owner = this; }

MiniaudioBackend::~MiniaudioBackend() { Stop(); }

bool MiniaudioBackend::Start(const BackendConfig& config, RenderCallback render, std::string* error) {
    auto fail = [&](const std::string& message, ma_result r) {
        if (error != nullptr) *error = message + " (" + ma_result_description(r) + ")";
        Stop();
        return false;
    };
    if (running_) {
        if (error != nullptr) *error = "already running";
        return false;
    }
    impl_->render = std::move(render);
    ma_backend null_only[] = {ma_backend_null};
    ma_result r = null_device_ ? ma_context_init(null_only, 1, nullptr, &impl_->context) : ma_context_init(nullptr, 0, nullptr, &impl_->context);
    if (r != MA_SUCCESS) return fail("couldn't start the audio system", r);
    impl_->context_ready = true;
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = 2;
    dc.sampleRate = config.sample_rate; // miniaudio converts if the device runs at another rate
    dc.periodSizeInFrames = std::max(config.period_frames, 1u);
    dc.dataCallback = &Impl::Data;
    dc.pUserData = impl_.get();
    r = ma_device_init(&impl_->context, &dc, &impl_->device);
    if (r != MA_SUCCESS) return fail("couldn't open an audio output device", r);
    impl_->device_ready = true;
    char name[256] = {};
    ma_device_get_name(&impl_->device, ma_device_type_playback, name, sizeof(name), nullptr);
    device_name_ = name;
    rate_ = impl_->device.sampleRate;
    r = ma_device_start(&impl_->device);
    if (r != MA_SUCCESS) return fail("couldn't start the audio output device", r);
    running_ = true;
    return true;
}

void MiniaudioBackend::Stop() {
    if (impl_->device_ready) {
        ma_device_uninit(&impl_->device); // stops it and waits for its thread
        impl_->device_ready = false;
    }
    if (impl_->context_ready) {
        ma_context_uninit(&impl_->context);
        impl_->context_ready = false;
    }
    running_ = false;
}

// --- Output ---------------------------------------------------------------------------------------

bool AudioOutput::Start(u32 period_frames, std::string* error) {
    if (backend_ == nullptr) {
        if (error != nullptr) *error = "no backend";
        return false;
    }
    if (backend_->Running()) return true;
    mixer_.SetThreaded(true);
    BackendConfig config;
    config.sample_rate = mixer_.SampleRate();
    config.period_frames = period_frames;
    if (!backend_->Start(config, [this](f32* out, u32 frames) { mixer_.Render(out, frames); }, error)) {
        mixer_.SetThreaded(false);
        return false;
    }
    return true;
}

void AudioOutput::Stop() {
    if (backend_ != nullptr) backend_->Stop();
    mixer_.SetThreaded(false);
}

std::unique_ptr<AudioBackend> CreateDefaultBackend(u32 sample_rate) {
    // Try the device briefly: if it opens, hand over a fresh (stopped) one.
    MiniaudioBackend probe;
    BackendConfig config;
    config.sample_rate = sample_rate;
    if (probe.Start(config, [](f32* out, u32 frames) { std::fill(out, out + static_cast<usize>(frames) * 2, 0.0f); })) {
        probe.Stop();
        return std::make_unique<MiniaudioBackend>();
    }
    return std::make_unique<NullBackend>(true);
}

} // namespace aether::audio
