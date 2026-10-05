#include "test_framework.h"

#include <csignal>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <io.h>
#define AETHER_TEST_WRITE(text, len) _write(2, text, static_cast<unsigned>(len))
#else
#include <unistd.h>
#define AETHER_TEST_WRITE(text, len) (void)!write(2, text, len)
#endif

namespace {

// Names the test that crashed: a fatal signal otherwise ends the run with
// output still buffered and no clue where it happened (CI logs).
extern "C" void OnFatalSignal(int signal) {
    const char* name = aether::test::CurrentTest();
    const char* prefix = "\n*** fatal signal in test: ";
    AETHER_TEST_WRITE(prefix, std::strlen(prefix));
    if (name) AETHER_TEST_WRITE(name, std::strlen(name));
    AETHER_TEST_WRITE("\n", 1);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
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
    return aether::test::RunAll();
}
