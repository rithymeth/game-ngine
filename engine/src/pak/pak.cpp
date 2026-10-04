#include "aether/pak/pak.h"

#include "aether/core/log.h"

#include <lz4.h>
#include <nlohmann/json.hpp>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace aether::pak {

namespace {

constexpr char kMagic[4] = {'A', 'P', 'A', 'K'};
constexpr u32 kVersion = 1;
constexpr usize kHeaderSize = 40;
// Entries smaller than this get LZ4 under the Auto policy (fast to read,
// and zstd's advantage is small there); larger ones get zstd.
constexpr usize kAutoZstdThreshold = 16 * 1024;

const std::array<u32, 256>& CrcTable() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

void Put16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}
void Put32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void Put64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
u64 Get(const u8* p, int bytes) {
    u64 v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
    return v;
}

// --- ChaCha20 (RFC 8439) -------------------------------------------------------

inline u32 Rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

inline void QuarterRound(u32* x, int a, int b, int c, int d) {
    x[a] += x[b]; x[d] ^= x[a]; x[d] = Rotl(x[d], 16);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = Rotl(x[b], 12);
    x[a] += x[b]; x[d] ^= x[a]; x[d] = Rotl(x[d], 8);
    x[c] += x[d]; x[b] ^= x[c]; x[b] = Rotl(x[b], 7);
}

u32 Load32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

void ChaChaBlock(const PakKey& key, const std::array<u8, 12>& nonce, u32 counter, u8 out[64]) {
    u32 state[16] = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
    for (int i = 0; i < 8; ++i) state[4 + i] = Load32(key.bytes.data() + 4 * i);
    state[12] = counter;
    for (int i = 0; i < 3; ++i) state[13 + i] = Load32(nonce.data() + 4 * i);
    u32 x[16];
    std::memcpy(x, state, sizeof(x));
    for (int round = 0; round < 10; ++round) {
        QuarterRound(x, 0, 4, 8, 12);
        QuarterRound(x, 1, 5, 9, 13);
        QuarterRound(x, 2, 6, 10, 14);
        QuarterRound(x, 3, 7, 11, 15);
        QuarterRound(x, 0, 5, 10, 15);
        QuarterRound(x, 1, 6, 11, 12);
        QuarterRound(x, 2, 7, 8, 13);
        QuarterRound(x, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; ++i) {
        const u32 v = x[i] + state[i];
        out[4 * i] = static_cast<u8>(v);
        out[4 * i + 1] = static_cast<u8>(v >> 8);
        out[4 * i + 2] = static_cast<u8>(v >> 16);
        out[4 * i + 3] = static_cast<u8>(v >> 24);
    }
}

// An entry's (or the index's) nonce: its offset in the file and a tag.
std::array<u8, 12> NonceFor(u64 offset, const char tag[4]) {
    std::array<u8, 12> n{};
    for (int i = 0; i < 8; ++i) n[static_cast<usize>(i)] = static_cast<u8>(offset >> (8 * i));
    std::memcpy(n.data() + 8, tag, 4);
    return n;
}

constexpr u32 kFlagEncrypted = 1u;

void SetError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// Compresses with one method; empty when it fails or doesn't shrink enough.
std::vector<u8> CompressWith(Compression method, std::span<const u8> in, int zstd_level) {
    std::vector<u8> out;
    if (in.empty()) return out;
    if (method == Compression::LZ4) {
        if (in.size() > static_cast<usize>(LZ4_MAX_INPUT_SIZE)) return out;
        out.resize(static_cast<usize>(LZ4_compressBound(static_cast<int>(in.size()))));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(in.data()), reinterpret_cast<char*>(out.data()),
                                           static_cast<int>(in.size()), static_cast<int>(out.size()));
        out.resize(n > 0 ? static_cast<usize>(n) : 0);
    } else if (method == Compression::Zstd) {
        out.resize(ZSTD_compressBound(in.size()));
        const usize n = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), zstd_level);
        out.resize(ZSTD_isError(n) ? 0 : n);
    }
    // Worth it only if it saves at least 1/32 of the size.
    if (!out.empty() && out.size() > in.size() - in.size() / 32) out.clear();
    return out;
}

