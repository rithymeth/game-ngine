#include "aether/debug/crash.h"

#include "aether/core/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <process.h>
#include <fcntl.h>
#else
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define AETHER_HAS_EXECINFO 1
#endif
#endif

namespace aether {

namespace {

// --- Fixed, preallocated state the handler reads without locks ---------------------------------

constexpr usize kLogSlots = 200, kLogSlotSize = 256;
constexpr usize kContextSlots = 16, kContextSize = 128;
constexpr usize kPathSize = 1024;

struct CrashState {
    std::atomic<bool> installed{false};
    std::atomic<bool> handling{false};
    char directory[kPathSize] = {};
    char app[128] = {};
    char build[128] = {};
    bool minidump = true;
    char log[kLogSlots][kLogSlotSize] = {};
    std::atomic<u64> log_next{0};
    char context_key[kContextSlots][kContextSize] = {};
    char context_value[kContextSlots][kContextSize] = {};
    int sink = 0;
    std::terminate_handler previous_terminate = nullptr;
};

CrashState& State() {
    static CrashState state;
    return state;
}

void Copy(char* dst, usize size, const char* src) {
    usize i = 0;
    for (; src && src[i] && i + 1 < size; ++i) dst[i] = src[i];
    dst[i] = 0;
}

// --- Async-signal-safe writing -------------------------------------------------------------------

#ifdef _WIN32
using Fd = int;
Fd OpenReport(const char* path) { return _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644); }
void WriteRaw(Fd fd, const char* s, usize n) { _write(fd, s, static_cast<unsigned>(n)); }
void CloseFd(Fd fd) { _close(fd); }
u64 Pid() { return static_cast<u64>(_getpid()); }
u64 ThreadId() { return static_cast<u64>(GetCurrentThreadId()); }
#else
using Fd = int;
Fd OpenReport(const char* path) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
void WriteRaw(Fd fd, const char* s, usize n) {
    while (n > 0) {
        const ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w, n -= static_cast<usize>(w);
    }
}
void CloseFd(Fd fd) { close(fd); }
u64 Pid() { return static_cast<u64>(getpid()); }
u64 ThreadId() { return static_cast<u64>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(pthread_self()))); }
#endif

void Put(Fd fd, const char* s) { WriteRaw(fd, s, std::strlen(s)); }

// Decimal or hex into `buf` (at least 24 bytes); returns buf.
char* Num(char* buf, u64 v, bool hex = false) {
    char tmp[24];
    int n = 0;
    const u64 base = hex ? 16 : 10;
    do {
        const u64 d = v % base;
        tmp[n++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v != 0 && n < 23);
    int i = 0;
    if (hex) buf[i++] = '0', buf[i++] = 'x';
    while (n > 0) buf[i++] = tmp[--n];
    buf[i] = 0;
    return buf;
}

void Field(Fd fd, const char* key, const char* value) {
    Put(fd, key);
    Put(fd, ": ");
    Put(fd, value);
    Put(fd, "\n");
}

// The report's path, without extension: <dir>/crash-<time>-<pid>.
void ReportStem(char* out, usize size, u64 when) {
    CrashState& s = State();
    char num[24];
    usize i = 0;
    const auto add = [&](const char* p) {
        for (; *p && i + 1 < size; ++p) out[i++] = *p;
    };
    add(s.directory);
    if (i > 0 && out[i - 1] != '/' && out[i - 1] != '\\') add("/");
    add("crash-");
    add(Num(num, when));
    add("-");
    add(Num(num, Pid()));
    out[i] = 0;
}

void WriteReportFile(const char* path, const char* reason, int signal, u64 address, u64 when, void* const* frames, int frame_count) {
    CrashState& s = State();
    const Fd fd = OpenReport(path);
    if (fd < 0) return;
    char num[24];
    Put(fd, "Aether crash report\n");
    Field(fd, "app", s.app);
    Field(fd, "build", s.build);
    Field(fd, "reason", reason);
    if (signal != 0) Field(fd, "signal", Num(num, static_cast<u64>(signal)));
    Field(fd, "address", Num(num, address, true));
    Field(fd, "time", Num(num, when));
    Field(fd, "pid", Num(num, Pid()));
    Field(fd, "thread", Num(num, ThreadId(), true));
    for (usize i = 0; i < kContextSlots; ++i) {
        if (!s.context_key[i][0]) continue;
        Put(fd, "context.");
        Field(fd, s.context_key[i], s.context_value[i]);
    }
    Put(fd, "--- backtrace ---\n");
#if defined(AETHER_HAS_EXECINFO)
    if (frames && frame_count > 0) backtrace_symbols_fd(frames, frame_count, fd);
#else
    for (int i = 0; frames && i < frame_count; ++i) {
        Put(fd, Num(num, reinterpret_cast<u64>(frames[i]), true));
        Put(fd, "\n");
    }
#endif
    Put(fd, "--- log ---\n");
    const u64 next = s.log_next.load(std::memory_order_acquire);
    const u64 first = next > kLogSlots ? next - kLogSlots : 0;
    for (u64 i = first; i < next; ++i) {
        const char* line = s.log[i % kLogSlots];
        WriteRaw(fd, line, strnlen(line, kLogSlotSize));
        Put(fd, "\n");
    }
    Put(fd, "--- end ---\n");
    CloseFd(fd);
}

int CaptureFrames(void** frames, int max) {
#if defined(AETHER_HAS_EXECINFO)
    return backtrace(frames, max);
#elif defined(_WIN32)
    return static_cast<int>(CaptureStackBackTrace(0, static_cast<DWORD>(max), frames, nullptr));
#else
    (void)frames, (void)max;
    return 0;
#endif
}

void Report(const char* reason, int signal, u64 address) {
    char stem[kPathSize], path[kPathSize + 8];
    const u64 when = static_cast<u64>(std::time(nullptr));
    ReportStem(stem, sizeof(stem), when);
    Copy(path, sizeof(path), stem);
    std::strncat(path, ".txt", sizeof(path) - std::strlen(path) - 1);
    void* frames[64];
    const int n = CaptureFrames(frames, 64);
    WriteReportFile(path, reason, signal, address, when, frames, n);
}

// --- Handlers ----------------------------------------------------------------------------------------

void OnTerminate() {
    CrashState& s = State();
    if (!s.handling.exchange(true)) {
        const char* what = "an uncaught C++ exception";
        char buf[kContextSize + 64];
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                Copy(buf, sizeof(buf), "uncaught exception: ");
                std::strncat(buf, ex.what(), sizeof(buf) - std::strlen(buf) - 1);
                what = buf;
            } catch (...) {
            }
        }
        Report(what, 0, 0);
    }
    if (s.previous_terminate) s.previous_terminate();
    std::abort();
}

