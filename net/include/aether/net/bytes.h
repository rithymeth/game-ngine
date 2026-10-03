#pragma once

#include "aether/core/base.h"

#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace aether::net {

// Little-endian byte serialization for packets. The reader never reads past
// the end: a short or malformed buffer sets a sticky failure flag and returns
// zeros, so callers check Ok() once after decoding.

class ByteWriter {
public:
    void U8(u8 v) { bytes_.push_back(v); }
    void U16(u16 v) { Raw(&v, 2); }
    void U32(u32 v) { Raw(&v, 4); }
    void U64(u64 v) { Raw(&v, 8); }
    void I32(i32 v) { Raw(&v, 4); }
    void F32(f32 v) { Raw(&v, 4); }
    void F64(f64 v) { Raw(&v, 8); }
    void Bool(bool v) { U8(v ? 1 : 0); }
    // Variable-length unsigned: 7 bits per byte, small values take one byte.
    void Varint(u64 v) {
        while (v >= 0x80) {
            U8(static_cast<u8>(v | 0x80));
            v >>= 7;
        }
        U8(static_cast<u8>(v));
    }
    void String(const std::string& s) {
        Varint(s.size());
        Bytes({reinterpret_cast<const u8*>(s.data()), s.size()});
    }
    void Bytes(std::span<const u8> b) { bytes_.insert(bytes_.end(), b.begin(), b.end()); }

    const std::vector<u8>& Data() const { return bytes_; }
    usize Size() const { return bytes_.size(); }
    std::vector<u8> Take() { return std::move(bytes_); }

private:
    void Raw(const void* p, usize n) {
        const u8* b = static_cast<const u8*>(p);
        bytes_.insert(bytes_.end(), b, b + n); // little-endian hosts only (x86/ARM)
    }
    std::vector<u8> bytes_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const u8> data) : data_(data) {}

    u8 U8() { return Read<u8>(); }
    u16 U16() { return Read<u16>(); }
    u32 U32() { return Read<u32>(); }
    u64 U64() { return Read<u64>(); }
    i32 I32() { return Read<i32>(); }
    f32 F32() { return Read<f32>(); }
    f64 F64() { return Read<f64>(); }
    bool Bool() { return U8() != 0; }
    u64 Varint() {
        u64 v = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            const u8 b = U8();
            if (!ok_) return 0;
            if (shift == 63 && b > 1) return Fail<u64>(); // overflows 64 bits
            v |= static_cast<u64>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        return Fail<u64>();
    }
    std::string String() {
        const u64 n = Varint();
        if (!ok_ || n > Remaining()) return Fail<std::string>();
        std::string s(reinterpret_cast<const char*>(data_.data() + pos_), static_cast<usize>(n));
        pos_ += static_cast<usize>(n);
        return s;
    }
    std::span<const u8> Bytes(usize n) {
        if (!ok_ || n > Remaining()) return Fail<std::span<const u8>>();
        std::span<const u8> out = data_.subspan(pos_, n);
        pos_ += n;
        return out;
    }

    bool Ok() const { return ok_; }
    usize Remaining() const { return data_.size() - pos_; }

private:
    template <typename T>
    T Read() {
        if (!ok_ || sizeof(T) > Remaining()) return Fail<T>();
        T v;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    template <typename T>
    T Fail() {
        ok_ = false;
        return T{};
    }
    std::span<const u8> data_;
    usize pos_ = 0;
    bool ok_ = true;
};

} // namespace aether::net