bool Decompress(Compression method, std::span<const u8> in, u64 size, std::vector<u8>& out) {
    out.resize(static_cast<usize>(size));
    switch (method) {
        case Compression::None:
            if (in.size() != size) return false;
            if (size) std::memcpy(out.data(), in.data(), static_cast<usize>(size));
            return true;
        case Compression::LZ4: {
            if (size > static_cast<u64>(LZ4_MAX_INPUT_SIZE) || in.size() > static_cast<usize>(LZ4_MAX_INPUT_SIZE)) return false;
            const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(in.data()), reinterpret_cast<char*>(out.data()),
                                              static_cast<int>(in.size()), static_cast<int>(size));
            return n >= 0 && static_cast<u64>(n) == size;
        }
        case Compression::Zstd: {
            const usize n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
            return !ZSTD_isError(n) && n == size;
        }
    }
    return false;
}

} // namespace

void ChaCha20Xor(const PakKey& key, const std::array<u8, 12>& nonce, u32 counter, std::span<u8> data) {
    u8 block[64];
    for (usize at = 0; at < data.size(); at += 64, ++counter) {
        ChaChaBlock(key, nonce, counter, block);
        const usize n = std::min<usize>(64, data.size() - at);
        for (usize i = 0; i < n; ++i) data[at + i] ^= block[i];
    }
}

bool PakKey::FromHex(std::string_view hex, PakKey& out) {
    if (hex.size() != 64) return false;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    PakKey key;
    for (usize i = 0; i < 32; ++i) {
        const int hi = digit(hex[2 * i]), lo = digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        key.bytes[i] = static_cast<u8>(hi * 16 + lo);
    }
    out = key;
    return true;
}

std::string PakKey::ToHex() const {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (u8 b : bytes) {
        hex.push_back(digits[b >> 4]);
        hex.push_back(digits[b & 15]);
    }
    return hex;
}

PakKey PakKey::Generate() {
    std::random_device rd;
    PakKey key;
    for (usize i = 0; i < 32; i += 4) {
        const u32 v = rd();
        for (usize k = 0; k < 4; ++k) key.bytes[i + k] = static_cast<u8>(v >> (8 * k));
    }
    return key;
}

u32 PakKey::Id() const {
    u8 block[64];
    ChaChaBlock(*this, NonceFor(~0ull, "KYID"), 0, block);
    const u32 id = Load32(block);
    return id == 0 ? 1 : id;
}

const char* CompressionName(Compression c) {
    switch (c) {
        case Compression::None: return "none";
        case Compression::LZ4: return "lz4";
        case Compression::Zstd: return "zstd";
    }
    return "?";
}

u32 Crc32(std::span<const u8> bytes, u32 crc) {
    const auto& table = CrcTable();
    crc = ~crc;
    for (u8 b : bytes) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::string NormalizePath(const std::string& path) {
    std::string out;
    std::string segment;
    const auto flush = [&]() -> bool {
        if (segment.empty() || segment == ".") {
            segment.clear();
            return true;
        }
        if (segment == "..") return false;
        if (!out.empty()) out += '/';
        out += segment;
        segment.clear();
        return true;
    };
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!flush()) return {};
        } else {
            segment += c;
        }
    }
    if (!flush()) return {};
    return out;
}

// --- PakWriter ---------------------------------------------------------------

bool PakWriter::Add(const std::string& path, std::span<const u8> bytes) {
    const std::string normalized = NormalizePath(path);
    if (normalized.empty() || normalized.size() > 0xFFFF) return false;
    for (Pending& p : pending_) {
        if (p.path == normalized) {
            p.bytes.assign(bytes.begin(), bytes.end());
            return true;
        }
    }
    pending_.push_back({normalized, std::vector<u8>(bytes.begin(), bytes.end())});
    return true;
}

