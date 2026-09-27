#include "aether/audio/stream.h"

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#include <dr_flac.h>
#include <dr_wav.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace aether::audio {

namespace {

bool Fail(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
}

bool SupportedChannels(u32 channels, std::string* error) {
    if (channels >= 1 && channels <= 2) return true;
    return Fail(error, "only mono and stereo sounds are supported (this one has " + std::to_string(channels) + " channels)");
}

// --- Streams --------------------------------------------------------------------------------

class VorbisStream final : public AudioStream {
public:
    VorbisStream(stb_vorbis* v, std::shared_ptr<const std::vector<u8>> keep) : v_(v), keep_(std::move(keep)) {
        const stb_vorbis_info info = stb_vorbis_get_info(v_);
        rate_ = info.sample_rate;
        channels_ = static_cast<u32>(info.channels);
        frames_ = stb_vorbis_stream_length_in_samples(v_);
    }
    ~VorbisStream() override { stb_vorbis_close(v_); }
    u32 SampleRate() const override { return rate_; }
    u32 Channels() const override { return channels_; }
    u64 Frames() const override { return frames_; }
    u32 Read(f32* out, u32 frames) override {
        u32 done = 0;
        while (done < frames) {
            const int got = stb_vorbis_get_samples_float_interleaved(v_, static_cast<int>(channels_), out + static_cast<usize>(done) * channels_,
                                                                     static_cast<int>((frames - done) * channels_));
            if (got <= 0) break;
            done += static_cast<u32>(got);
        }
        return done;
    }
    bool Seek(u64 frame) override { return stb_vorbis_seek(v_, static_cast<unsigned>(std::min<u64>(frame, frames_))) != 0; }

private:
    stb_vorbis* v_;
    std::shared_ptr<const std::vector<u8>> keep_; // the bytes stb_vorbis reads from
    u32 rate_ = 0, channels_ = 0;
    u64 frames_ = 0;
};

class FlacStream final : public AudioStream {
public:
    FlacStream(drflac* f, std::shared_ptr<const std::vector<u8>> keep) : f_(f), keep_(std::move(keep)) {}
    ~FlacStream() override { drflac_close(f_); }
    u32 SampleRate() const override { return f_->sampleRate; }
    u32 Channels() const override { return f_->channels; }
    u64 Frames() const override { return f_->totalPCMFrameCount; }
    u32 Read(f32* out, u32 frames) override { return static_cast<u32>(drflac_read_pcm_frames_f32(f_, frames, out)); }
    bool Seek(u64 frame) override { return drflac_seek_to_pcm_frame(f_, std::min<u64>(frame, Frames())) != 0; }

private:
    drflac* f_;
    std::shared_ptr<const std::vector<u8>> keep_;
};

class WavStream final : public AudioStream {
public:
    explicit WavStream(std::shared_ptr<const std::vector<u8>> keep) : keep_(std::move(keep)) {}
    ~WavStream() override {
        if (open_) drwav_uninit(&w_);
    }
    bool InitMemory() { return open_ = drwav_init_memory(&w_, keep_->data(), keep_->size(), nullptr) != 0; }
    bool InitFile(const std::string& path) { return open_ = drwav_init_file(&w_, path.c_str(), nullptr) != 0; }
    u32 SampleRate() const override { return w_.sampleRate; }
    u32 Channels() const override { return w_.channels; }
    u64 Frames() const override { return w_.totalPCMFrameCount; }
    u32 Read(f32* out, u32 frames) override { return static_cast<u32>(drwav_read_pcm_frames_f32(&w_, frames, out)); }
    bool Seek(u64 frame) override { return drwav_seek_to_pcm_frame(&w_, std::min<u64>(frame, Frames())) != 0; }

private:
    drwav w_{};
    bool open_ = false;
    std::shared_ptr<const std::vector<u8>> keep_;
};

class WaveStream final : public AudioStream {
public:
    explicit WaveStream(std::shared_ptr<const SoundWave> s) : s_(std::move(s)) {}
    u32 SampleRate() const override { return s_->sample_rate; }
    u32 Channels() const override { return s_->channels; }
    u64 Frames() const override { return s_->Frames(); }
    u32 Read(f32* out, u32 frames) override {
        const u64 n = std::min<u64>(frames, Frames() - at_);
        std::copy_n(s_->samples.begin() + static_cast<std::ptrdiff_t>(at_ * s_->channels), n * s_->channels, out);
        at_ += n;
        return static_cast<u32>(n);
    }
    bool Seek(u64 frame) override {
        at_ = std::min<u64>(frame, Frames());
        return true;
    }

private:
    std::shared_ptr<const SoundWave> s_;
    u64 at_ = 0;
};

// Checks what every stream needs before the mixer gets it.
std::unique_ptr<AudioStream> Checked(std::unique_ptr<AudioStream> s, std::string* error) {
    if (!SupportedChannels(s->Channels(), error)) return nullptr;
    if (s->SampleRate() == 0) return Fail(error, "the sound has no sample rate"), nullptr;
    if (s->Frames() == 0) return Fail(error, "the sound is empty or its length is unknown"), nullptr;
    return s;
}

// Reads a whole stream into a sound.
bool ReadAll(AudioStream& s, SoundWave& out) {
    SoundWave w;
    w.sample_rate = s.SampleRate();
    w.channels = s.Channels();
    w.samples.resize(static_cast<usize>(s.Frames()) * w.channels);
    const u32 got = s.Read(w.samples.data(), static_cast<u32>(s.Frames()));
    w.samples.resize(static_cast<usize>(got) * w.channels);
    out = std::move(w);
    return true;
}

} // namespace

