#pragma once

#include "aether/core/log.h"

// Keeps engine logging quiet while fuzzing (a malformed input logs an error by design).
inline void FuzzQuietLogs() {
    static bool done = false;
    if (!done) {
        aether::Logger::Instance().SetMinLevel(aether::LogLevel::Fatal);
        done = true;
    }
}
