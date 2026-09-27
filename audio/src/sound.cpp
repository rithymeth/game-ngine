#include "aether/audio/sound.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aether::audio {

namespace {

u16 U16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 U32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

void Put16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x & 0xFF));
    v.push_back(static_cast<u8>(x >> 8));
}
void Put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>((x >> (8 * i)) & 0xFF));
}
void PutTag(std::vector<u8>& v, const char* tag) { v.insert(v.end(), tag, tag + 4); }

constexpr u16 kFormatPcm = 1, kFormatFloat = 3, kFormatExtensible = 0xFFFE;

} // namespace

bool DecodeWav(const std::vector<u8>& b, SoundWave& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error != nullptr) *error = m;
        return false;
    };
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return fail("not a WAV file");
    u16 format = 0, channels = 0, bits = 0, block_align = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    usize data_size = 0;
    for (usize at = 12; at + 8 <= b.size();) {
        const u8* chunk = b.data() + at;
        const u32 size = U32(chunk + 4);
        const usize available = b.size() - (at + 8);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || available < 16) return fail("the WAV's format chunk is cut short");
            const u8* f = chunk + 8;
            format = U16(f);
            channels = U16(f + 2);
            rate = U32(f + 4);
            block_align = U16(f + 12);
            bits = U16(f + 14);
            if (format == kFormatExtensible) {
                if (size < 40 || available < 40) return fail("the WAV's extensible format chunk is cut short");
                format = U16(f + 24); // the sub-format GUID starts with the plain format code
            }
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = chunk + 8;
            data_size = std::min<usize>(size, available); // files that claim more than they hold play what's there
        }
        at += 8 + static_cast<usize>(size) + (size & 1); // chunks are padded to even sizes
        if (data != nullptr && format != 0) break;
    }
    if (format == 0) return fail("the WAV has no format chunk");
    if (data == nullptr) return fail("the WAV has no data");
    if (format != kFormatPcm && format != kFormatFloat) return fail("unsupported WAV encoding (only PCM and 32-bit float)");
    if (channels < 1 || channels > 2) return fail("only mono and stereo WAVs are supported");
    if (rate == 0) return fail("the WAV has no sample rate");
    const bool ok_bits = format == kFormatFloat ? bits == 32 : (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    if (!ok_bits) return fail("unsupported sample size (" + std::to_string(bits) + " bits)");
    const u32 bytes = bits / 8;
    if (block_align != bytes * channels) return fail("the WAV's block size doesn't match its format");
    const usize count = data_size / bytes / channels * channels;
    SoundWave s;
    s.sample_rate = rate;
    s.channels = channels;
    s.samples.resize(count);
    for (usize i = 0; i < count; ++i) {
        const u8* p = data + i * bytes;
        f32 v = 0.0f;
        if (format == kFormatFloat) {
            std::memcpy(&v, p, 4);
        } else if (bits == 8) {
            v = (static_cast<f32>(p[0]) - 128.0f) / 128.0f;
        } else if (bits == 16) {
            v = static_cast<f32>(static_cast<i16>(U16(p))) / 32768.0f;
        } else if (bits == 24) {
            i32 x = static_cast<i32>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (x & 0x800000) x |= ~0xFFFFFF; // sign-extend
            v = static_cast<f32>(x) / 8388608.0f;
        } else {
            v = static_cast<f32>(static_cast<i32>(U32(p))) / 2147483648.0f;
        }
        s.samples[i] = v;
    }
    out = std::move(s);
    return true;
}

std::vector<u8> EncodeWav(const SoundWave& s, u32 bits) {
    const bool is_float = bits == 32;
    const u32 bytes = is_float ? 4 : 2;
    const u32 channels = std::max(s.channels, 1u);
    const u32 data_size = static_cast<u32>(s.samples.size() * bytes);
    std::vector<u8> v;
    PutTag(v, "RIFF");
    Put32(v, 36 + data_size + (data_size & 1));
    PutTag(v, "WAVE");
    PutTag(v, "fmt ");
    Put32(v, 16);
    Put16(v, is_float ? kFormatFloat : kFormatPcm);
    Put16(v, static_cast<u16>(channels));
    Put32(v, s.sample_rate);
    Put32(v, s.sample_rate * channels * bytes);
    Put16(v, static_cast<u16>(channels * bytes));
    Put16(v, static_cast<u16>(bytes * 8));
    PutTag(v, "data");
    Put32(v, data_size);
    for (f32 x : s.samples) {
        if (is_float) {
            u32 raw;
            std::memcpy(&raw, &x, 4);
            Put32(v, raw);
        } else {
            const f32 c = std::clamp(x, -1.0f, 1.0f);
            Put16(v, static_cast<u16>(static_cast<i16>(std::lround(c * 32767.0f))));
        }
    }
    if (data_size & 1) v.push_back(0);
    return v;
}

SoundWave GenerateTone(f32 frequency, f32 seconds, u32 sample_rate, f32 amplitude, u32 channels) {
    SoundWave s;
    s.name = "Tone";
    s.sample_rate = sample_rate;
    s.channels = std::clamp(channels, 1u, 2u);
    const usize frames = static_cast<usize>(std::max(0.0f, seconds) * static_cast<f32>(sample_rate));
    s.samples.resize(frames * s.channels);
    for (usize i = 0; i < frames; ++i) {
        const f32 v = amplitude * std::sin(2.0f * 3.14159265358979f * frequency * static_cast<f32>(i) / static_cast<f32>(sample_rate));
        for (u32 c = 0; c < s.channels; ++c) s.samples[i * s.channels + c] = v;
    }
    return s;
}

} // namespace aether::audio
