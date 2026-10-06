#pragma once

// Aether crash handler (Phase 23). On Windows it installs a structured
// exception handler (SEH) plus std::terminate and SIGABRT handlers. When a
// crash or uncaught exception hits, the installed handlers write a minidump
// (logs/crash_YYYYMMDD_HHMMSS.dmp) and the captured log
// (logs/crash_YYYYMMDD_HHMMSS.log, taken from the engine's Logger ring
// buffer) and then show a small "Aether Crash Reporter" dialog pointing at
// the saved files.
//
// On non-Windows targets crash reporting is unavailable: InstallCrashHandler
// returns false (the engine still logs) and WriteCrashArtifactsForTesting is
// a no-op, so the header still compiles and can be included everywhere.

#if defined(_WIN32)

namespace aether::dev {

// Installs the Windows SEH + terminate/abort handlers that write a minidump
// (logs/crash_<timestamp>.dmp) and a captured log
// (logs/crash_<timestamp>.log), then show a crash reporter dialog. Returns
// false on non-Windows (crash reporting not available - engine still logs).
bool InstallCrashHandler();

// Forces a simulate-crash for testing (writes dump + log). Used by a manual
// test only; does not trigger the dialog.
void WriteCrashArtifactsForTesting();

} // namespace aether::dev

#else // !defined(_WIN32)

namespace aether::dev {

// Crash reporting is Windows-only; the engine still logs on other platforms.
inline bool InstallCrashHandler() { return false; }
inline void WriteCrashArtifactsForTesting() {}

} // namespace aether::dev

#endif // defined(_WIN32)
