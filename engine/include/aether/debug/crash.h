#pragma once

#include "aether/core/base.h"

#include <map>
#include <string>
#include <vector>

namespace aether {

// Crash handling (Phase 23 step 4, docs/design/PHASE_SPECS.md §23.4).
//
// Install once at startup. On a crash - a fatal signal (SIGSEGV, SIGABRT,
// SIGFPE, SIGILL, SIGBUS) on POSIX, an unhandled SEH exception on Windows,
// or an uncaught C++ exception anywhere - the handler writes a report file
// to `directory` and lets the process die as it would have:
//   crash-<unix time>-<pid>.txt: the app and build, the reason and fault
//   address, the time, the thread, the context the game set (SetContext),
//   a backtrace, and the last log lines;
// and on Windows a minidump (crash-....dmp) next to it.
// The handler only does async-signal-safe work: the log lines and context
// are copied into fixed buffers as they happen, and the report is written
// with plain write() calls, on an alternate stack (so a stack overflow can
// still be reported).
//
// The next run finds the reports (ListCrashReports) and the editor's crash
// reporter shows them.
struct CrashConfig {
    std::string directory = "crashes";
    std::string app_name = "Aether";
    std::string build = "dev";
    bool minidump = true; // Windows only
};

class CrashHandler {
public:
    static bool Install(const CrashConfig& config, std::string* error = nullptr);
    static void Uninstall();
    static bool Installed();

    // Up to 16 key/value pairs (each cut to 127 characters) included in reports.
    static void SetContext(const std::string& key, const std::string& value);
    static void ClearContext();

    // Writes a report as a crash would, without crashing (for tests and
    // "report a problem"). Returns its path.
    static std::string WriteReport(const std::string& reason);
};

struct CrashReport {
    std::string path;
    std::map<std::string, std::string> fields; // app, build, reason, signal, address, time, pid, thread, context.*
    std::vector<std::string> backtrace;
    std::vector<std::string> log;
    bool seen = false; // dismissed in an earlier run
    std::string Field(const std::string& key) const;
};

bool ParseCrashReport(const std::string& path, CrashReport& out);
// The reports in `directory`, newest first; with include_seen, dismissed ones too.
std::vector<CrashReport> ListCrashReports(const std::string& directory, bool include_seen = false);
// Marks a report seen (it's renamed .seen.txt) so it isn't offered again.
bool MarkCrashReportSeen(const CrashReport& report);
bool DeleteCrashReport(const CrashReport& report); // the report and its minidump

} // namespace aether
