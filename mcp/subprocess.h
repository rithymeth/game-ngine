#pragma once

// Runs a child process to completion, capturing stdout and stderr together.
// Used by the build and test tools. Kills the whole process tree on timeout.

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace aether::mcp {

struct ProcessSpec {
    // The program and its arguments (no shell). Ignored when `command_line` is set.
    std::vector<std::string> argv;
    // A whole command line run through the shell (cmd.exe /c on Windows, sh -c elsewhere).
    std::string command_line;
    std::string working_directory;
    // Set in the child on top of the inherited environment (or of `base_environment`, if not empty).
    std::map<std::string, std::string> environment;
    // If not empty, the child's environment starts from this instead of ours.
    std::map<std::string, std::string> base_environment;
    unsigned timeout_ms = 600000;
    // Only the last `max_output_bytes` of the output are kept.
    std::size_t max_output_bytes = 4 * 1024 * 1024;
};

struct ProcessResult {
    bool started = false;     // false: the program could not be launched (see `output`)
    bool timed_out = false;
    int exit_code = -1;
    double seconds = 0.0;
    bool output_truncated = false;
    std::string output;
};

ProcessResult RunProcess(const ProcessSpec& spec);

// The environment of this process.
std::map<std::string, std::string> CurrentEnvironment();

} // namespace aether::mcp