bool PakWriter::Add(const std::string& path, const std::string& text) {
    return Add(path, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

bool PakWriter::AddFile(const std::string& path, const std::string& file_on_disk) {
    std::ifstream in(file_on_disk, std::ios::binary);
    if (!in) return false;
    std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Add(path, bytes);
}

usize PakWriter::AddDirectory(const std::string& directory, const std::string& prefix) {
    namespace sfs = std::filesystem;
    std::error_code ec;
    if (!sfs::is_directory(directory, ec)) return 0;
    std::vector<sfs::path> files;
    for (auto it = sfs::recursive_directory_iterator(directory, ec); !ec && it != sfs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end()); // deterministic archives
    usize added = 0;
    for (const sfs::path& f : files) {
        const std::string rel = sfs::relative(f, directory, ec).generic_string();
        if (AddFile(prefix.empty() ? rel : prefix + "/" + rel, f.string())) ++added;
    }
    return added;
}

u64 PakWriter::OriginalBytes() const {
    u64 total = 0;
    for (const Pending& p : pending_) total += p.bytes.size();
    return total;
}

std::vector<u8> PakWriter::Build() const {
    std::vector<u8> out(kHeaderSize, 0);
    std::vector<PakEntry> entries;
    entries.reserve(pending_.size());
    for (const Pending& p : pending_) {
        PakEntry e;
        e.path = p.path;
        e.size = p.bytes.size();
        e.crc32 = Crc32(p.bytes);
        std::vector<u8> packed;
        Compression method = Compression::None;
        switch (policy_) {
            case CompressionPolicy::None: break;
            case CompressionPolicy::LZ4: method = Compression::LZ4; break;
            case CompressionPolicy::Zstd: method = Compression::Zstd; break;
            case CompressionPolicy::Auto:
                method = p.bytes.size() >= kAutoZstdThreshold ? Compression::Zstd : Compression::LZ4;
                break;
        }
        if (method != Compression::None) packed = CompressWith(method, p.bytes, zstd_level_);
        e.offset = out.size();
        if (packed.empty()) {
            e.compression = Compression::None;
            e.stored_size = p.bytes.size();
            out.insert(out.end(), p.bytes.begin(), p.bytes.end());
        } else {
            e.compression = method;
            e.stored_size = packed.size();
            out.insert(out.end(), packed.begin(), packed.end());
        }
        if (encrypt_) {
            ChaCha20Xor(key_, NonceFor(e.offset, "DATA"), 0,
                        std::span<u8>(out.data() + e.offset, static_cast<usize>(e.stored_size)));
        }
        entries.push_back(std::move(e));
    }

    std::vector<u8> index;
    for (const PakEntry& e : entries) {
        Put16(index, static_cast<u16>(e.path.size()));
        index.insert(index.end(), e.path.begin(), e.path.end());
        index.push_back(static_cast<u8>(e.compression));
        index.push_back(0);
        Put16(index, 0);
        Put64(index, e.offset);
        Put64(index, e.stored_size);
        Put64(index, e.size);
        Put32(index, e.crc32);
    }
    const u64 index_offset = out.size();
    if (encrypt_) ChaCha20Xor(key_, NonceFor(index_offset, "INDX"), 0, index);
    out.insert(out.end(), index.begin(), index.end());

    std::vector<u8> header;
    header.insert(header.end(), kMagic, kMagic + 4);
    Put32(header, kVersion);
    Put32(header, static_cast<u32>(entries.size()));
    Put32(header, encrypt_ ? kFlagEncrypted : 0u);
    Put64(header, index_offset);
    Put64(header, index.size());
    Put32(header, Crc32(index)); // of the index as stored (encrypted or not)
    Put32(header, encrypt_ ? key_.Id() : 0u);
    std::memcpy(out.data(), header.data(), kHeaderSize);
    return out;
}

bool PakWriter::Write(const std::string& file, std::string* error) const {
    const std::vector<u8> bytes = Build();
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(file).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    // Write beside it, then rename: a crash never leaves half an archive.
    const std::string temp = file + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            SetError(error, "can't write " + temp);
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            SetError(error, "failed writing " + temp);
            return false;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        SetError(error, "can't replace " + file);
        return false;
    }
    return true;
}

// --- PakReader ---------------------------------------------------------------

bool PakReader::Open(const std::string& file, std::string* error, std::span<const PakKey> keys) {
    *this = PakReader();
    file_ = file;
    return Parse(error, keys);
}

bool PakReader::OpenMemory(std::vector<u8> bytes, std::string* error, std::span<const PakKey> keys) {
    *this = PakReader();
    memory_ = std::move(bytes);
    in_memory_ = true;
    file_ = "<memory>";
    return Parse(error, keys);
}

bool PakReader::Parse(std::string* error, std::span<const PakKey> keys) {
    // The header, and the total size.
    u8 header[kHeaderSize];
    u64 file_size = 0;
    std::vector<u8> index;
    std::ifstream in;
    if (in_memory_) {
        file_size = memory_.size();
        if (file_size < kHeaderSize) {
            SetError(error, "too small to be an archive");
            return false;
        }
        std::memcpy(header, memory_.data(), kHeaderSize);
    } else {
        in.open(file_, std::ios::binary);
        if (!in) {
            SetError(error, "can't open " + file_);
            return false;
        }
        in.seekg(0, std::ios::end);
        file_size = static_cast<u64>(in.tellg());
        in.seekg(0);
        if (file_size < kHeaderSize || !in.read(reinterpret_cast<char*>(header), kHeaderSize)) {
            SetError(error, "too small to be an archive");
            return false;
        }
    }
    if (std::memcmp(header, kMagic, 4) != 0) {
        SetError(error, "not an .apak archive");
        return false;
    }
    if (Get(header + 4, 4) != kVersion) {
        SetError(error, "unsupported archive version " + std::to_string(Get(header + 4, 4)));
        return false;
    }
    const u64 flags = Get(header + 12, 4);
    if (flags & ~static_cast<u64>(kFlagEncrypted)) {
        SetError(error, "the archive uses features this engine doesn't know (flags " + std::to_string(flags) + ")");
        return false;
    }
    if (flags & kFlagEncrypted) {
        const u32 key_id = static_cast<u32>(Get(header + 36, 4));
        const auto key = std::find_if(keys.begin(), keys.end(), [&](const PakKey& k) { return k.Id() == key_id; });
        if (key == keys.end()) {
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", key_id);
            SetError(error, std::string("the archive is encrypted, and no key given is its key (key id ") + id + ")");
            return false;
        }
        encrypted_ = true;
        key_ = *key;
    }
    const u64 count = Get(header + 8, 4);
    const u64 index_offset = Get(header + 16, 8);
    const u64 index_size = Get(header + 24, 8);
    const u32 index_crc = static_cast<u32>(Get(header + 32, 4));
    if (index_offset < kHeaderSize || index_offset > file_size || index_size > file_size - index_offset) {
        SetError(error, "the index lies outside the file (truncated?)");
        return false;
    }
    index.resize(static_cast<usize>(index_size));
    if (in_memory_) {
        if (index_size) std::memcpy(index.data(), memory_.data() + index_offset, static_cast<usize>(index_size));
    } else {
        in.seekg(static_cast<std::streamoff>(index_offset));
        if (index_size && !in.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index_size))) {
            SetError(error, "can't read the index");
            return false;
        }
    }
    if (Crc32(index) != index_crc) {
        SetError(error, "the index is damaged (CRC mismatch)");
        return false;
    }
    if (encrypted_) ChaCha20Xor(key_, NonceFor(index_offset, "INDX"), 0, index);

    usize p = 0;
    entries_.reserve(static_cast<usize>(count));
    for (u64 i = 0; i < count; ++i) {
        if (index.size() - p < 2) {
            SetError(error, "the index ends early");
            return false;
        }
        const usize len = static_cast<usize>(Get(index.data() + p, 2));
        p += 2;
        if (index.size() - p < len + 32) {
            SetError(error, "the index ends early");
            return false;
        }
        PakEntry e;
        e.path.assign(reinterpret_cast<const char*>(index.data() + p), len);
        p += len;
        const u8 method = index[p];
        p += 4;
        e.offset = Get(index.data() + p, 8);
        e.stored_size = Get(index.data() + p + 8, 8);
        e.size = Get(index.data() + p + 16, 8);
        e.crc32 = static_cast<u32>(Get(index.data() + p + 24, 4));
        p += 28;
        if (method > static_cast<u8>(Compression::Zstd)) {
            SetError(error, "'" + e.path + "' uses an unknown compression");
            return false;
        }
        e.compression = static_cast<Compression>(method);
        if (e.offset < kHeaderSize || e.offset > index_offset || e.stored_size > index_offset - e.offset ||
            e.path.empty() || NormalizePath(e.path) != e.path) {
            SetError(error, "entry '" + e.path + "' is malformed");
            return false;
        }
        index_[e.path] = entries_.size();
        entries_.push_back(std::move(e));
    }
    open_ = true;
    return true;
}

