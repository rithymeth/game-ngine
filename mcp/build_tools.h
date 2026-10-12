#pragma once

// MCP tools that build, test and run the engine itself: configure and build a
// CMake build directory, run the unit tests, and run any program the build
// produced (the headless player, the functional-test runner, the cooker...).
//
// They act on one CMake build directory, found from --build-dir, the
// AETHER_BUILD_DIR environment variable, or by walking up from the server
// executable to the nearest CMakeCache.txt (the server is built inside it).

#include "mcp_server.h"

#include <filesystem>
#include <string>

namespace aether::mcp {

struct BuildContext {
    std::filesystem::path build_dir;  // holds CMakeCache.txt; empty if none was found
    std::filesystem::path source_dir; // CMAKE_HOME_DIRECTORY from the cache
};

// Finds the build directory: `explicit_dir` if not empty, else $AETHER_BUILD_DIR, else the nearest
// CMakeCache.txt at or above `start` (normally the server executable's folder). Empty context if none.
BuildContext FindBuildContext(const std::string& explicit_dir, const std::filesystem::path& start);

void RegisterBuildTools(McpServer& server, BuildContext context);

} // namespace aether::mcp
