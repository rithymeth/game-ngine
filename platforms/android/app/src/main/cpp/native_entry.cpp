// The Android app shell's native side. On start it plays Lantern Run (samples/frozen_api_game) headless, the same
// checks as the desktop `frozen_api_game` test, and logs one line the CI emulator job looks for:
// "AETHER_SMOKE PASS" or "AETHER_SMOKE FAIL <first failing check>" under the log tag "Aether".
#include "lantern_run.h"

#include <android/log.h>
#include <jni.h>

#include <string>

namespace {

std::string RunLanternRun() {
    using namespace lantern;
    Settings tuned;
    tuned.walk_speed = 3.0f;
    tuned.lantern_count = 6;
    Settings loaded;
    if (!UnpackSettings(PackSettings(tuned), loaded) || loaded.lantern_count != 6) return "settings pack round trip";
    Game game(loaded);
    int frames = 0;
    while (!game.Done() && frames < 6000) game.Step(1.0f / 60.0f), ++frames;
    if (!game.Done() || game.Lit() != 6) return "lamplighter lights every lantern";
    if (frames <= 60 || frames >= 1500) return "plausible play time";
    Game second(loaded);
    if (!second.Load(game.Save()) || second.Lit() != 6) return "saved game loads back";
    if (second.Load("{not json")) return "bad save refused";
    return {};
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_aether_engine_MainActivity_nativeEngineStatus(JNIEnv* env, jclass) {
    const std::string failure = RunLanternRun();
    const std::string line = failure.empty() ? "AETHER_SMOKE PASS" : "AETHER_SMOKE FAIL " + failure;
    __android_log_print(failure.empty() ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "Aether", "%s", line.c_str());
    return env->NewStringUTF(("Aether Engine: Lantern Run self-test\n" + line).c_str());
}