const PakEntry* PakReader::Find(const std::string& path) const {
    const auto it = index_.find(NormalizePath(path));
    return it == index_.end() ? nullptr : &entries_[it->second];
}

bool PakReader::ReadStored(const PakEntry& entry, std::vector<u8>& out) const {
    out.resize(static_cast<usize>(entry.stored_size));
    if (in_memory_) {
        if (entry.stored_size) std::memcpy(out.data(), memory_.data() + entry.offset, out.size());
    } else {
        std::ifstream in(file_, std::ios::binary);
        if (!in) return false;
        in.seekg(static_cast<std::streamoff>(entry.offset));
        if (entry.stored_size != 0 &&
            !in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
            return false;
        }
    }
    if (encrypted_) ChaCha20Xor(key_, NonceFor(entry.offset, "DATA"), 0, out);
    return true;
}

bool PakReader::Read(const std::string& path, std::vector<u8>& out, std::string* error) const {
    const PakEntry* entry = Find(path);
    if (!entry) {
        SetError(error, "'" + path + "' is not in " + file_);
        return false;
    }
    std::vector<u8> stored;
    if (!ReadStored(*entry, stored)) {
        SetError(error, "can't read '" + entry->path + "' from " + file_);
        return false;
    }
    if (!Decompress(entry->compression, stored, entry->size, out)) {
        SetError(error, "'" + entry->path + "' doesn't decompress (" + CompressionName(entry->compression) + ")");
        return false;
    }
    if (Crc32(out) != entry->crc32) {
        SetError(error, "'" + entry->path + "' is damaged (CRC mismatch)");
        return false;
    }
    return true;
}