#ifdef _WIN32
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
    CrashState& s = State();
    if (s.handling.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    const u64 address = info && info->ExceptionRecord ? reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress) : 0;
    const char* reason = code == EXCEPTION_ACCESS_VIOLATION        ? "access violation"
                         : code == EXCEPTION_STACK_OVERFLOW        ? "stack overflow"
                         : code == EXCEPTION_INT_DIVIDE_BY_ZERO    ? "integer divide by zero"
                         : code == EXCEPTION_ILLEGAL_INSTRUCTION   ? "illegal instruction"
                                                                   : "unhandled exception";
    Report(reason, static_cast<int>(code), address);
    if (s.minidump) {
        char stem[kPathSize], path[kPathSize + 8];
        ReportStem(stem, sizeof(stem), static_cast<u64>(std::time(nullptr)));
        Copy(path, sizeof(path), stem);
        std::strncat(path, ".dmp", sizeof(path) - std::strlen(path) - 1);
        HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                              static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                              info ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(file);
        }
    }
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
#else
const int kSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS};
struct sigaction g_previous[5];
std::vector<char> g_alt_stack;

const char* SignalReason(int sig) {
    switch (sig) {
    case SIGSEGV: return "segmentation fault (SIGSEGV)";
    case SIGABRT: return "abort (SIGABRT)";
    case SIGFPE: return "floating-point exception (SIGFPE)";
    case SIGILL: return "illegal instruction (SIGILL)";
    case SIGBUS: return "bus error (SIGBUS)";
    }
    return "fatal signal";
}

void OnSignal(int sig, siginfo_t* info, void*) {
    CrashState& s = State();
    if (!s.handling.exchange(true)) Report(SignalReason(sig), sig, info ? reinterpret_cast<u64>(info->si_addr) : 0);
    // Die as we would have: the default action, re-raised.
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

} // namespace

bool CrashHandler::Install(const CrashConfig& config, std::string* error) {
    CrashState& s = State();
    if (s.installed) Uninstall();
    std::error_code ec;
    std::filesystem::create_directories(config.directory, ec);
    if (ec) {
        if (error) *error = "couldn't create " + config.directory + ": " + ec.message();
        return false;
    }
    const std::string dir = std::filesystem::absolute(config.directory, ec).string();
    Copy(s.directory, sizeof(s.directory), (ec ? config.directory : dir).c_str());
    Copy(s.app, sizeof(s.app), config.app_name.c_str());
    Copy(s.build, sizeof(s.build), config.build.c_str());
    s.minidump = config.minidump;
    s.handling = false;
    s.sink = Logger::Instance().AddSink([](const LogLine& line) {
        CrashState& st = State();
        char* slot = st.log[st.log_next.fetch_add(1, std::memory_order_acq_rel) % kLogSlots];
        std::snprintf(slot, kLogSlotSize, "[%s] %s: %s", LogLevelName(line.level), line.category.c_str(), line.message.c_str());
    });
    s.previous_terminate = std::set_terminate(&OnTerminate);
#ifdef _WIN32
    g_previous_filter = SetUnhandledExceptionFilter(&OnUnhandled);
#else
    // An alternate stack, so a stack overflow can still be reported.
    g_alt_stack.assign(64 * 1024, 0);
    stack_t ss{};
    ss.ss_sp = g_alt_stack.data();
    ss.ss_size = g_alt_stack.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = &OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (usize i = 0; i < 5; ++i) sigaction(kSignals[i], &sa, &g_previous[i]);
#if defined(AETHER_HAS_EXECINFO)
    void* warm[1];
    backtrace(warm, 1); // loads libgcc now, not inside the handler
#endif
#endif
    s.installed = true;
    return true;
}

