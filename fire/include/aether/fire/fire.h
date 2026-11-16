#pragma once

#include "aether/core/base.h"
#include "aether/math/math.h"
#include "aether/reflection/reflection.h"

namespace aether::fire {

// Something that can burn: put it on an entity. It catches when it has been heated enough (by a burning neighbour, or Fire.Ignite from a script), burns for `fuel`
// seconds, heats whatever is within `spread_radius` while it does, and then is burnt: it stays where it is, or is destroyed (`consume`). Wetness slows the heating and
// puts a burning one out. While it burns, a flame (a particle system, from `flame_effect`) and a light follow it. A component, saved with the entity.
struct Flammable {
    static constexpr usize kEffectLength = 128;

    bool enabled = true;
    f32 fuel = 10.0f;                 // seconds it burns for once alight
    f32 ignition_heat = 1.0f;         // how much heat it takes to catch
    f32 spread_radius = 3.0f;         // m: how far its flames heat others
    f32 heat_rate = 0.6f;             // heat per second it gives something right beside it, falling to nothing at spread_radius
    f32 cool_rate = 0.25f;            // heat lost per second while nothing heats it
    f32 wetness = 0.0f;               // 0 to 1: scales heating down, and puts it out at douse_wetness. Set by a script (Fire.SetWetness, Fire.WetAll)
    f32 douse_wetness = 0.6f;         // a burning one at or above this goes out, and one at or above it cannot catch
    bool consume = false;             // destroy the entity when it has burnt out
    char flame_effect[kEffectLength] = {}; // a particle system asset (.avfx) for the flames; empty: no flames drawn
    Vec3 flame_offset{0.0f, 0.0f, 0.0f};   // where the flames and light sit, from the entity's position, m
    Vec3 light_color{1.0f, 0.55f, 0.2f};
    f32 light_intensity = 12.0f;      // 0: no light
    f32 light_range = 8.0f;
};

void RegisterFireComponents();

} // namespace aether::fire

AETHER_REFLECT(aether::fire::Flammable, 1,
    AETHER_FIELD(enabled, Field_EditAnywhere),
    AETHER_FIELD(fuel, Field_EditAnywhere, {.tooltip = "Seconds it burns for once alight", .range_min = 0.1, .range_max = 100000.0, .units = "s"}),
    AETHER_FIELD(ignition_heat, Field_EditAnywhere, {.tooltip = "How much heat it takes to catch", .range_min = 0.01, .range_max = 1000.0}),
    AETHER_FIELD(spread_radius, Field_EditAnywhere, {.tooltip = "How far its flames heat others", .range_min = 0.0, .range_max = 1000.0, .units = "m"}),
    AETHER_FIELD(heat_rate, Field_EditAnywhere, {.tooltip = "Heat per second it gives something right beside it, falling to nothing at the radius", .range_min = 0.0, .range_max = 1000.0}),
    AETHER_FIELD(cool_rate, Field_EditAnywhere, {.tooltip = "Heat lost per second while nothing heats it", .range_min = 0.0, .range_max = 1000.0}),
    AETHER_FIELD(wetness, Field_EditAnywhere, {.tooltip = "Slows heating and puts a burning one out at douse_wetness", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(douse_wetness, Field_EditAnywhere, {.tooltip = "At or above this a burning one goes out and one cannot catch", .range_min = 0.0, .range_max = 1.0}),
    AETHER_FIELD(consume, Field_EditAnywhere, {.tooltip = "Destroy the entity when it has burnt out"}),
    AETHER_FIELD(flame_effect, Field_EditAnywhere, {.tooltip = "A particle system asset (.avfx) for the flames; empty: none"}),
    AETHER_FIELD(flame_offset, Field_EditAnywhere, {.tooltip = "Where the flames and light sit, from the entity's position", .units = "m"}),
    AETHER_FIELD(light_color, Field_EditAnywhere, {.tooltip = "The flames' light"}),
    AETHER_FIELD(light_intensity, Field_EditAnywhere, {.tooltip = "0 for no light", .range_min = 0.0, .range_max = 10000.0}),
    AETHER_FIELD(light_range, Field_EditAnywhere, {.tooltip = "The light fades to nothing here", .range_min = 0.1, .range_max = 1000.0, .units = "m"}))
