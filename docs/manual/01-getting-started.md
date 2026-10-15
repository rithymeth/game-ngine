# Your first playable build

Build and play **Coin Run**, a small 2D platformer included in the repository.
This is the shortest working path from a clean checkout to a running game. It
does not require a graphics card or prior Aether experience.

## Requirements

- Windows 10 or 11, x64, with an AVX2-capable CPU.
- Visual Studio 2022 with **Desktop development with C++** and a Windows SDK.
- CMake 3.20 or later and Git.
- Internet access during the first CMake configure so it can download the
  engine's pinned source dependencies.

The Coin Run window uses Windows GDI software rendering. It does not use the
engine's D3D12 renderer. Current platform scope is described in the
[support policy proposal](support_policy.md).

## Build and launch

Open PowerShell in the repository root and configure a small build that omits
editor, physics, scripting, Vulkan, and test targets:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DAETHER_BUILD_EDITOR=OFF `
  -DAETHER_BUILD_QT_EDITOR=OFF `
  -DAETHER_BUILD_EDITOR_UI=OFF `
  -DAETHER_BUILD_MCP=OFF `
  -DAETHER_BUILD_SANDBOX=OFF `
  -DAETHER_BUILD_PHYSICS=OFF `
  -DAETHER_BUILD_SCRIPTING=OFF `
  -DAETHER_BUILD_VULKAN=OFF `
  -DAETHER_BUILD_TESTS=OFF
cmake --build build --config RelWithDebInfo --target coin_run
$game = Get-ChildItem .\build\samples\coin_run -Recurse -Filter coin_run.exe |
  Select-Object -First 1
& $game.FullName
```

The first configure downloads dependencies and can take a few minutes. Later
configures reuse them. The sample opens a window; collect all 10 coins to open
the door, then reach the flag.

| Controls | Action |
|---|---|
| A / D or Left / Right | Move |
| Space / W / Up | Jump; hold for a higher jump |
| R | Restart |
| Esc | Quit |

## Make one change

Close the game. In `samples/coin_run/coin_run.cpp`, find the `DefaultLevel()`
map and change one `C` (coin) to `.` (empty space). Rebuild with the same
`cmake --build` command and launch again. The coin counter will reflect the
updated map. This is the sample's gameplay source; its simulation is separate
from the Windows window and renderer.

## If the build does not launch

- If CMake cannot download a dependency, check the network connection and
  configure again; the partial dependency cache is reused.
- If Windows reports an AVX2 or illegal-instruction error, the CPU does not
  meet this build's x64 requirement.
- If `coin_run.exe` was not found, check that the `coin_run` build completed
  successfully and that the shell is in the repository root.

## Where to go next

- The editor's **New Project** tool includes **Blank**, **First Person**, **Third Person**, **Top Down**, **Vehicle** and **2D Platformer** starting points; see [the template notes](README.md#the-aether-manual) and inspect each sample before choosing a workflow.
- For later command-line steps, `aether_cook` builds a packaged game, `aether_player` launches a cooked package, and `aether_docgen` generates the API reference. See [packaging](06-packaging.md) and [API reference](08-api-reference.md).
- Read [Projects and assets](02-projects-and-assets.md) for project structure.
- Use the [Aether manual](README.md) for scripting, input, 2D, and packaging.
- The Qt editor is an opt-in Windows/D3D12 migration host with partial tool
  coverage. See its [current status and build steps](../../editor/qt/README.md)
  before choosing it as your first project workflow.
- [`tutorials/FIRST_GAME.md`](../tutorials/FIRST_GAME.md) describes a broader
  Blueprint-based target workflow; it is a roadmap acceptance document, not a
  tutorial that works in the current editor.
