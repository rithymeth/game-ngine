// Aether Windows crash handler. Guarded entirely with `#if defined(_WIN32)`
// so the translation unit stays empty on other platforms; the non-Windows
// stubs live in the header (crash.h) and return gracefully.
#if defined(_WIN32)

#include "aether/dev/crash.h"
#include "aether/core/log.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace aether::dev {

namespace {

// Re-entrancy guard: once a crash handler starts running we must not recurse
// into the (non-reentrant) artifact writers, e.g. if the logger mutex is held
// by the crashed thread or a secondary abort fires mid-reporting.
std::atomic<bool> g_in_handler{false};

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

std::string TimestampBase() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char filename[64];
    std::snprintf(filename, sizeof(filename), "crash_%04u%02u%02u_%02u%02u%02u",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
    return std::string("logs/") + filename;
}

bool WriteDump(const std::string& path, _EXCEPTION_POINTERS* ep) {
    HANDLE file =
        CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = TRUE;

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                      MiniDumpNormal, ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

void WriteCapturedLog(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        return;
    }
    for (const LogEntry& entry : Logger::Instance().RecentLogLines()) {
        std::fprintf(f, "[%s] %s: %s\n", LevelName(entry.level), entry.category.c_str(),
                     entry.message.c_str());
    }
    std::fclose(f);
}

// Writes crash artifacts (guarded against recursion) and returns the dump
// path, or an empty string if it could not (or is already in a handler) write.
std::string WriteCrashArtifacts(_EXCEPTION_POINTERS* ep) {
    // If a handler fires while we are already handling a crash, do not recurse.
    if (g_in_handler.exchange(true)) {
        return {};
    }
    CreateDirectoryA("logs", nullptr);
    const std::string base = TimestampBase();
    const std::string dump_path = base + ".dmp";
    if (!WriteDump(dump_path, ep)) {
        return {};
    }
    WriteCapturedLog(base + ".log");
    return dump_path;
}

void ReportCrashDialog(const std::string& dump_path) {
    std::string log_path = dump_path;
    log_path.replace(log_path.size() - 4, 4, ".log");
    const std::string text =
        "Aether has crashed.\n\n"
        "A minidump and the captured log were written to:\n  " +
        dump_path + "\n  " + log_path +
        "\n\nPlease include these files when reporting the crash.";
    MessageBoxA(nullptr, text.c_str(), "Aether Crash Reporter",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

LONG WINAPI SehHandler(_EXCEPTION_POINTERS* ep) {
    const std::string dump_path = WriteCrashArtifacts(ep);
    if (!dump_path.empty()) {
        ReportCrashDialog(dump_path);
    }
    // Reporting we handled it lets the OS terminate cleanly (dump already written).
    return EXCEPTION_EXECUTE_HANDLER;
}

void TerminateHandler() {
    const std::string dump_path = WriteCrashArtifacts(nullptr);
    if (!dump_path.empty()) {
        ReportCrashDialog(dump_path);
    }
    // Never return from a terminate handler: the runtime is in an undefined
    // state, so re-raise abort (the guarded SIGABRT handler _Exit()s) as the
    // final act.
    std::abort();
}

void AbortSignalHandler(int /*sig*/) {
    // Final safety net: no dialog (the terminate handler already reported).
    // The guarded writer cannot recurse; hard-exit afterwards.
    (void)WriteCrashArtifacts(nullptr);
    std::_Exit(3);
}

} // namespace (private helpers)

bool InstallCrashHandler() {
    SetUnhandledExceptionFilter(SehHandler);
    std::set_terminate(TerminateHandler);
    std::signal(SIGABRT, AbortSignalHandler);
    return true;
}

void WriteCrashArtifactsForTesting() {
    // Hooks the same artifact writer; just drops the path. No dialog.
    (void)WriteCrashArtifacts(nullptr);
}

} // namespace aether::dev

#endif // defined(_WIN32)
