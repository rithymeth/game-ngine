#include "aether/platform/filesystem.h"
#include "test_framework.h"

#include <filesystem>
#include <string>

using namespace aether;

AETHER_TEST(Filesystem_WriteThenReadBytesRoundTrips) {
    std::string path = (std::filesystem::temp_directory_path() / "aether_test_bytes.bin").string();
    const u8 payload[] = {1, 2, 3, 4, 5};

    AETHER_CHECK(fs::WriteFileBytes(path, payload, sizeof(payload)));
    AETHER_CHECK(fs::Exists(path));
    AETHER_CHECK(fs::FileSize(path) == sizeof(payload));

    std::vector<u8> read_back;
    AETHER_CHECK(fs::ReadFileBytes(path, read_back));
    AETHER_CHECK(read_back.size() == sizeof(payload));
    for (usize i = 0; i < sizeof(payload); ++i) {
        AETHER_CHECK(read_back[i] == payload[i]);
    }

    std::filesystem::remove(path);
}

AETHER_TEST(Filesystem_WriteThenReadTextRoundTrips) {
    std::string path = (std::filesystem::temp_directory_path() / "aether_test_text.txt").string();
    std::string content = "hello aether";

    AETHER_CHECK(fs::WriteFileBytes(path, content.data(), content.size()));

    std::string read_back;
    AETHER_CHECK(fs::ReadFileText(path, read_back));
    AETHER_CHECK(read_back == content);

    std::filesystem::remove(path);
}

AETHER_TEST(Filesystem_MissingFileReadsFail) {
    std::vector<u8> bytes;
    AETHER_CHECK(!fs::ReadFileBytes("this_file_should_not_exist_12345.bin", bytes));
    AETHER_CHECK(!fs::Exists("this_file_should_not_exist_12345.bin"));
}

AETHER_TEST(Filesystem_PathUtilities) {
    AETHER_CHECK(fs::Extension("model.gltf") == ".gltf");
    AETHER_CHECK(!fs::ParentPath((std::filesystem::path("a") / "b" / "c.txt").string()).empty());
}
