#pragma once

// Phase 42.6: data layers.
//
// A data layer is a named group of entities that can be switched off together -- the level
// designer's "hide the props while you work on the terrain" tool, and the runtime switch that turns a
// wing of the map on. It reuses the existing Active component rather than inventing a second notion of
// enabled: switching a layer off sets Active on its members, and switching it back restores what each
// entity had before, so an entity that was already off does not come back on.
//
// An entity without a DataLayer component belongs to no layer and is never touched.

#include "aether/ecs/world.h"

#include <string>
#include <string_view>
#include <vector>

namespace aether {

struct DataLayer {
    std::string layer;                  // "" means the entity is in no layer
    bool active_when_layer_on = true;    // what Active.active was before the layer went off
    bool captured = false;               // false until a disable has recorded the state above
};

// The layers that at least one entity belongs to, sorted, without duplicates.
std::vector<std::string> DataLayerNames(const World& world);
// How many entities belong to `layer`.
usize DataLayerSize(const World& world, std::string_view layer);
// Switches every entity in `layer` on or off. Switching off remembers each entity's current
// Active.active; switching on restores it. Returns how many entities changed. A layer nobody belongs
// to changes nothing.
usize SetDataLayerActive(World& world, std::string_view layer, bool active);
// True when every entity in `layer` is currently active. An empty layer is not active.
bool IsDataLayerActive(const World& world, std::string_view layer);

} // namespace aether