const char* SoundFormatName(SoundFormat f) {
    switch (f) {
    case SoundFormat::Wav: return "WAV";
    case SoundFormat::OggVorbis: return "Ogg Vorbis";
    case SoundFormat::Flac: return "FLAC";
    case SoundFormat::Unknown: break;
    }
    return "unknown";
}

SoundFormat DetectFormat(const u8* b, usize size) {
    if (size >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVE", 4) == 0) return SoundFormat::Wav;
    if (size >= 4 && std::memcmp(b, "OggS", 4) == 0) return SoundFormat::OggVorbis;
    if (size >= 4 && std::memcmp(b, "fLaC", 4) == 0) return SoundFormat::Flac;
    return SoundFormat::Unknown;
}

bool DecodeOggVorbis(const std::vector<u8>& bytes, SoundWave& out, std::string* error) {
    if (DetectFormat(bytes) != SoundFormat::OggVorbis) return Fail(error, "not an Ogg Vorbis file");
    auto s = OpenStream(std::make_shared<const std::vector<u8>>(bytes), error);
    return s != nullptr && ReadAll(*s, out);
}

bool DecodeFlac(const std::vector<u8>& bytes, SoundWave& out, std::string* error) {
    if (DetectFormat(bytes) != SoundFormat::Flac) return Fail(error, "not a FLAC file");
    auto s = OpenStream(std::make_shared<const std::vector<u8>>(bytes), error);
    return s != nullptr && ReadAll(*s, out);
}

bool DecodeSound(const std::vector<u8>& bytes, SoundWave& out, std::string* error) {
    switch (DetectFormat(bytes)) {
    case SoundFormat::Wav: return DecodeWav(bytes, out, error);
    case SoundFormat::OggVorbis: return DecodeOggVorbis(bytes, out, error);
    case SoundFormat::Flac: return DecodeFlac(bytes, out, error);
    case SoundFormat::Unknown: break;
    }
    return Fail(error, "unrecognised sound format (WAV, Ogg Vorbis and FLAC are supported)");
}

std::unique_ptr<AudioStream> OpenStream(std::shared_ptr<const std::vector<u8>> bytes, std::string* error) {
    if (bytes == nullptr) return Fail(error, "no data"), nullptr;
    switch (DetectFormat(*bytes)) {
    case SoundFormat::OggVorbis: {
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(bytes->data(), static_cast<int>(bytes->size()), &err, nullptr);
        if (v == nullptr) return Fail(error, "couldn't read the Ogg Vorbis stream (error " + std::to_string(err) + ")"), nullptr;
        return Checked(std::make_unique<VorbisStream>(v, std::move(bytes)), error);
    }
    case SoundFormat::Flac: {
        drflac* f = drflac_open_memory(bytes->data(), bytes->size(), nullptr);
        if (f == nullptr) return Fail(error, "couldn't read the FLAC stream"), nullptr;
        return Checked(std::make_unique<FlacStream>(f, std::move(bytes)), error);
    }
    case SoundFormat::Wav: {
        auto w = std::make_unique<WavStream>(std::move(bytes));
        if (!w->InitMemory()) return Fail(error, "couldn't read the WAV stream"), nullptr;
        return Checked(std::move(w), error);
    }
    case SoundFormat::Unknown: break;
    }
    return Fail(error, "unrecognised sound format (WAV, Ogg Vorbis and FLAC are supported)"), nullptr;
}

std::unique_ptr<AudioStream> OpenStreamFile(const std::string& path, std::string* error) {
    u8 header[12] = {};
    usize got = 0;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        got = std::fread(header, 1, sizeof(header), f);
        std::fclose(f);
    } else {
        return Fail(error, "couldn't open '" + path + "'"), nullptr;
    }
    switch (DetectFormat(header, got)) {
    case SoundFormat::OggVorbis: {
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_filename(path.c_str(), &err, nullptr);
        if (v == nullptr) return Fail(error, "couldn't read the Ogg Vorbis file '" + path + "'"), nullptr;
        return Checked(std::make_unique<VorbisStream>(v, nullptr), error);
    }
    case SoundFormat::Flac: {
        drflac* f = drflac_open_file(path.c_str(), nullptr);
        if (f == nullptr) return Fail(error, "couldn't read the FLAC file '" + path + "'"), nullptr;
        return Checked(std::make_unique<FlacStream>(f, nullptr), error);
    }
    case SoundFormat::Wav: {
        auto w = std::make_unique<WavStream>(nullptr);
        if (!w->InitFile(path)) return Fail(error, "couldn't read the WAV file '" + path + "'"), nullptr;
        return Checked(std::move(w), error);
    }
    case SoundFormat::Unknown: break;
    }
    return Fail(error, "'" + path + "' isn't a WAV, Ogg Vorbis or FLAC file"), nullptr;
}

std::unique_ptr<AudioStream> MakeWaveStream(std::shared_ptr<const SoundWave> sound) {
    if (sound == nullptr || sound->channels < 1 || sound->channels > 2 || sound->Frames() == 0 || sound->sample_rate == 0) return nullptr;
    return std::make_unique<WaveStream>(std::move(sound));
}

} // namespace aether::audio
