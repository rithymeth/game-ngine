// The Android app shell's native side. On start it plays Lantern Run headless (samples/frozen_api_game/smoke.h) and
// logs "AETHER_SMOKE PASS" or "AETHER_SMOKE FAIL <check>" under the log tag "Aether"; the CI emulator job looks for it.
#include "smoke.h"

#include <android/log.h>
#include <jni.h>

#include <string>

extern "C" JNIEXPORT jstring JNICALL
Java_com_aether_engine_MainActivity_nativeEngineStatus(JNIEnv* env, jclass) {
    const std::string line = lantern::SmokeLine();
    const bool pass = line == "AETHER_SMOKE PASS";
    __android_log_print(pass ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "Aether", "%s", line.c_str());
    return env->NewStringUTF(("Aether Engine: Lantern Run self-test\n" + line).c_str());
}
