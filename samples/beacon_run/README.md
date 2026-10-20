# Beacon Run

A top-down arena game and the third playable Aether sample. Collect all eight
beacons while dodging three patrol drones. Three collisions end the run.

| Key | Action |
| --- | --- |
| W / A / S / D or arrow keys | move |
| R | restart |
| Esc | quit |

Build and run on Windows: `cmake --build <build-dir> --target beacon_run`.

## How it is put together

- `beacon_run.h/.cpp` contains the fixed 60 Hz simulation and ECS entities for
  the player, beacons and drones. It has no window or rendering dependency.
- `main.cpp` contains the Win32 input loop and a small GDI software renderer.
  `AETHER_BEACONRUN_MAX_FRAMES=N` auto-quits and
  `AETHER_BEACONRUN_SCREENSHOT=path.bmp` saves the last frame.
