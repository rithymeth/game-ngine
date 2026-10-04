#include "aether/pak/pak.h"
#include "aether/pak/vfs.h"
#include "test_framework.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Phase 25 step 1: .apak archives (compression, CRCs, damage detection)
// and the virtual file system that mounts directories and archives.

using namespace aether;
using namespace aether::pak;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {

std::string TempDir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / (std::string("aether_pak_") + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

void WriteText(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::vector<u8> Repetitive(usize size) {
    std::vector<u8> v(size);
    for (usize i = 0; i < size; ++i) v[i] = static_cast<u8>("aether engine "[i % 14]);
    return v;
}

std::vector<u8> Noise(usize size) {
    std::vector<u8> v(size);
    u32 x = 0x12345678u;
    for (u8& b : v) {
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        b = static_cast<u8>(x);
    }
    return v;
}

} // namespace

AETHER_TEST(Pak_PathsAndCrc) {
    CHECK(NormalizePath("textures\\rock.png") == "textures/rock.png");
    CHECK(NormalizePath("./a//b/./c.txt") == "a/b/c.txt" && NormalizePath("/root.json") == "root.json");
    CHECK(NormalizePath("a/../b").empty() && NormalizePath("").empty());
    const std::string check = "123456789";
    CHECK(Crc32(std::span<const u8>(reinterpret_cast<const u8*>(check.data()), check.size())) == 0xCBF43926u);
    CHECK(Crc32({}) == 0);
}

AETHER_TEST(Pak_RoundTripWithEveryCompression) {
    const std::vector<u8> big = Repetitive(200000);   // zstd under Auto
    const std::vector<u8> small = Repetitive(3000);   // LZ4 under Auto
    const std::vector<u8> noise = Noise(50000);       // doesn't compress: stored raw
    PakWriter writer;
    CHECK(writer.Add("levels/big.bin", big) && writer.Add("ui/small.bin", small) && writer.Add("audio/noise.bin", noise));
    CHECK(writer.Add("empty.txt", std::string()) && writer.Add("scenes\\start.json", std::string("{\"name\":\"Start\"}")));
    CHECK(!writer.Add("../escape.txt", std::string("x")) && !writer.Add("", std::string("x")));
    CHECK(writer.Add("ui/small.bin", small)); // replaces, doesn't duplicate
    CHECK(writer.EntryCount() == 5 && writer.OriginalBytes() == big.size() + small.size() + noise.size() + 16);

    PakReader reader;
    std::string error;
    CHECK(reader.OpenMemory(writer.Build(), &error));
    CHECK(reader.Entries().size() == 5 && reader.Contains("scenes/start.json") && reader.Contains("./ui\\small.bin"));
    CHECK(reader.Find("levels/big.bin")->compression == Compression::Zstd);
    CHECK(reader.Find("ui/small.bin")->compression == Compression::LZ4);
    CHECK(reader.Find("audio/noise.bin")->compression == Compression::None);
    CHECK(reader.Find("levels/big.bin")->stored_size < big.size() / 20);
    std::vector<u8> out;
    CHECK(reader.Read("levels/big.bin", out) && out == big);
    CHECK(reader.Read("ui/small.bin", out) && out == small);
    CHECK(reader.Read("audio/noise.bin", out) && out == noise);
    CHECK(reader.Read("empty.txt", out) && out.empty());
    std::string text;
    CHECK(reader.ReadText("scenes/start.json", text) && text == "{\"name\":\"Start\"}");
    CHECK(!reader.Read("missing.txt", out, &error) && error.find("not in") != std::string::npos);
    CHECK(reader.Verify().empty());

    // Each forced policy.
    for (CompressionPolicy policy : {CompressionPolicy::None, CompressionPolicy::LZ4, CompressionPolicy::Zstd}) {
        PakWriter w(policy);
        w.Add("big.bin", big);
        PakReader r;
        CHECK(r.OpenMemory(w.Build()) && r.Read("big.bin", out) && out == big);
        const Compression expected = policy == CompressionPolicy::None  ? Compression::None
                                     : policy == CompressionPolicy::LZ4 ? Compression::LZ4
                                                                        : Compression::Zstd;
        CHECK(r.Find("big.bin")->compression == expected);
    }
}

AETHER_TEST(Pak_DetectsDamage) {
    PakWriter writer(CompressionPolicy::None);
    writer.Add("a.txt", std::string("hello archive"));
    writer.Add("b.txt", std::string("second entry"));
    const std::vector<u8> good = writer.Build();
    PakReader reader;
    std::string error;

    std::vector<u8> bytes = good;
    bytes[0] = 'X';
    CHECK(!reader.OpenMemory(bytes, &error) && error.find("not an .apak") != std::string::npos);
    bytes = good;
    bytes.resize(bytes.size() - 5);
    CHECK(!reader.OpenMemory(bytes, &error) && error.find("truncated") != std::string::npos);
    bytes = good;
    bytes.back() ^= 0x40; // the index
    CHECK(!reader.OpenMemory(bytes, &error) && error.find("index is damaged") != std::string::npos);
    CHECK(!reader.OpenMemory(std::vector<u8>(10, 0), &error));

    // A flipped data byte: the archive opens, that entry fails its CRC.
    bytes = good;
    bytes[40] ^= 0x01;
    CHECK(reader.OpenMemory(bytes, &error));
    std::vector<u8> out;
    CHECK(!reader.Read("a.txt", out, &error) && error.find("CRC") != std::string::npos);
    CHECK(reader.Read("b.txt", out));
    CHECK(reader.Verify() == std::vector<std::string>{"a.txt"});

    // Damaged compressed data fails to decompress or fails its CRC.
    PakWriter zw(CompressionPolicy::Zstd);
    zw.Add("z.bin", Repetitive(100000));
    bytes = zw.Build();
    bytes[60] ^= 0xFF;
    CHECK(reader.OpenMemory(bytes) && !reader.Read("z.bin", out, &error));
}

AETHER_TEST(Pak_FilesOnDisk) {
    const std::string dir = TempDir("disk");
    const std::filesystem::path content = std::filesystem::path(dir) / "content";
    WriteText(content / "scenes/start.json", "{\"scene\":1}");
    WriteText(content / "textures/rock.txt", std::string(10000, 'r'));
    WriteText(content / "readme.md", "# Game");
    PakWriter writer;
    CHECK(writer.AddDirectory(content.string(), "game") == 3);
    const std::string file = (std::filesystem::path(dir) / "out/base.apak").string();
    std::string error;
    CHECK(writer.Write(file, &error));
    CHECK(std::filesystem::exists(file) && !std::filesystem::exists(file + ".tmp"));
    PakReader reader;
    CHECK(reader.Open(file, &error) && reader.Entries().size() == 3);
    std::string text;
    CHECK(reader.ReadText("game/textures/rock.txt", text) && text == std::string(10000, 'r'));
    CHECK(reader.Entries()[0].path == "game/readme.md"); // sorted: deterministic archives
    CHECK(!reader.Open(dir + "/missing.apak", &error) && error.find("can't open") != std::string::npos);
    std::filesystem::remove_all(dir);
}

AETHER_TEST(Pak_VirtualFileSystemOverrides) {
    const std::string dir = TempDir("vfs");
    const std::filesystem::path root(dir);
    // Base content, a patch that replaces one file and adds one, and a loose
    // directory of work in progress on top.
    PakWriter base;
    base.Add("scenes/start.json", std::string("base start"));
    base.Add("scenes/level1.json", std::string("base level1"));
    base.Add("textures/rock.png", std::string("base rock"));
    CHECK(base.Write((root / "base.apak").string()));
    PakWriter patch;
    patch.Add("scenes/level1.json", std::string("patched level1"));
    patch.Add("scenes/level2.json", std::string("new level2"));
    CHECK(patch.Write((root / "patch_001.apak").string()));
    WriteText(root / "loose/scenes/start.json", "loose start");

    VirtualFileSystem vfs;
    std::string error;
    CHECK(vfs.Mount((root / "base.apak").string(), "", 0, &error));
    CHECK(vfs.Mount((root / "patch_001.apak").string(), "", 10, &error));
    CHECK(vfs.Mount((root / "loose").string(), "", 100, &error));
    CHECK(!vfs.Mount((root / "nothing").string(), "", 0, &error));
    CHECK(vfs.MountCount() == 3);

    std::string text;
    CHECK(vfs.ReadText("scenes/start.json", text) && text == "loose start");
    CHECK(vfs.ReadText("scenes/level1.json", text) && text == "patched level1");
    CHECK(vfs.ReadText("scenes/level2.json", text) && text == "new level2");
    CHECK(vfs.ReadText("textures/rock.png", text) && text == "base rock");
    CHECK(vfs.Resolve("scenes/level1.json") == (root / "patch_001.apak").string());
    CHECK(!vfs.Exists("scenes/level3.json") && !vfs.ReadText("scenes/level3.json", text, &error));
    CHECK(vfs.List("scenes") == (std::vector<std::string>{"scenes/level1.json", "scenes/level2.json", "scenes/start.json"}));
    CHECK(vfs.List().size() == 4);

    // Unmounting the patch falls back to the base.
    CHECK(vfs.Unmount((root / "patch_001.apak").string()) && !vfs.Unmount("nope"));
    CHECK(vfs.ReadText("scenes/level1.json", text) && text == "base level1" && !vfs.Exists("scenes/level2.json"));

    // Mount points, and an in-memory archive.
    VirtualFileSystem dlc;
    PakWriter pack;
    pack.Add("maps/arena.json", std::string("arena"));
    PakReader reader;
    CHECK(reader.OpenMemory(pack.Build()));
    dlc.MountPak(std::move(reader), "dlc/arena_pack");
    CHECK(dlc.ReadText("dlc/arena_pack/maps/arena.json", text) && text == "arena");
    CHECK(!dlc.Exists("maps/arena.json") && dlc.List("dlc") == std::vector<std::string>{"dlc/arena_pack/maps/arena.json"});
    std::filesystem::remove_all(dir);
}
