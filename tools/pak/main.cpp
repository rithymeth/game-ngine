// aether_pak: the command-line side of .apak archives (Phase 25 step 1,
// docs/design/PHASE_SPECS.md §25.1).
//
//   aether_pak create <out.apak> <directory> [--prefix p] [--compression auto|lz4|zstd|none]
//   aether_pak list <archive.apak>
//   aether_pak extract <archive.apak> <out-directory>
//   aether_pak verify <archive.apak>
#include "aether/pak/pak.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::pak;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  aether_pak create <out.apak> <directory> [--prefix p] [--compression auto|lz4|zstd|none]\n"
                 "  aether_pak list <archive.apak>\n"
                 "  aether_pak extract <archive.apak> <out-directory>\n"
                 "  aether_pak verify <archive.apak>\n");
    return 2;
}

bool Open(PakReader& reader, const char* file) {
    std::string error;
    if (!reader.Open(file, &error)) {
        std::fprintf(stderr, "aether_pak: %s: %s\n", file, error.c_str());
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return Usage();
    const std::string command = argv[1];

    if (command == "create") {
        if (argc < 4) return Usage();
        std::string prefix;
        CompressionPolicy policy = CompressionPolicy::Auto;
        for (int i = 4; i + 1 < argc; i += 2) {
            if (std::strcmp(argv[i], "--prefix") == 0) {
                prefix = argv[i + 1];
            } else if (std::strcmp(argv[i], "--compression") == 0) {
                const std::string c = argv[i + 1];
                policy = c == "lz4" ? CompressionPolicy::LZ4
                         : c == "zstd" ? CompressionPolicy::Zstd
                         : c == "none" ? CompressionPolicy::None
                                       : CompressionPolicy::Auto;
            } else {
                return Usage();
            }
        }
        PakWriter writer(policy);
        const usize added = writer.AddDirectory(argv[3], prefix);
        if (added == 0) {
            std::fprintf(stderr, "aether_pak: no files under %s\n", argv[3]);
            return 1;
        }
        std::string error;
        if (!writer.Write(argv[2], &error)) {
            std::fprintf(stderr, "aether_pak: %s\n", error.c_str());
            return 1;
        }
        const u64 written = std::filesystem::file_size(argv[2]);
        std::printf("%s: %zu files, %llu bytes -> %llu bytes (%.1f%%)\n", argv[2], added,
                    static_cast<unsigned long long>(writer.OriginalBytes()), static_cast<unsigned long long>(written),
                    writer.OriginalBytes() ? 100.0 * static_cast<double>(written) / static_cast<double>(writer.OriginalBytes())
                                           : 100.0);
        return 0;
    }

    PakReader reader;
    if (!Open(reader, argv[2])) return 1;

    if (command == "list") {
        u64 stored = 0, original = 0;
        for (const PakEntry& e : reader.Entries()) {
            std::printf("%10llu %10llu  %-4s  %08x  %s\n", static_cast<unsigned long long>(e.size),
                        static_cast<unsigned long long>(e.stored_size), CompressionName(e.compression), e.crc32,
                        e.path.c_str());
            stored += e.stored_size, original += e.size;
        }
        std::printf("%zu entries, %llu bytes stored, %llu bytes original\n", reader.Entries().size(),
                    static_cast<unsigned long long>(stored), static_cast<unsigned long long>(original));
        return 0;
    }
    if (command == "verify") {
        const std::vector<std::string> damaged = reader.Verify();
        for (const std::string& path : damaged) std::printf("damaged: %s\n", path.c_str());
        std::printf("%zu of %zu entries OK\n", reader.Entries().size() - damaged.size(), reader.Entries().size());
        return damaged.empty() ? 0 : 1;
    }
    if (command == "extract") {
        if (argc < 4) return Usage();
        int failures = 0;
        for (const PakEntry& e : reader.Entries()) {
            std::vector<u8> bytes;
            std::string error;
            if (!reader.Read(e.path, bytes, &error)) {
                std::fprintf(stderr, "aether_pak: %s\n", error.c_str());
                ++failures;
                continue;
            }
            const std::filesystem::path out = std::filesystem::path(argv[3]) / e.path;
            std::error_code ec;
            std::filesystem::create_directories(out.parent_path(), ec);
            std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                       static_cast<std::streamsize>(bytes.size()));
        }
        std::printf("extracted %zu entries to %s\n", reader.Entries().size() - failures, argv[3]);
        return failures ? 1 : 0;
    }
    return Usage();
}
