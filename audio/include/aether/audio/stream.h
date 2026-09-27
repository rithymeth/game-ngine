#pragma once

#include "aether/audio/sound.h"

#include <memory>
#include <string>
#include <vector>

namespace aether::audio {

// Decoding and streaming (Phase 17 step 3, docs/design/PHASE_SPECS.md §17.3).
// WAV, Ogg Vorbis and FLAC decode whole into a SoundWave, or play as a
// stream that decodes as the mixer needs it: from compressed bytes held in
// memory, or from a file on disk. Mono and stereo only.

enum class SoundFormat : u8 { Unknown, Wav, OggVorbis, Flac };
const char* SoundFormatName(SoundFormat format);
// From the first bytes of a file.
SoundFormat DetectFormat(const u8* bytes, usize size);
inline SoundFormat DetectFormat(const std::vector<u8>& bytes) { return DetectFormat(bytes.data(), bytes.size()); }

bool DecodeOggVorbis(const std::vector<u8>& bytes, SoundWave& out, std::string* error = nullptr);
bool DecodeFlac(const std::vector<u8>& bytes, SoundWave& out, std::string* error = nullptr);
// Any supported format, by its header.
bool DecodeSound(const std::vector<u8>& bytes, SoundWave& out, std::string* error = nullptr);

// Frames decoded on demand. Not thread-safe; the mixer owns a voice's stream.
class AudioStream {
public:
    virtual ~AudioStream() = default;
    virtual u32 SampleRate() const = 0;
    virtual u32 Channels() const = 0;
    virtual u64 Frames() const = 0; // the total length
    // Reads up to `frames` interleaved frames; fewer only at the end.
    virtual u32 Read(f32* out, u32 frames) = 0;
    virtual bool Seek(u64 frame) = 0;
    f32 Duration() const { return SampleRate() == 0 ? 0.0f : static_cast<f32>(static_cast<f64>(Frames()) / SampleRate()); }
};

// A stream over compressed bytes in memory, shared so many voices can stream one asset.
std::unique_ptr<AudioStream> OpenStream(std::shared_ptr<const std::vector<u8>> bytes, std::string* error = nullptr);
// A stream reading from a file as it plays.
std::unique_ptr<AudioStream> OpenStreamFile(const std::string& path, std::string* error = nullptr);
// A stream over an already decoded sound (tests, and generated audio).
std::unique_ptr<AudioStream> MakeWaveStream(std::shared_ptr<const SoundWave> sound);

} // namespace aether::audio
