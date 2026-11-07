#include <cstdlib>
#include <cerrno>
#include "test_framework.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#define AETHER_TEST_WRITE(text, len) _write(2, text, static_cast<unsigned>(len))
#else
#include <unistd.h>
#define AETHER_TEST_WRITE(text, len) (void)!write(2, text, len)
#endif

namespace {

unsigned long ProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Integer to text by hand: a crash handler cannot rely on printf.
void WriteNumber(int value) {
    char digits[12];
    int count = 0;
    if (value <= 0) {
        digits[count++] = '0';
    } else {
        char reversed[12];
        int written = 0;
        while (value > 0 && written < 12) {
            reversed[written++] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        while (written > 0) digits[count++] = reversed[--written];
    }
    AETHER_TEST_WRITE(digits, static_cast<unsigned>(count));
}

// Writes where the run died. Only async-signal-safe work belongs here: raw writes, no allocation, no printf.
void WriteCrashLocation(const char* kind) {
    AETHER_TEST_WRITE("\n*** ", 5);
    AETHER_TEST_WRITE(kind, std::strlen(kind));
    const char* testLabel = " in test: ";
    AETHER_TEST_WRITE(testLabel, std::strlen(testLabel));
    if (const char* name = aether::test::CurrentTest()) {
        AETHER_TEST_WRITE(name, std::strlen(name));
    } else {
        AETHER_TEST_WRITE("(outside any test)", 18);
    }
    // The check matters more than the test: a test can be a hundred checks long, and naming only the test
    // still leaves the failing line to be found by hand.
    const auto& check = aether::test::CurrentCheck();
    if (check.expression) {
        const char* checkLabel = "\n***   last check: ";
        AETHER_TEST_WRITE(checkLabel, std::strlen(checkLabel));
        AETHER_TEST_WRITE(check.expression, std::strlen(check.expression));
        if (check.file) {
            const char* open = " (";
            AETHER_TEST_WRITE(open, std::strlen(open));
            AETHER_TEST_WRITE(check.file, std::strlen(check.file));
            AETHER_TEST_WRITE(":", 1);
            WriteNumber(check.line);
            AETHER_TEST_WRITE(")", 1);
        }
    }
    AETHER_TEST_WRITE("\n", 1);
}

// Names the test and the check that crashed. A fatal signal otherwise ends the run with output still
// buffered and no clue where it happened, which is how a release was blocked by "Illegal instruction".
extern "C" void OnFatalSignal(int signal) {
    WriteCrashLocation("fatal signal");
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

#if defined(_WIN32)
// Structured exceptions never reach std::signal on Windows: an assertion raises a breakpoint and a null
// dereference raises an access violation, so the handler above prints nothing there. This is that path,
// and it is the one CI needs -- the release-blocking trap was a Windows-style assertion abort.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    const char* kind = "fatal exception";
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        switch (info->ExceptionRecord->ExceptionCode) {
            case EXCEPTION_BREAKPOINT: kind = "assertion trap"; break;
            case EXCEPTION_ACCESS_VIOLATION: kind = "access violation"; break;
            case EXCEPTION_ILLEGAL_INSTRUCTION: kind = "illegal instruction"; break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO: kind = "integer divide by zero"; break;
            case EXCEPTION_STACK_OVERFLOW: kind = "stack overflow"; break;
            default: break;
        }
    }
    WriteCrashLocation(kind);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Every test that writes scratch state asks the system for a temp directory and uses a fixed name inside
// it, so a file left behind by a failed run can fail the next one before its logic executes. Give each run
// a private directory and point the temp environment at it: one place instead of 150 test-side helpers.
std::filesystem::path PrepareRunDirectory() {
    std::error_code error;
    // AETHER_TEST_TEMP_DIR keeps scratch state off the system disk. CMake's aether_test_scratch_dir sets it for the ctest
    // entries; this honours it when the test binary is run directly too, which is how a developer usually runs it and is
    // why a machine can end up with scratch on one drive and the system temp on another.
    std::filesystem::path root;
    if (const char* configured = std::getenv("AETHER_TEST_TEMP_DIR"); configured != nullptr && *configured != '\0') {
        root = std::filesystem::path(configured);
        std::filesystem::create_directories(root, error);
        if (error) {
            std::fprintf(stderr, "AETHER_TEST_TEMP_DIR=%s could not be used (%s); falling back to the system temp\n",
                         configured, error.message().c_str());
            root.clear();
            error.clear();
        }
    }
    if (root.empty()) {
        root = std::filesystem::temp_directory_path(error);
        if (error) root = std::filesystem::current_path(error);
        if (error) return {};
    }
    const auto dir = root / ("aether-tests-" + std::to_string(ProcessId()));
    // A directory left by a killed run of this same process id is stale by definition.
    std::filesystem::remove_all(dir, error);
    error.clear();
    if (!std::filesystem::create_directories(dir, error) && !std::filesystem::exists(dir, error)) {
        // Returning empty silently left every writing test pointed at the system temp directory, which is the one thing
        // this function exists to prevent -- a file from an earlier run then fails a later one before its logic runs.
        std::fprintf(stderr, "scratch directory %s could not be created (%s, errno=%d); writing tests will use %s\n",
                     dir.string().c_str(), error.message().c_str(), errno,
                     std::filesystem::temp_directory_path(error).string().c_str());
        return {};
    }
    return dir;
}

void PointTempEnvironmentAt(const std::filesystem::path& dir) {
    if (dir.empty()) return;
#if defined(_WIN32)
    _wputenv_s(L"TEMP", dir.c_str());
    _wputenv_s(L"TMP", dir.c_str());
#else
    setenv("TMPDIR", dir.c_str(), 1);
    setenv("TMP", dir.c_str(), 1);
    setenv("TEMP", dir.c_str(), 1);
#endif
}

void RemoveRunDirectory(const std::filesystem::path& dir) {
    if (dir.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    if (error) {
        std::fprintf(stderr, "\nscratch state left in %s (%s)\n", dir.string().c_str(), error.message().c_str());
    }
}

} // namespace

int main() {
    // Line by line, so a crash loses nothing already printed.
#if defined(_WIN32)
    std::setvbuf(stdout, nullptr, _IONBF, 0); // MSVC has no line buffering
#else
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif
    for (int s : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(s, OnFatalSignal);
#if defined(SIGBUS)
    std::signal(SIGBUS, OnFatalSignal);
#endif
#if defined(_WIN32)
    SetUnhandledExceptionFilter(OnUnhandledException);
#endif
    // Before any test runs, so every test's scratch state is private to this run.
    const std::filesystem::path runDirectory = PrepareRunDirectory();
    PointTempEnvironmentAt(runDirectory);
    const int failures = aether::test::RunAll();
    RemoveRunDirectory(runDirectory);
    return failures;
}
