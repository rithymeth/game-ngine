# Lanternwake validation

Validated on 2026-11-27 against Aether base commit
`ea7f49121b9bbdeece3cc53c2ea029168fcb581d`.

## Tested results

| Check | Result |
|---|---|
| Standalone native Release build, Linux x86-64 | Passed |
| Native game CTest | 1/1 CTest entry; all 14 gameplay cases passed |
| Same gameplay cases compiled to WebAssembly and executed in Node | 14/14 passed |
| Complete journey through ordinary movement and ability inputs | Won in 1,328 simulation frames (22.13 seconds), four health remaining, one hit |
| Native application autoplay, 1,800 frames | Won; 12 embers, three restored shrines, two hits, score 4,597 |
| SDL window, frame upload and audio callback with dummy drivers | Passed, including a 960 × 540 window |
| Asset lookup when launched outside the checkout | Passed; assets found beside the executable |
| Explicit native executable target packaging | Passed in a fresh isolated build; all 11 assets matched; a deleted copied sprite was restored on rebuild |
| Missing assets / invalid arguments | Clear error; exit codes 1 / 2 respectively |
| Native title and gameplay frame captures | Rendered and visually inspected at 1280 × 800 |
| Browser export, Emscripten 4.0.21 | Built successfully as a single HTML file with embedded runtime, art and fonts |
| In-browser input, rendering and audio | Not verified: this workspace's preview browser denied local-file access |
| Windows and macOS native builds | Not run |

The normal-input gameplay route does not teleport entities, grant resources,
or bypass collision damage. Its result matches between native and wasm32.
Focused cases also cover normalized diagonal movement, boundaries, dash timing
and protection, pulse energy and cooldown, stun, collection conservation,
atomic and interrupted offerings, harmless shades after restoration, contact
damage, defeat, pause, restart, terminal-state freezing and deterministic replay.

## Engine integration gate

The repository CTest gate is being run in a Release build with assertions,
portable editor UI and Luau scripting enabled. The final outcome is recorded
before publication.

The integration configuration includes the actual Aether Engine, UI and Audio
libraries, the portable editor tests and scripting. Optional Vulkan/windowing,
physics, desktop editor, MCP host and sandbox targets are disabled:

```sh
cmake -S . -B build-validation -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DAETHER_ENABLE_ASSERTIONS=ON \
  -DAETHER_BUILD_TESTS=ON -DAETHER_BUILD_EDITOR_UI=ON \
  -DAETHER_BUILD_LANTERNWAKE=ON \
  -DAETHER_BUILD_SANDBOX=OFF -DAETHER_BUILD_PHYSICS=OFF \
  -DAETHER_BUILD_EDITOR=OFF -DAETHER_BUILD_VULKAN=OFF \
  -DAETHER_BUILD_WINDOWING=OFF -DAETHER_BUILD_SCRIPTING=ON \
  -DAETHER_BUILD_MCP=OFF -DAETHER_BUILD_BENCH=OFF
cmake --build build-validation --parallel 8
ctest --test-dir build-validation --output-on-failure --parallel 4
```

## Reproduce the game checks

```sh
bash games/LANTERNWAKE/tools/build_native.sh
ctest --test-dir build-lanternwake --output-on-failure
./build-lanternwake/lanternwake_tests
./build-lanternwake/lanternwake --headless --autoplay --frames 1800 --screenshot ending.png
```

After activating Emscripten, run the exact same simulation cases on wasm32:

```sh
em++ -std=c++20 -O2 -Werror=shift-count-overflow \
  -Iengine/include -Igames/LANTERNWAKE/src \
  games/LANTERNWAKE/tests/game_tests.cpp \
  games/LANTERNWAKE/src/game.cpp engine/src/ecs/archetype.cpp \
  -sENVIRONMENT=node -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 \
  -o /tmp/lanternwake-wasm-tests.js
node /tmp/lanternwake-wasm-tests.js
bash games/LANTERNWAKE/tools/build_web.sh
```

## Portability correction

Building the real engine ECS on wasm32 exposed undefined behavior in
`EntityHash`: a 32-bit `size_t` was shifted left by 32. The fix packs the
index/generation into `u64`, preserves the previous 64-bit result, and mixes
both components before folding to a 32-bit result. Two regression cases were
added to the engine ECS tests. The ECS-only native harness passed all 11 cases;
a wasm32 hash probe passed all eight cases, compiled with
`-Werror=shift-count-overflow`.

## Verification limits

The browser artifact is a successful build, not a claimed browser playtest.
WebAssembly simulation tests do not verify browser input mapping, browser frame
upload, Web Audio behavior, or performance on the user's machine. The native
SDL dummy-driver check verifies initialization and callback execution; it is
not a physical-speaker listening test. Native captured frames verify the
actual game's rendering. No Windows, macOS, mobile, gamepad or touch testing
is claimed. The export targets desktop keyboard and mouse.
