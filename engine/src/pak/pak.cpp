#include "aether/pak/pak.h"

#include "aether/core/log.h"

#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

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
    out.insert(out.end(), index.begin(), index.end());

    std::vector<u8> header;
    header.insert(header.end(), kMagic, kMagic + 4);
    Put32(header, kVersion);
    Put32(header, static_cast<u32>(entries.size()));
    Put32(header, 0);
    Put64(header, index_offset);
    Put64(header, index.size());
    Put32(header, Crc32(index));
    Put32(header, 0);
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

bool PakReader::Open(const std::string& file, std::string* error) {
    *this = PakReader();
    file_ = file;
    return Parse(error);
}

bool PakReader::OpenMemory(std::vector<u8> bytes, std::string* error) {
    *this = PakReader();
    memory_ = std::move(bytes);
    in_memory_ = true;
    file_ = "<memory>";
    return Parse(error);
}

bool PakReader::Parse(std::string* error) {
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
        return true;
    }
    std::ifstream in(file_, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(entry.offset));
    return entry.stored_size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()),
                                                               static_cast<std::streamsize>(out.size())));
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

} // namespace aether::pak
