#pragma once

#include "aether/core/base.h"

#include <array>
#include <span>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

// .apak archives (Phase 25 step 1, docs/design/PHASE_SPECS.md §25.1): the
// packaged game's content in a few files instead of thousands. Each entry is
// stored raw, LZ4- or zstd-compressed, with a CRC-32 of its original bytes;
// an index at the end maps paths to entries.
//
// Layout (little-endian):
//   header   "APAK", u32 version, u32 entry count, u32 flags,
//            u64 index offset, u64 index size, u32 index CRC-32, u32 0
//   data     each entry's stored bytes, back to back
//   index    per entry: u16 path length, path bytes (UTF-8, '/'-separated,
//            no leading '/'), u8 compression, u8 0, u16 0, u64 offset,
//            u64 stored size, u64 original size, u32 CRC-32
namespace aether::pak {

// Encryption (Phase 25 step 6, §25.6): with a key, every entry's stored
// bytes and the index are encrypted with ChaCha20 (RFC 8439), each under its
// own nonce (its offset). The header's flags mark the archive encrypted and
// its last field holds the key's id, so a wrong key is reported as such.
// It keeps content from being read with an archive tool; it isn't DRM: the
// key ships with the game.
struct PakKey {
    std::array<u8, 32> bytes{};

    // 64 hex digits; false for anything else.
    static bool FromHex(std::string_view hex, PakKey& out);
    std::string ToHex() const;
    // A new random key.
    static PakKey Generate();
    // A fingerprint of the key (not the key): what archives record.
    u32 Id() const;
};

// XORs `data` with the ChaCha20 keystream for (key, nonce, counter): it both
// encrypts and decrypts.
void ChaCha20Xor(const PakKey& key, const std::array<u8, 12>& nonce, u32 counter, std::span<u8> data);

enum class Compression : u8 { None = 0, LZ4 = 1, Zstd = 2 };
const char* CompressionName(Compression c);

struct PakEntry {
    std::string path;
    Compression compression = Compression::None;
    u64 offset = 0;        // of its stored bytes, from the start of the file
    u64 stored_size = 0;   // as stored (compressed, or raw)
    u64 size = 0;          // as read back
    u32 crc32 = 0;         // of the bytes as read back
};

u32 Crc32(std::span<const u8> bytes, u32 crc = 0);
// Normalizes an archive path: backslashes to '/', no leading "./" or '/',
// no empty or "." segments. Empty when the path is invalid (it has "..").
std::string NormalizePath(const std::string& path);

// How to compress what's added: one method, or Auto (zstd for large
// entries, LZ4 for small ones), or None. Whatever is chosen, an entry
// that doesn't shrink by at least 1/32 is stored raw.
enum class CompressionPolicy : u8 { None, LZ4, Zstd, Auto };

class PakWriter {
public:
    explicit PakWriter(CompressionPolicy policy = CompressionPolicy::Auto, int zstd_level = 9)
        : policy_(policy), zstd_level_(zstd_level) {}

    // Encrypts the archive with `key` (see PakKey).
    void SetEncryption(const PakKey& key) {
        key_ = key;
        encrypt_ = true;
    }

    // Adds (or replaces) an entry. False for an invalid path.
    bool Add(const std::string& path, std::span<const u8> bytes);
    bool Add(const std::string& path, const std::string& text);
    // Adds a file from disk under `path`; false when it can't be read.
    bool AddFile(const std::string& path, const std::string& file_on_disk);
    // Every regular file under `directory`, under `prefix/` + its relative path.
    usize AddDirectory(const std::string& directory, const std::string& prefix = "");

    // Writes the archive; false (and `error`) on failure.
    bool Write(const std::string& file, std::string* error = nullptr) const;
    // The archive's bytes in memory.
    std::vector<u8> Build() const;

    usize EntryCount() const { return pending_.size(); }
    u64 OriginalBytes() const;

private:
    struct Pending {
        std::string path;
        std::vector<u8> bytes;
    };
    CompressionPolicy policy_;
    int zstd_level_;
    std::vector<Pending> pending_;
    PakKey key_;
    bool encrypt_ = false;
};

class PakReader {
public:
    PakReader() = default;
    PakReader(PakReader&&) noexcept = default;
    PakReader& operator=(PakReader&&) noexcept = default;

    // Opens an archive, checking the header and the index CRC; the entries'
    // data stays on disk until read. False (and `error`) when it isn't one.
    // An encrypted archive needs its key: `keys` are tried by their id.
    [[nodiscard]] bool Open(const std::string& file, std::string* error = nullptr, std::span<const PakKey> keys = {});
    // The same over bytes already in memory (kept by the reader).
    [[nodiscard]] bool OpenMemory(std::vector<u8> bytes, std::string* error = nullptr, std::span<const PakKey> keys = {});

    bool IsOpen() const { return open_; }
    bool IsEncrypted() const { return encrypted_; }
    const std::string& File() const { return file_; }
    const std::vector<PakEntry>& Entries() const { return entries_; }
    const PakEntry* Find(const std::string& path) const;
    bool Contains(const std::string& path) const { return Find(path) != nullptr; }

    // Reads and decompresses an entry, checking its CRC; false (and
    // `error`) when it's missing or damaged.
    [[nodiscard]] bool Read(const std::string& path, std::vector<u8>& out, std::string* error = nullptr) const;
    [[nodiscard]] bool ReadText(const std::string& path, std::string& out, std::string* error = nullptr) const;
    // Reads every entry; the paths of the damaged ones.
    std::vector<std::string> Verify() const;

private:
    bool Parse(std::string* error, std::span<const PakKey> keys);
    bool ReadStored(const PakEntry& entry, std::vector<u8>& out) const;

    bool open_ = false;
    bool encrypted_ = false;
    PakKey key_;
    std::string file_;
    std::vector<u8> memory_;   // when opened from memory
    bool in_memory_ = false;
    std::vector<PakEntry> entries_;
    std::unordered_map<std::string, usize> index_;
};

// Patches (§25.6): what changed between two builds of an archive. A patch
// archive holds the entries that are new or differ in `updated`, and lists
// the ones `updated` no longer has in kRemovedListPath (a JSON array), which
// the virtual file system hides in the archives mounted below it.
inline constexpr const char* kRemovedListPath = "PatchRemoved.json";

struct PatchReport {
    std::vector<std::string> added;
    std::vector<std::string> changed;
    std::vector<std::string> removed;
    usize unchanged = 0;
};

// Fills `patch` (which keeps its compression and encryption) with what
// turns `base` into `updated`. Entries are compared by size and CRC.
PatchReport MakePatch(const PakReader& base, const PakReader& updated, PakWriter& patch);

} // namespace aether::pak
