# Mobile readiness (research)

What it would take to make mobile games (Android, iOS) with Aether, from reading the repo's plans and code. Nothing here was built or run on
a device: "looks supported" means supported on paper. Mobile is out of scope for the current AETHER-01 work
(`AETHER_01_ENGINE_READINESS_MATRIX.md`, `M8_PRODUCTION_TECHNOLOGY.md`), so none of it is scheduled.

**Bottom line:** the engine has building blocks for mobile, but nothing builds or runs on Android or iOS today.

## Status in the plans

- `platforms/README.md`: Windows, Linux and macOS are built in. Android is meant to plug in from outside the engine as a *platform plugin*
  (a directory with a `CMakeLists.txt` that calls `aether_platform_plugin()`). Only an example plugin exists.
- `UPGRADE_PLAN.md`, Phase 45 (Mobile and Console):
  - 45.1 input abstraction and remapping: done except a remapping UI.
  - 45.2 lifecycle, permissions and memory-pressure events: started (`aether/platform/lifecycle.h`, a mock backend); no real backend.
  - 45.3 Android build (CMake toolchain, Gradle, signing) and 45.4 iOS build: not started; CI-only (NDK, macOS runners).
  - 45.5 touch and haptics: not started.
  - 45.7 device-farm job: needs a paid service, so the user's decision.
- `PHASE_SPECS.md` §25.3: cooked textures are BC1/3/4/5/7; "ASTC for mobile ... comes with Phase 33's mobile targets".

## Already in place

- ARM64 maths: the SIMD maths compiles through sse2neon (the Apple Silicon route).
- A Vulkan RHI backend; the player uses D3D12 on Windows and Vulkan elsewhere.
- Audio through miniaudio (which has mobile backends).
- Jolt physics, Luau scripting, cooked and encryptable paks.
- The UI input router models touch pointers (`ui/include/aether/ui/input.h`).

## Gaps that block a mobile game

1. **No window or surface layer.** `window.h` has a Win32 and a GLFW implementation; GLFW has no Android or iOS target. A platform plugin
   would supply the native window, surface and event loop.
2. **No touch input.** `WindowEvent` carries keys, mouse and gamepad axes only. Nothing feeds touches to the UI or to gameplay, and there is no
   on-screen virtual stick.
3. **Texture formats.** The cooker writes BC formats, which most mobile GPUs lack; mobile needs ASTC or ETC2.
4. **Hard limits that fit mobile poorly.**
   - The bindless texture table is `kMaxBindlessTextures = 32`.
   - Push constants are already 128 bytes, the minimum Vulkan guarantees.
   - The frame-constants buffer (scene lights) exists on D3D12 only; Vulkan reports no support and keeps the fixed light, so mobile
     Vulkan would need it implemented.
5. **Renderer cost.** The player's renderer is a plain forward pass with a per-pixel loop over lights. There is no mobile tuning (quality
   tiers beyond the presets, resolution scale, thermal or battery handling) that I could find.
6. **No packaging.** No Gradle build, signing, Android manifest or iOS project, and no way to run on a device.
7. **UI input.** The player's UI works with keys and the gamepad, not the mouse or touch.

## Suggested order

1. Touch and mouse input for the UI and gameplay (helps desktop too; testable here).
2. The Android platform plugin: window and surface, an input bridge, lifecycle hooks. Needs the NDK.
3. ASTC in the cooker, with a per-platform texture format. Testable here.
4. Frame constants on Vulkan and a larger texture table. Vulkan is not built on the Windows dev machine, so CI is the check.
5. Gradle, signing and CI, then a real device (and the device-farm decision).

Steps 1 and 3 can be done and tested on a desktop; 2, 4 and 5 need the Android NDK or a device.
