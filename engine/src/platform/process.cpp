#include "aether/platform/process.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace aether::platform {

namespace stdfs = std::filesystem;

namespace {

bool Fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

#if defined(_WIN32)
// One argument, quoted the way CommandLineToArgvW reads it back.
std::wstring Quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    usize slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
        } else {
            out.append(slashes, L'\\');
        }
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<usize>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

} // namespace

stdfs::path ExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) {
            buffer.resize(n);
            return stdfs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code ec;
    const stdfs::path p = stdfs::canonical(buffer.c_str(), ec);
    return ec ? stdfs::path(buffer.c_str()) : p;
#else
    std::error_code ec;
    const stdfs::path p = stdfs::read_symlink("/proc/self/exe", ec);
    return ec ? stdfs::path() : p;
#endif
}

ChildProcess::~ChildProcess() { Reset(); }

void ChildProcess::Reset() {
#if defined(_WIN32)
    if (process_) CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
#else
    pid_ = 0;
#endif
    started_ = false;
    exited_ = false;
    exit_code_ = -1;
}

bool ChildProcess::Start(const stdfs::path& program, const std::vector<std::string>& args, const stdfs::path& working_dir,
                         std::string* error) {
    if (started_ && !exited_ && Running()) return Fail(error, "The process is still running");
    Reset();
    std::error_code ec;
    if (!stdfs::is_regular_file(program, ec)) return Fail(error, "No program at " + program.string());
#if defined(_WIN32)
    std::wstring command = Quote(program.wstring());
    for (const std::string& a : args) command += L" " + Quote(Widen(a));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    const std::wstring dir = working_dir.wstring();
    if (!CreateProcessW(program.wstring().c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &startup, &info)) {
        return Fail(error, "Can't start " + program.string() + " (error " + std::to_string(GetLastError()) + ")");
    }
    CloseHandle(info.hThread);
    process_ = info.hProcess;
#else
    const std::string path = program.string();
    std::vector<std::string> storage;
    storage.push_back(path);
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
#if (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))) || defined(__APPLE__)
    if (!working_dir.empty()) posix_spawn_file_actions_addchdir_np(&actions, working_dir.string().c_str());
#else
    if (!working_dir.empty()) {
        posix_spawn_file_actions_destroy(&actions);
        return Fail(error, "Starting a process in another folder isn't supported on this platform");
    }
#endif
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return Fail(error, "Can't start " + path + ": " + std::strerror(rc));
    pid_ = pid;
#endif
    started_ = true;
    return true;
}

bool ChildProcess::Running() {
    if (!started_ || exited_) return false;
#if defined(_WIN32)
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) return true;
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exit_code_ = static_cast<int>(code);
#else
    int status = 0;
    const pid_t r = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == 0) return true;
    exit_code_ = r < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
    exited_ = true;
    return false;
}

bool ChildProcess::Wait(int* exit_code) {
    if (!started_) return false;
    if (!exited_) {
#if defined(_WIN32)
        WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
        exit_code_ = static_cast<int>(code);
#else
        int status = 0;
        pid_t r;
        do {
            r = waitpid(static_cast<pid_t>(pid_), &status, 0);
        } while (r < 0 && errno == EINTR);
        exit_code_ = r < 0 ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
        exited_ = true;
    }
    if (exit_code) *exit_code = exit_code_;
    return true;
}

void ChildProcess::Kill() {
    if (!started_ || exited_) return;
#if defined(_WIN32)
    TerminateProcess(static_cast<HANDLE>(process_), 1);
#else
    kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
    Wait();
}

} // namespace aether::platform
