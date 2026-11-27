#pragma once

#include "game.h"
#include "aether/audio/sound.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lanternwake {

// Original game music and effects stored in Aether SoundWaves. SDL only
// supplies the device callback; no game or renderer work runs on that thread.
class Audio {
public:
    Audio() = default;
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool Start(std::string& error);
    void Stop();
    void Wake();
    void SetMuted(bool muted);
    [[nodiscard]] bool Muted() const { return muted_; }
    [[nodiscard]] bool Ready() const { return device_ != 0; }
    void HandleEvents(const std::vector<Event>& events);

private:
    struct Voice {
        const aether::audio::SoundWave* wave = nullptr;
        std::size_t cursor = 0;
        float volume = 1, pan = 0;
    };
    std::uint32_t device_ = 0;
    bool muted_ = false;
    std::array<aether::audio::SoundWave, 8> effects_;
    aether::audio::SoundWave ambience_;
    std::array<Voice, 24> voices_{};
    std::size_t ambience_frame_ = 0;
    float master_ = 0;

    void Compose();
    void Mix(float* output, int frames);
    static void Callback(void* userdata, std::uint8_t* bytes, int byte_count);
};

} // namespace lanternwake
