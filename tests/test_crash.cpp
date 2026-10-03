#include "aether/core/log.h"
#include "aether/debug/crash.h"
#include "test_framework.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Phase 23 step 4: crash reports - writing one on purpose and reading it
// back (fields, context, backtrace, log), listing, dismissing and
// deleting, and (on POSIX) real crashes in child processes: a segfault,
// an abort and an uncaught exception, each reported and still fatal.

using namespace aether;

#define CHECK(...) AETHER_CHECK((__VA_ARGS__))

namespace {
std::filesystem::path Dir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    return dir;
}
bool LogHas(const CrashReport& r, const std::string& text) {
    return std::any_of(r.log.begin(), r.log.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
}
} // namespace

AETHER_TEST(Crash_ReportsWrittenAndRead) {
    const std::filesystem::path dir = Dir("aether_test_crash");
    CrashConfig config;
    config.directory = dir.string();
    config.app_name = "TestGame";
    config.build = "1.2.3";
    CHECK(CrashHandler::Install(config) && CrashHandler::Installed());
    Logger::Instance().SetStdout(false);
    AETHER_LOG_INFO("Test", "loading level %d", 3);
    AETHER_LOG_WARN("Test", "texture missing");
    Logger::Instance().SetStdout(true);
    CrashHandler::SetContext("level", "Docks");
    CrashHandler::SetContext("players", "2");
    CrashHandler::SetContext("level", "Harbour"); // replaces
    const std::string path = CrashHandler::WriteReport("a test report");
    CHECK(std::filesystem::exists(path));

    CrashReport r;
    CHECK(ParseCrashReport(path, r));
    CHECK(r.Field("app") == "TestGame" && r.Field("build") == "1.2.3" && r.Field("reason") == "a test report");
    CHECK(r.Field("context.level") == "Harbour" && r.Field("context.players") == "2");
    CHECK(!r.Field("pid").empty() && r.Field("address") == "0x0" && !r.Field("time").empty() && !r.seen);
    CHECK(LogHas(r, "[INFO] Test: loading level 3") && LogHas(r, "[WARN] Test: texture missing"));
#if !defined(_WIN32)
    CHECK(!r.backtrace.empty());
#endif
    CrashHandler::ClearContext();
    const std::string second = CrashHandler::WriteReport("another");
    CHECK(second != path);
    CrashReport r2;
    CHECK(ParseCrashReport(second, r2) && r2.Field("context.level").empty());

    // Listing, dismissing, deleting.
    std::vector<CrashReport> list = ListCrashReports(dir.string());
    CHECK(list.size() == 2);
    CHECK(MarkCrashReportSeen(list[0]));
    CHECK(ListCrashReports(dir.string()).size() == 1 && ListCrashReports(dir.string(), true).size() == 2);
    for (const CrashReport& report : ListCrashReports(dir.string(), true)) CHECK(DeleteCrashReport(report));
    CHECK(ListCrashReports(dir.string(), true).empty());
    CHECK(!ParseCrashReport((dir / "nope.txt").string(), r));
    CrashHandler::Uninstall();
    CHECK(!CrashHandler::Installed());
    std::filesystem::remove_all(dir);
}

#ifndef _WIN32
namespace {
// Runs `crash` in a child process with the handler installed; returns the signal it died of.
int CrashInChild(const std::filesystem::path& dir, void (*crash)()) {
    const pid_t pid = fork();
    if (pid == 0) {
        CrashConfig config;
        config.directory = dir.string();
        config.app_name = "Child";
        Logger::Instance().SetStdout(false);
        if (!CrashHandler::Install(config)) _exit(3);
        CrashHandler::SetContext("phase", "child");
        AETHER_LOG_INFO("Child", "about to crash");
        crash();
        _exit(0); // not reached
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : -WEXITSTATUS(status);
}
} // namespace

AETHER_TEST(Crash_RealCrashesAreReported) {
    struct Case {
        void (*crash)();
        int signal;
        const char* reason;
    };
    const Case cases[] = {
        {[] { raise(SIGSEGV); }, SIGSEGV, "segmentation fault"},
        {[] { std::abort(); }, SIGABRT, "abort (SIGABRT)"},
        {[] { throw std::runtime_error("the save file is corrupt"); }, SIGABRT, "uncaught exception: the save file is corrupt"},
        {[] { raise(SIGFPE); }, SIGFPE, "floating-point exception"},
    };
    for (const Case& c : cases) {
        const std::filesystem::path dir = Dir("aether_test_crash_child");
        CHECK(CrashInChild(dir, c.crash) == c.signal); // still dies of it
        const std::vector<CrashReport> reports = ListCrashReports(dir.string());
        CHECK(reports.size() == 1); // one report, even when terminate leads to abort
        if (reports.size() != 1) continue;
        const CrashReport& r = reports[0];
        CHECK(r.Field("reason").find(c.reason) == 0 && r.Field("app") == "Child" && r.Field("context.phase") == "child");
        CHECK(LogHas(r, "about to crash") && !r.backtrace.empty());
        std::filesystem::remove_all(dir);
    }
}
#endif