bool PakReader::ReadText(const std::string& path, std::string& out, std::string* error) const {
    std::vector<u8> bytes;
    if (!Read(path, bytes, error)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

std::vector<std::string> PakReader::Verify() const {
    std::vector<std::string> damaged;
    std::vector<u8> scratch;
    for (const PakEntry& e : entries_) {
        if (!Read(e.path, scratch)) damaged.push_back(e.path);
    }
    return damaged;
}

// --- Patches -----------------------------------------------------------------

PatchReport MakePatch(const PakReader& base, const PakReader& updated, PakWriter& patch) {
    PatchReport report;
    std::vector<u8> bytes;
    for (const PakEntry& e : updated.Entries()) {
        if (e.path == kRemovedListPath) continue;
        const PakEntry* old = base.Find(e.path);
        if (old && old->size == e.size && old->crc32 == e.crc32) {
            ++report.unchanged;
            continue;
        }
        if (!updated.Read(e.path, bytes)) continue; // damaged: Verify reports it
        patch.Add(e.path, bytes);
        (old ? report.changed : report.added).push_back(e.path);
    }
    for (const PakEntry& e : base.Entries()) {
        if (e.path != kRemovedListPath && !updated.Find(e.path)) report.removed.push_back(e.path);
    }
    if (!report.removed.empty()) patch.Add(kRemovedListPath, nlohmann::json(report.removed).dump(1));
    return report;
}

} // namespace aether::pak
