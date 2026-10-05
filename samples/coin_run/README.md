# Coin Run

The Aether sample game: a small 2D platformer. Collect all 10 coins to open
the door, dodge the spikes, and reach the flag.

| Key | Action |
| --- | --- |
| A / D or Left / Right | move |
| Space / W / Up | jump (hold for a higher jump) |
| R | restart |
| Esc | quit |

Build and run (Windows):

```
cmake --build <build-dir> --target coin_run
<build-dir>/samples/coin_run/coin_run.exe
```

## How it's put together

- `coin_run.h/.cpp` -- all gameplay, with no platform or rendering code. The
  player and coins are entities in an `aether::World` (`Position`, `Body`,
  `Player`, `Coin` components); the level is an ASCII tile grid (see
  `Game::DefaultLevel()` for the legend). A fixed 60 Hz `Game::Step(Input)`
  drives it, with coyote time, jump buffering and variable jump height.
- `main.cpp` -- the Win32 front end: `aether::Window`, keyboard polling, a
  fixed-timestep loop and a software pixel-buffer renderer blitted with GDI
  (no GPU needed). `AETHER_COINRUN_MAX_FRAMES=N` auto-quits after N frames and
  `AETHER_COINRUN_SCREENSHOT=file.bmp` saves the last frame.
- `tests/test_coin_run.cpp` -- unit tests, including a scripted run that beats
  the shipped level, so a level edit that makes it unwinnable fails CI.

To make your own level, pass a `std::vector<std::string>` to `Game`.
