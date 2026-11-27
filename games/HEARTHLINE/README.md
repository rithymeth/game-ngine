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

Collect resources around the opening camp, build the bridge at x=62, cross the river, defeat the guardian near x=90, place two relays at x=105 and x=107 from an adjacent tile and rotate each east with 2, then activate the beacon at x=108. Build a campfire and survive 24 action turns, replenishing firewood as needed. Combat now runs at a fixed 20 Hz independent of movement or other actions. The guardian chases within a bounded eight-tile leash, navigates terrain, telegraphs its strike for 600 ms, and recovers for 600 ms. Attacks have a 400 ms cooldown; evasion grants 300 ms of strike immunity and has a one-second cooldown. Move away from the marked tiles or time the V key to avoid damage. Night survival remains turn-based for this prototype; use Space (or the Wait button) rather than V to wait. The terminal simulation advances five combat ticks per command; use . to wait.

| Key | Action |
| --- | --- |
| Arrow keys / WASD | Move one tile |
| G | Gather nearby pickup or harvest adjacent tree/rock |
| B | Build bridge at crossing |
| K / V | Attack / timed evasion (300 ms) |
| Space | Wait one survival turn |
| P / Q | Place relay at fixed first / second puzzle socket |
| E | Open adjacent supply cache or activate solved beacon |
| F / R | Build campfire / refuel |
| H | Heal using food |
| T | Rotate construction direction clockwise |
| 1 / 3 | Toggle wooden wall / stone floor on the outlined adjacent tile |
| 2 | Rotate nearest adjacent relay |
| J / L | Save / load the current expedition (one local slot) |
| M | Toggle explored-area map (pauses combat and gameplay) |
| Escape | Pause or close map |
| Ctrl+N or F5 after win/loss | Restart |

## Base construction and persistence

Move to face a direction or press T to rotate the outlined build target without moving. Press 1 to place/remove a wooden wall (2 wood), or 3 to place/remove a stone floor (1 stone). Removing a piece refunds its recipe. Walls block movement; stone floors are walkable. Build on an unoccupied grass, floor or ruin tile adjacent to the player, including before beacon activation. The authored main road, cache approaches, guardian leash and ruin objectives are protected from construction. This is an initial adjacent-tile construction system, not yet multi-tile freeform placement or a general crafting tree.

Press J to save, L to load. Desktop and terminal use `hearthline.sav` in the current working directory; the terminal uses lowercase j/l and t, 1, 3 for construction. Desktop/browser loads pause the game so the player can resume safely. Browser saves are stored locally in the current origin's localStorage under `hearthline.save.v1`; they persist through page reloads when browser storage is available. No account or server is involved. Clearing site data loses that save. Each save uses a versioned, explicit little-endian binary format of approximately 768 KiB (approximately 1 MiB when base64 encoded in the browser). Terrain changes, objects, discovery, inventory, pickups, relays, combat state and quest progression are restored. The checksum detects corruption, not malicious tampering. Invalid data is rejected without changing the active game. Native saving writes a temporary file before replacing the existing save. The format currently supports version 1 only; future generator/schema changes require migration or versioning.

## Wilderness and landmarks

The entire 512 x 512 world has harvestable trees and rocks. Stand next to a resource and press G; gathering an opening-camp pickup has priority, otherwise one adjacent tree or rock is cleared into grass and yields two units of wood or stone. Cleared terrain remains walkable.

Four optional supply caches sit along side trails: the Forester's stash, Quarry supply chest, Lost expedition pack and Ruined watchpost. Their paths are guaranteed regardless of seed; each chest can be opened once with E, from its tile or an adjacent tile. Rewards supplement the main route's wood, stone and food without changing the required energy components. The status shows cache and harvested counts. Open M for a 128 x 128 tile map window centered on the player (clamped to world boundaries). Only discovered terrain and landmarks are visible. Gold marks unopened caches, grey marks looted caches, white marks the player, cyan marks the beacon, and red marks a visible guardian. Closing the map restores the prior pause state.

