// Placeholder Win32 front-end (mirrors gem_hop style).
// Full integration with Aether window, input, and renderer comes next.
// For now this is a compile skeleton.

#include "abyss_protocol.h"
#include <iostream>

int main() {
    abyss::Game game;
    std::cout << "Abyss Protocol starter — submarine at depth "
              << game.GetSub().position.y << "\n";
    game.Step(1.0f / 60.0f);
    return 0;
}
