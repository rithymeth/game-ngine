#pragma once

#include "aether/core/base.h"

#include <filesystem>
#include <string>
#include <vector>

// Child processes and the running executable's path (Phase 25 step 5): the
// editor's Build and Package window launches the packaged game with these.

namespace aether::platform {

// The running executable's full path; empty if the platform won't say.
std::filesystem::path ExecutablePath();

// A program started with arguments. Destroying a running one leaves it
// running (the editor doesn't kill a game it launched when it closes).
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts `program` with `args` (not including the program itself) in
    // `working_dir` (empty: this process's). False, with `error`, if it
    // couldn't start; one already started must be waited for first.
    bool Start(const std::filesystem::path& program, const std::vector<std::string>& args,
               const std::filesystem::path& working_dir = {}, std::string* error = nullptr);
    bool Started() const { return started_; }
    // Still running (false once it has exited and been waited for).
    bool Running();
    // Waits for it to exit; its exit code goes to `exit_code`. False if it
    // was never started.
    bool Wait(int* exit_code = nullptr);
    // Its exit code, once it has exited; -1 before.
    int ExitCode() const { return exit_code_; }
    void Kill();

private:
    void Reset();
    bool started_ = false;
    bool exited_ = false;
    int exit_code_ = -1;
#if defined(_WIN32)
    void* process_ = nullptr; // HANDLE
#else
    i64 pid_ = 0;
#endif
};

} // namespace aether::platform
