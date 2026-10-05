# Gem Hop

The Aether 3D sample game: a third-person platformer. Hop across floating
platforms, collect all 8 gems, then stand on the goal pad (it turns gold once
every gem is collected). Fall into the void and you respawn at the start.

| Key | Action |
| --- | --- |
| W / A / S / D | move (relative to the camera) |
| Space | jump (hold for a higher jump) |
| Left / Right | orbit the camera |
| Up / Down | camera pitch |
| R | restart |
| Esc | quit |

Build and run (Windows): `cmake --build <build-dir> --target gem_hop`.

## How it's put together

- `gem_hop.h/.cpp` -- all gameplay, no platform or rendering code. The player
  and gems are entities in an `aether::World`; platforms are axis-aligned
  slabs. Fixed 60 Hz `Game::Step(Input)` with gravity, coyote time, jump
  buffering and variable jump height.
- `main.cpp` -- Win32 front end: `aether::Window`, keyboard polling, an orbit
  camera, and a small software 3D rasteriser (near-plane clipping, z-buffer,
  flat lighting, blob shadow) blitted with GDI, so no GPU is needed.
  `AETHER_GEMHOP_MAX_FRAMES=N` auto-quits and `AETHER_GEMHOP_SCREENSHOT=f.bmp`
  saves the last frame.
- `tests/test_gem_hop.cpp` -- unit tests, including one that checks every hop
  in the shipped route is makeable, so an edit that makes the level
  impossible fails CI.