void CrashHandler::Uninstall() {
    CrashState& s = State();
    if (!s.installed) return;
    Logger::Instance().RemoveSink(s.sink);
    std::set_terminate(s.previous_terminate);
#ifdef _WIN32
    SetUnhandledExceptionFilter(g_previous_filter);
#else
    for (usize i = 0; i < 5; ++i) sigaction(kSignals[i], &g_previous[i], nullptr);
#endif
    s.installed = false;
}

bool CrashHandler::Installed() { return State().installed; }

void CrashHandler::SetContext(const std::string& key, const std::string& value) {
    CrashState& s = State();
    usize free_slot = kContextSlots;
    for (usize i = 0; i < kContextSlots; ++i) {
        if (s.context_key[i][0] && key.compare(0, kContextSize - 1, s.context_key[i]) == 0) {
            Copy(s.context_value[i], kContextSize, value.c_str());
            return;
        }
        if (!s.context_key[i][0] && free_slot == kContextSlots) free_slot = i;
    }
    if (free_slot == kContextSlots) return;
    Copy(s.context_value[free_slot], kContextSize, value.c_str());
    Copy(s.context_key[free_slot], kContextSize, key.c_str());
}

void CrashHandler::ClearContext() {
    CrashState& s = State();
    for (usize i = 0; i < kContextSlots; ++i) s.context_key[i][0] = 0, s.context_value[i][0] = 0;
}

std::string CrashHandler::WriteReport(const std::string& reason) {
    char stem[kPathSize];
    const u64 when = static_cast<u64>(std::time(nullptr));
    ReportStem(stem, sizeof(stem), when);
    // Reports written on purpose get a counter so several in one second don't collide.
    static std::atomic<u32> counter{0};
    const std::string path = std::string(stem) + "-r" + std::to_string(counter++) + ".txt";
    void* frames[64];
    const int n = CaptureFrames(frames, 64);
    WriteReportFile(path.c_str(), reason.c_str(), 0, 0, when, frames, n);
    return path;
}

// ---------------------------------------------------------------- reading reports

std::string CrashReport::Field(const std::string& key) const {
    auto it = fields.find(key);
    return it == fields.end() ? std::string() : it->second;
}

bool ParseCrashReport(const std::string& path, CrashReport& out) {
    std::ifstream in(path);
    if (!in) return false;
    out = CrashReport{};
    out.path = path;
    out.seen = path.size() >= 9 && path.compare(path.size() - 9, 9, ".seen.txt") == 0;
    std::string line;
    if (!std::getline(in, line) || line.rfind("Aether crash report", 0) != 0) return false;
    enum { Fields, Backtrace, Log } section = Fields;
    while (std::getline(in, line)) {
        if (line == "--- backtrace ---") section = Backtrace;
        else if (line == "--- log ---") section = Log;
        else if (line == "--- end ---") break;
        else if (section == Backtrace) out.backtrace.push_back(line);
        else if (section == Log) out.log.push_back(line);
        else if (const usize colon = line.find(": "); colon != std::string::npos)
            out.fields[line.substr(0, colon)] = line.substr(colon + 2);
    }
    return true;
}

std::vector<CrashReport> ListCrashReports(const std::string& directory, bool include_seen) {
    std::vector<std::pair<std::filesystem::file_time_type, CrashReport>> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_regular_file() || name.rfind("crash-", 0) != 0 || entry.path().extension() != ".txt") continue;
        CrashReport report;
        if (!ParseCrashReport(entry.path().string(), report) || (report.seen && !include_seen)) continue;
        found.emplace_back(entry.last_write_time(ec), std::move(report));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second.path > b.second.path;
    });
    std::vector<CrashReport> out;
    for (auto& [time, report] : found) out.push_back(std::move(report));
    return out;
}

bool MarkCrashReportSeen(const CrashReport& report) {
    if (report.seen) return true;
    std::filesystem::path p(report.path);
    std::error_code ec;
    std::filesystem::rename(p, p.parent_path() / (p.stem().string() + ".seen.txt"), ec);
    return !ec;
}

bool DeleteCrashReport(const CrashReport& report) {
    std::filesystem::path p(report.path);
    std::error_code ec;
    std::string stem = p.stem().string();
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".seen") == 0) stem.resize(stem.size() - 5);
    std::filesystem::remove(p.parent_path() / (stem + ".dmp"), ec);
    return std::filesystem::remove(p, ec) && !ec;
}

} // namespace aether
