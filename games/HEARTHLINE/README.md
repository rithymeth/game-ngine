# HEARTHLINE — First Expedition

Prototype for a top-down survival-adventure built alongside the Aether Engine.

## Scope
- A deterministic **512 x 512** terrain simulation, with a **128 x 128** authored expedition area and three conceptual map layers.
- Gather wood, stone, energy, and food.
- Bridge a river, fight a guardian, place light relays, activate a beacon, build a campfire, and survive the night.
- Fixed-time action simulation and three seeded regression expeditions.

## Build and test

```sh
cmake -S games/HEARTHLINE -B build/hearthline -DBUILD_TESTING=ON
cmake --build build/hearthline
ctest --test-dir build/hearthline --output-on-failure
```

For now, `hearthline_sim` is a **terminal-driven simulation**, not the earlier described full SDL/WebAssembly playable release. The engine-independent core is intended to be integrated with the existing SDL and Aether renderer before shipping. No assets are imported or copied from another game.

## Current constraints
- The relay puzzle is a fixed two-node arrangement (105,64) and (107,64) facing east, not a generalized circuit solver.
- The world is generated as a dense in-memory tile grid. World streaming, save/load, pathfinding, and visual rendering are pending.
- Survival and enemy behaviors are simplified to deterministic turn actions for reproducible tests.
- Interactive graphical playtesting and FPS benchmarking have not yet happened.

## Key controls for terminal simulation
`w a s d`: move, `g`: gather, `b`: bridge, `k`: attack,
`v`: dodge, `p`/`q`: place relays, `e`: activate beacon,
`f`: build fire, `r`: refuel, `h`: heal, `x`: quit.

This branch intentionally does not alter the engine or other games, so it does not resolve unrelated pull-request merge conflicts.
