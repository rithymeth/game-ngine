#include "test_framework.h"

#include <csignal>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define AETHER_TEST_WRITE(text, len) _write(2, text, static_cast<unsigned>(len))
#else
#include <unistd.h>
#define AETHER_TEST_WRITE(text, len) (void)!write(2, text, len)
#endif

namespace {

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
    return aether::test::RunAll();
}
