# Getting started

## The editor

On Windows, download `AetherEditor-windows-x64.zip` from the latest GitHub
release and unzip it. `aether_editor.exe` is the editor; beside it are the
command-line tools (`aether_cook`, `aether_pak`, `aether_player`,
`aether_docgen`), this manual in `manual/` and the generated API reference in
`api/`. It needs a D3D12 graphics card.

To build from source you need CMake, a C++20 compiler (MSVC, GCC or Clang)
and, on Linux, the X11 development packages for the window layer:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

The editor opens with a sample project and a **Tools** menu: every tool editor
the engine has (Blueprint, Material, Particle, Animation, Behavior Tree,
Navigation, Sound Cue, UI Designer, terrain tools, Tilemap, networking, the
console and profiler, and the Project tools) can be opened from it, either in
the **Tools** hub or popped out into their own windows.

## Your first project

1. Open **Tools > Project > New Project**.
2. Pick a template, give the project a name and a folder, and press
   **Create**. The editor opens the new project.
3. Press **Play** (or run the cooked game: see [Packaging](06-packaging.md)).

Each template creates a startup scene, its input bindings, and what its game
needs (a Luau controller and a spinning-pickup Blueprint for the 3D ones; a
tilemap level and a platformer character for the 2D one). Everything is plain
files under `Content/`, so it works with version control.

| Template | What you get |
|---|---|
| Blank | A camera and a Player Start |
| First Person | WASD and mouse look, jump, pickups |
| Third Person | Camera-relative movement and an orbiting camera |
| Top Down | World-axis movement and a follow camera |
| Vehicle | An arcade car with a chase camera |
| 2D Platformer | A tilemap level, run and jump with coyote time and jump buffering |

## Playing without the editor

The player runs a cooked game:

    aether_cook MyGame/MyGame.aproject --out Paks --config shipping
    aether_player --pak Paks

`--press W --report --headless --frames 120` runs it without a window and
prints where the player ended up, which is how the templates are tested.
