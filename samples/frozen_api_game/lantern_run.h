#pragma once

// "Lantern Run": a tiny headless game written the way a third party would write one, against the frozen API only.
// Every engine header it includes is `@stability: stable` (tools/check_api_stability.py --consumer checks that in CI),
// so a release that breaks this file has broken the freeze. A lamplighter walks a street and lights the lanterns it
// passes; the settings come out of a packed archive, and the game saves and reloads its state as JSON.

#include "aether/core/base.h"
#include "aether/ecs/world.h"
#include "aether/math/vec.h"
#include "aether/reflection/reflection.h"
#include "aether/reflection/serialize.h"

#include <string>
#include <vector>

namespace lantern {

struct Settings {
    aether::f32 walk_speed = 2.0f;   // metres per second
    aether::f32 reach = 1.5f;        // a lantern within this of the lamplighter gets lit
    aether::i32 lantern_count = 8;
    aether::f32 spacing = 4.0f;
};

struct Lamplighter {
    aether::Vec3 position;
    aether::i32 lit = 0;
};

struct Lantern {
    aether::Vec3 position;
    bool lit = false;
};

class Game {
public:
    explicit Game(const Settings& settings);

    void Step(aether::f32 dt);
    aether::i32 Lit() const;
    aether::i32 Total() const { return settings_.lantern_count; }
    bool Done() const { return Lit() == Total(); }
    aether::f32 PlayerX() const;

    // The whole state as JSON text, and back.
    std::string Save() const;
    bool Load(const std::string& json);

private:
    Settings settings_;
    aether::World world_;
    aether::Entity player_;
};

// Settings from JSON text; a packed archive holds them in the shipped game.
bool ParseSettings(const std::string& json, Settings& out);
std::vector<aether::u8> PackSettings(const Settings& settings);
bool UnpackSettings(const std::vector<aether::u8>& archive, Settings& out);

} // namespace lantern

AETHER_REFLECT(lantern::Settings, 1,
    AETHER_FIELD(walk_speed, Field_EditAnywhere),
    AETHER_FIELD(reach, Field_EditAnywhere),
    AETHER_FIELD(lantern_count, Field_EditAnywhere),
    AETHER_FIELD(spacing, Field_EditAnywhere)
)
