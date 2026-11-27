# HEARTHLINE — First Expedition

Top-down 2D exploration, combat, building, survival, and beacon puzzle prototype.

## Play the graphical prototype (native SDL2)

Requires CMake 3.21+, C++20 compiler, SDL2 development libraries, and a graphical desktop.

```sh
cmake -S games/HEARTHLINE -B build/hearthline -DHEARTHLINE_BUILD_VISUAL=ON -DBUILD_TESTING=ON
cmake --build build/hearthline
ctest --test-dir build/hearthline --output-on-failure
./build/hearthline/hearthline_visual
```

On Ubuntu, install SDL2 with `sudo apt-get install libsdl2-dev`. The visual executable is opt-in so ordinary simulation-only builds have no SDL2 dependency. For a noninteractive SDL test:
```sh
SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./build/hearthline/hearthline_visual --smoke --seed 7
```

## Game objectives

Collect resources around the opening camp, build the bridge at x=62, cross the river, defeat the guardian near x=90, place two east-facing relays at x=105 and x=107, then activate the beacon at x=108. Build a campfire and survive 24 action turns, replenishing firewood as needed.

| Key | Action |
| --- | --- |
| Arrow keys / WASD | Move one tile |
| G | Gather nearby resource |
| B | Build bridge at crossing |
| K / V | Attack / dodge guardian |
| P / Q | Place relay at fixed first / second puzzle socket |
| E | Activate solved beacon |
| F / R | Build campfire / refuel |
| H | Heal using food |
| 1 / 2 | Build wall to east / rotate adjacent eastern relay |
| Escape | Pause |
| Ctrl+N or F5 after win/loss | Restart |

The window title gives the current objective and exact inventory quantities; the HUD shows health, inventory bars, night progress, and fuel. The visuals currently use colored tile primitives, not production sprites or text rendering.

## Implementation and limitations

- Deterministic **512 x 512** tile terrain with a designated **128 x 128** expedition area. `kLayerCount = 3` is reserved, but the current terrain grid is a single array and not yet three independently authored tile layers.
- Shared gameplay simulation in `src/game.cpp`; the SDL2 host in `src/visual_main.cpp` draws only visible tiles around the player and sends keyboard actions to the same core.
- Three seeded regression expeditions run in CTest; CI also verifies SDL2 headless startup.
- This is a **native graphical prototype**, not yet a WebAssembly/browser export, Aether sprite-batch renderer, or tested commercial-quality experience.
- No sprites, sounds, save/load, world streaming, generalized relay solver, or benchmarked 60 FPS guarantee.