The window title gives the current objective and exact inventory quantities; the HUD shows health, inventory bars, night progress, and fuel. Terrain, trees, rocks and the ranger use original procedural pixel art. Gathered resources disappear, walls are drawn, directional relay indicators show rotation, and explored tiles fade outside a 12-tile sight radius. The fixed logical canvas keeps the HUD readable on resize.


## Browser version (Emscripten/WebAssembly)

This uses the **same C++ simulation and SDL2 render source** as the native build. Activate an Emscripten SDK with `emcmake` on `PATH`, then run:

```sh
emcmake cmake -S games/HEARTHLINE -B build/hearthline-web -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DHEARTHLINE_BUILD_VISUAL=ON
cmake --build build/hearthline-web --target hearthline_visual --parallel 2
```

The browser game is a **single-file HTML** export at `build/hearthline-web/index.html`. Serve that directory over HTTP to play it locally, for example `python3 -m http.server 8000 -d build/hearthline-web`, then open `http://localhost:8000`. Browsers may reject local `file://` loads.

The page includes a responsive canvas, current objectives/inventory text, touchscreen action and movement buttons, and fullscreen control. Keyboard focus is attached to the canvas. The browser build runs an Emscripten animation-frame callback rather than the native blocking event loop.

### Test the browser build locally

Serve the build from the repository root:

```sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory build/hearthline-web
```

With Playwright and its Chromium browser installed, run this in another terminal from the repository root:

```sh
node games/HEARTHLINE/tests/browser_smoke.mjs
```

Set `HEARTHLINE_URL` if you used a different address, `HEARTHLINE_CHROMIUM` to use an existing compatible Chromium executable, or `HEARTHLINE_ARTIFACTS` to choose where screenshots are written (the system temporary directory by default).

The headless Chromium test covers save/reload and core controls at desktop and mobile viewport sizes. The desktop run also checks simulated focus loss during combat and plays through to victory. This does not replace testing other browsers or physical devices. GitHub Actions is disabled by the repository owner; these checks can run locally without changing repository settings.


## Publish the browser game

A dedicated GitHub Actions workflow, `.github/workflows/hearthline-pages.yml`, can publish Hearthline's single-file HTML to a permanent GitHub Pages URL.

The repository currently reports `has_pages: false` in GitHub metadata, so publishing requires a **one-time repository setting**:

1. Open **Settings → Pages** in `rithymeth/game-ngine`.
2. Under **Build and deployment**, select **GitHub Actions** as the source.
3. In **Actions**, run **Publish Hearthline to GitHub Pages** using **Run workflow**.
4. After the deployment succeeds, use the URL reported in the `github-pages` environment/deployment step.

The deployment workflow is deliberately **manual** until Pages is configured, to avoid creating failing CI runs or unintentionally replacing another site. It publishes only the Hearthline game, not the engine documentation. The repository is not assumed to be publicly playable until the deployment succeeds and the URL is tested.

## Implementation and limitations

- Deterministic **512 x 512** tile terrain with a designated **128 x 128** expedition area. Three dense byte-sized runtime grids hold terrain, constructed objects and persistent discovery (786,432 cells total, 768 KiB of cell data). These are not yet Aether tilemap assets.
- Shared gameplay simulation in `src/game.cpp`; the SDL2 host in `src/visual_main.cpp` draws only visible tiles around the player and sends keyboard actions to the same core.
- 64 seeded regression expeditions, 64 exploration scenarios and 64 construction/persistence scenarios exercise all four cache routes, one-time loot, natural resource depletion, combat timing, idle death, timed and mistimed dodges, cooldowns, healing, retreat, discovery, bridge placement, relay rotation, and survival. Local Release CMake/CTest and strict-warning undefined-behavior sanitizer builds passed the core tests. The WebAssembly build passed the headless Chromium smoke at desktop and mobile viewport sizes, including save/reload.
- This is a native SDL2 graphical prototype with an experimental Emscripten browser export; it is not yet an Aether sprite-batch renderer or a tested commercial-quality experience.
- Guardian attack markers, health bars, slash, evasion and damage feedback use procedural animations. Movement is still tile-stepped; there are no imported sprite sheets, sounds, recurring day/night cycles, nighttime enemy waves, world streaming, generalized relay solver, or benchmarked 60 FPS guarantee.
