#pragma once
// A short Lantern Run playthrough for the mobile app shells: the Android and iOS apps run it on start and print
// "AETHER_SMOKE PASS" or "AETHER_SMOKE FAIL <check>", which the emulator and simulator CI jobs look for.
#include <string>

namespace lantern {

// Empty when every check passes, otherwise the name of the first one that failed.
std::string SmokeTest();

// "AETHER_SMOKE PASS" or "AETHER_SMOKE FAIL <check>".
std::string SmokeLine();

} // namespace lantern
