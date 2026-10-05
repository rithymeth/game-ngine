// Runs a fuzz target's LLVMFuzzerTestOneInput over every file in the given paths (files or
// directories, searched recursively): the corpus as a regression test on any compiler.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    std::size_t ran = 0;
    for (int i = 1; i < argc; ++i) {
        std::vector<fs::path> files;
        if (fs::is_directory(argv[i])) {
            for (const auto& e : fs::recursive_directory_iterator(argv[i])) {
                if (e.is_regular_file()) files.push_back(e.path());
            }
        } else {
            files.push_back(argv[i]);
        }
        for (const fs::path& p : files) {
            std::ifstream f(p, std::ios::binary);
            const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
            ++ran;
        }
    }
    std::printf("replayed %zu input(s)\n", ran);
    return 0;
}
