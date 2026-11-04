// Fuzz target: file-based loaders that have no from-memory entry point (Phase 47 step 1): the project file and
// the crash report. The first input byte picks one; the rest is written to a temporary file and loaded.
#include "fuzz_common.h"

#include "aether/debug/crash.h"
#include "aether/project/project.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzQuietLogs();
    if (size == 0) return 0;
    namespace fs = std::filesystem;
    static const fs::path dir = fs::temp_directory_path() / ("aether_fuzz_files_" + std::to_string(reinterpret_cast<std::uintptr_t>(&dir)));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const bool project = (data[0] & 1) == 0;
    const fs::path file = dir / (project ? "fuzz.aproject" : "crash-fuzz.txt");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data + 1), static_cast<std::streamsize>(size - 1));
    }
    std::string error;
    if (project) {
        aether::ProjectSettings settings;
        (void)aether::LoadProject(file, settings, &error);
    } else {
        aether::CrashReport report;
        (void)aether::ParseCrashReport(file.string(), report);
    }
    return 0;
}
