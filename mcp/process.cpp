#include "process.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace aether::mcp {

namespace {

using Clock = std::chrono::steady_clock;

void Append(ProcessResult& result, const char* data, std::size_t size, std::size_t cap) {
    result.output.append(data, size);
    if (result.output.size() > cap) {
        result.output.erase(0, result.output.size() - cap);
        result.output_truncated = true;
    }
}

} // namespace

#ifdef _WIN32

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The CommandLineToArgvW rules, inverted.
std::string QuoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}

} // namespace

std::map<std::string, std::string> CurrentEnvironment() {
    std::map<std::string, std::string> env;
    LPWCH block = GetEnvironmentStringsW();
    for (LPWCH p = block; block != nullptr && *p != L'\0'; p += wcslen(p) + 1) {
        const std::wstring entry = p;
        const std::size_t eq = entry.find(L'=', 1); // "=C:=..." style entries start with '='
        if (eq == std::wstring::npos) continue;
        const auto narrow = [](const std::wstring& w) {
            if (w.empty()) return std::string();
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        };
        env[narrow(entry.substr(0, eq))] = narrow(entry.substr(eq + 1));
    }
    if (block != nullptr) FreeEnvironmentStringsW(block);
    return env;
}

ProcessResult RunProcess(const ProcessSpec& spec) {
    ProcessResult result;
    const auto start = Clock::now();

    std::string command;
    if (!spec.command_line.empty()) {
        command = "cmd.exe /d /s /c \"" + spec.command_line + "\"";
    } else if (!spec.argv.empty()) {
        for (std::size_t i = 0; i < spec.argv.size(); ++i) command += (i ? " " : "") + QuoteArg(spec.argv[i]);
    } else {
        result.output = "empty command";
        return result;
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        result.output = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_in;
    si.hStdOutput = write_end;
    si.hStdError = write_end;

    // Environment block: sorted, double-null terminated.
    std::map<std::string, std::string> env = spec.base_environment.empty() ? CurrentEnvironment() : spec.base_environment;
    for (const auto& [k, v] : spec.environment) env[k] = v;
    std::wstring env_block;
    for (const auto& [k, v] : env) env_block += Widen(k) + L"=" + Widen(v) + L'\0';
    env_block += L'\0';

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    std::wstring wide_command = Widen(command);
    const std::wstring cwd = Widen(spec.working_directory);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, wide_command.data(), nullptr, nullptr, TRUE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, env_block.data(),
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        result.output = "could not start: " + command + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(read_end);
        if (job != nullptr) CloseHandle(job);
        return result;
    }
    if (job != nullptr) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    result.started = true;

    char buffer[16384];
    for (;;) {
        DWORD available = 0;
        const BOOL peeked = PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr);
        if (peeked && available > 0) {
            DWORD got = 0;
            if (ReadFile(read_end, buffer, static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), available)), &got, nullptr) && got > 0) {
                Append(result, buffer, got, spec.max_output_bytes);
            }
            continue;
        }
        if (!peeked) break; // the pipe closed: the child (and its children) are done writing
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            // Drain whatever is left, then stop.
            DWORD left = 0;
            if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &left, nullptr) && left > 0) continue;
            break;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() > spec.timeout_ms) {
            result.timed_out = true;
            if (job != nullptr) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = result.timed_out ? -1 : static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(read_end);
    if (job != nullptr) CloseHandle(job);
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

#else // POSIX

std::map<std::string, std::string> CurrentEnvironment() {
    std::map<std::string, std::string> env;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string entry = *e;
        const std::size_t eq = entry.find('=');
        if (eq != std::string::npos) env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    return env;
}

ProcessResult RunProcess(const ProcessSpec& spec) {
    ProcessResult result;
    const auto start = Clock::now();
    std::vector<std::string> argv = spec.argv;
    if (!spec.command_line.empty()) argv = {"/bin/sh", "-c", spec.command_line};
    if (argv.empty()) {
        result.output = "empty command";
        return result;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        result.output = "pipe failed";
        return result;
    }
    std::map<std::string, std::string> env = spec.base_environment.empty() ? CurrentEnvironment() : spec.base_environment;
    for (const auto& [k, v] : spec.environment) env[k] = v;
    std::vector<std::string> env_strings;
    for (const auto& [k, v] : env) env_strings.push_back(k + "=" + v);

    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        result.output = "fork failed";
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        const int null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) dup2(null_fd, 0);
        close(fds[0]);
        close(fds[1]);
        if (!spec.working_directory.empty() && chdir(spec.working_directory.c_str()) != 0) _exit(127);
        std::vector<char*> args, envp;
        for (std::string& a : argv) args.push_back(a.data());
        args.push_back(nullptr);
        for (std::string& e : env_strings) envp.push_back(e.data());
        envp.push_back(nullptr);
        execvpe(args[0], args.data(), envp.data());
        _exit(127);
    }
    close(fds[1]);
    result.started = true;
    char buffer[16384];
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        const int r = poll(&p, 1, 50);
        if (r > 0) {
            const ssize_t got = read(fds[0], buffer, sizeof(buffer));
            if (got <= 0) break;
            Append(result, buffer, static_cast<std::size_t>(got), spec.max_output_bytes);
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() > spec.timeout_ms) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            break;
        }
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = result.timed_out ? -1 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return result;
}

#endif

} // namespace aether::mcp
