// Plays Lantern Run headless and checks the result; it is the CTest `frozen_api_game`.
#include "lantern_run.h"

#include <cstdio>

int main() {
    using namespace lantern;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        failures += ok ? 0 : 1;
    };

    Settings tuned;
    tuned.walk_speed = 3.0f;
    tuned.lantern_count = 6;
    Settings loaded;
    check(UnpackSettings(PackSettings(tuned), loaded) && loaded.lantern_count == 6 && loaded.walk_speed == 3.0f,
          "settings survive a trip through a packed archive");

    Game game(loaded);
    int frames = 0;
    while (!game.Done() && frames < 6000) game.Step(1.0f / 60.0f), ++frames;
    check(game.Done() && game.Lit() == 6, "the lamplighter lights every lantern");
    check(frames > 60 && frames < 1500, "in a plausible time");

    const std::string saved = game.Save();
    Game second(loaded);
    check(second.Load(saved) && second.Lit() == 6 && second.Done(), "a saved game loads back");
    check(!second.Load("{not json") && !second.Load("{\"x\":1}"), "bad saves are refused");

    Settings bad;
    check(!ParseSettings("[1, 2]", bad), "bad settings are refused");
    std::printf("%s\n", failures == 0 ? "frozen api game passed" : "frozen api game FAILED");
    return failures == 0 ? 0 : 1;
}
