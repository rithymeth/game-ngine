#include "smoke.h"

#include "lantern_run.h"

namespace lantern {

std::string SmokeTest() {
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

std::string SmokeLine() {
    const std::string failure = SmokeTest();
    return failure.empty() ? "AETHER_SMOKE PASS" : "AETHER_SMOKE FAIL " + failure;
}

} // namespace lantern
