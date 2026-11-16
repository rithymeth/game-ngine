// Simple console demo of the Abyss Protocol systems.
// Full Win32 / renderer integration comes later.

#include "abyss_protocol.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    abyss::Game game;
    std::cout << "Abyss Protocol — acoustic demo\n";
    std::cout << "Simulating 10 seconds of movement + sonar...\n\n";

    for (int i = 0; i < 600; ++i) {  // 10 seconds at 60 Hz
        if (i == 60) game.SetSonar(true);          // turn sonar on at t=1s
        if (i == 180) game.SetSonar(false);        // off at t=3s
        if (i == 120) game.ApplyThrust({2.0f, 0.0f, 1.0f});

        game.Step(1.0f / 60.0f);

        if (i % 60 == 0) {
            std::cout << "t=" << (i / 60) << "s  " << game.StatusText() << "\n";
        }
    }

    std::cout << "\nFinal: " << game.StatusText() << "\n";
    return 0;
}
