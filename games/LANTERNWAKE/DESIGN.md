# Lanternwake

A short, complete overhead fantasy adventure built on Aether's native C++ ECS.
The player is the last lantern keeper in a forgotten grove. Gather twelve
scattered embers, offer four to each of three shrines, then return through the
northern arch. Six wandering shades guard the routes. A lantern pulse stuns
them; a short dash crosses danger. Five hits end a journey; restarting restores
every collectible and shrine. There is no mandatory time limit.

## Presentation

Original painted forest and transparent character art share an ink-blue,
teal and amber palette. The 1280 × 800 scene uses smooth movement, warm light,
particle trails, a restrained HUD, legible real fonts, an ambient score and
event sounds. Title, pause, defeat, victory and replay are part of the game.
The generated art's source and license/provenance notes travel with the assets.

## Completed implementation

- [x] Simulation: fixed 60 Hz C++ `Game`, actual `aether::World` components,
  normalized movement, collision damage, dash, pulse, shrine offering,
  terminal states, input edges and deterministic tests.
- [x] Presentation: original PNGs loaded with Aether's image decoder, a
  game-owned pixel renderer, TrueType text, animated lighting and particles.
- [x] Platform: SDL2 window/input/audio adapter, a display-free screenshot
  path, native build, and the same C++ code compiled to WebAssembly if the
  toolchain is available. SDL is the host platform layer.
- [x] Delivery: reproducible scripts, asset manifest, test/build evidence,
  playable package and reviewable Git branch. Run the repository CTest gate
  if the complete dependency build is available and disclose its outcome.

The native and single-file browser builds are delivered. All 14 gameplay
cases pass natively and on wasm32. The complete registered headless Linux
CTest suite passes 55/55 checks. See [VALIDATION.md](VALIDATION.md) for exact
configuration, reproduction commands and the browser/platform testing limits.

## Controls and acceptance

WASD/arrows move; Space dashes; F or left mouse pulses; hold E near a shrine
to offer four embers; E at the unlocked arch finishes the run. Enter starts,
Escape pauses/resumes, R retries from terminal/pause screens, M mutes.
Keyboard-only play and mouse menu buttons both work. Focus loss pauses.

Check a real start → pickups → three offerings → exit → victory → replay
route, the damage → defeat → retry route, pause freezes, ability cooldowns,
asset missing errors and rendering at different window sizes. Keep the
12-ember accounting exact so no route can lose required resources.

This is a deliberately scoped first playable game, not a claim of a full
commercial release or a demonstration of Aether's Vulkan/PBR renderer.
