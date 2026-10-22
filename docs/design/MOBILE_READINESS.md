# Mobile readiness

Current state of the mobile work in Phase 45. Android and iOS app shells now build in CI, but neither has been run on a device. The
device-farm step still needs a paid service and the user's decision.

**Current state:** Android and iOS app bundles build in CI. Shared touch controls and a platform-neutral haptic request API are implemented;
native mobile windowing, real-device input, and haptic backends remain open.

## Status in the plans

- `platforms/README.md`: Windows, Linux and macOS are built in. Android is meant to plug in from outside the engine as a *platform plugin*
  (a directory with a `CMakeLists.txt` that calls `aether_platform_plugin()`). Only an example plugin exists.
- `UPGRADE_PLAN.md`, Phase 45 (Mobile and Console):
  - 45.1 input abstraction and remapping: done except a remapping UI.
  - 45.2 lifecycle, permissions and memory-pressure events: complete interface and mock; platform plugins can connect native callbacks.
  - 45.3 Android build: started; arm64 Gradle app and debug APK build in CI, and an x86_64 build runs on an emulator in CI, where the app plays Lantern Run headless on start and then renders through the Vulkan RHI into a SurfaceView: a clear plus a triangle drawn with an HLSL pipeline that glslang (built from source for Android) compiles on the device; the job checks a screenshot for both (required job). Touches reach the engine through ApplyWindowEvents (the triangle turns blue while a finger is down; the job long-presses and checks a screenshot). Still open: the scene renderer on Android (materials, meshes, lights), lifecycle (pause/resume), and real-device testing.
  - 45.4 iOS build: started; unsigned arm64 app bundle build in CI, and a Simulator build runs in CI, where the app plays Lantern Run headless on start (required job). Signing, platform windowing and real-device testing remain.
  - 45.5 touch and haptics: started; touch controls and semantic haptic requests are implemented. Native mobile haptic backends remain.
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
2. **No mobile touch backend.** The shared input state, UI routing and virtual controls support touch, and Windows emits `WM_TOUCH` events.
   Android and iOS app shells do not yet feed native touch events into `WindowEvent`.
3. **Texture formats.** The cooker writes BC formats, which most mobile GPUs lack; mobile needs ASTC or ETC2.
4. **Hard limits that fit mobile poorly.**
   - The bindless texture table is `kMaxBindlessTextures = 32`.
   - Push constants are already 128 bytes, the minimum Vulkan guarantees.
   - The frame-constants buffer (scene lights) exists on D3D12 only; Vulkan reports no support and keeps the fixed light, so mobile
     Vulkan would need it implemented.
5. **Renderer cost.** The player's renderer is a plain forward pass with a per-pixel loop over lights. There is no mobile tuning (quality
   tiers beyond the presets, resolution scale, thermal or battery handling) that I could find.
6. **No packaging.** No Gradle build, signing, Android manifest or iOS project, and no way to run on a device.
7. **Mobile haptics.** Scripts can request semantic feedback through `Input.PlayHaptic`, but the Android and iOS shells have not installed a
   native haptic handler yet.

## Suggested order

1. Touch and mouse input for the UI and gameplay (helps desktop too; testable here).
2. The Android platform plugin: window and surface, an input bridge, lifecycle hooks. Needs the NDK.
3. ASTC in the cooker, with a per-platform texture format. Testable here.
4. Frame constants on Vulkan and a larger texture table. Vulkan is not built on the Windows dev machine, so CI is the check.
5. Gradle, signing and CI, then a real device (and the device-farm decision).

Steps 1 and 3 can be done and tested on a desktop; 2, 4 and 5 need the Android NDK or a device.

## Implementation notes (where each step would go)

These are pointers from reading the code, not a finished design.

**Window and surface (gap 1).** `aether::Window` (`platform/window.h`) is one concrete class whose constructor is implemented per platform at
compile time (`window.cpp` for Win32, `window_glfw.cpp` for GLFW). `PlatformPlugin` (`platform/platform_plugin.h`) offers only `startup`,
`shutdown`, `user_data_dir` and `create_device`, so a plugin cannot supply a window today. Two ways to close this:

- add a third backend file (`window_android.cpp`) that wraps the native activity's `ANativeWindow`, selected by CMake like the GLFW one; or
- let `Window` be created in an "injected" mode where the platform plugin owns the OS window and feeds `PushEvent`, `NotifyResized` and
  `NotifyClosed` (already public, marked "called by the platform backend"), and add a `create_window` hook to `PlatformPlugin`.

Either way the Vulkan backend needs a surface for an `ANativeWindow` (`VK_KHR_android_surface`) and, on iOS, a `CAMetalLayer` through MoltenVK.

**Touch input (gap 2).** `WindowEventType` has no touch type. A `Touch` event (id, phase, x, y) would feed two places: `UIInputRouter`
(`ui/input.h`), which already takes a pointer id in `PointerMove/Down/Up`, and `ApplyWindowEvents` (`platform/window_events.cpp`), which
could expose the first touch as `Key::MouseLeft` plus the pointer position for code written for the mouse. Gameplay needs on-screen controls:
a virtual stick producing the `Move` axis, buttons producing `Jump` and the like. These fit the UI layer (`.aui` widgets writing input state).
The player today never forwards pointer events to the UI at all (only keys and the pad), so the pointer path has to be added for desktop
first.

**ASTC textures (gap 3).** `cook::TextureFormat` (`cook/texture_cook.h`) is `RGBA8` and `BC1/3/4/5/7`. Adding `ASTC_4x4`, `ASTC_6x6` and
`ASTC_8x8` means: new enum values and `BlockBytes` (16 bytes per block for every ASTC size), an encoder (a library such as astcenc), a format
choice per cook target in `ProjectSettings` (the cook already takes a configuration), and RHI support. `IDevice::CreateTexture` takes RGBA8
only, so the RHI needs a compressed-texture upload for both backends (or the player decodes to RGBA8 at load as a stopgap, which costs
memory).

**Limits (gap 4).** `kMaxBindlessTextures = 32` (`gfx/rhi/types.h`) and the 128-byte push-constant block are fixed in the scene shader.
Mobile Vulkan drivers vary in how large a descriptor array they allow, so the table size should come from device limits, not a constant.
Frame constants on Vulkan need a descriptor set 2 with a uniform buffer; the HLSL already names it (`register(b0, space2)`).

**Lifecycle (gap 6 and plan step 45.2).** `PlatformLifecycle` and its mock backend exist (`platform/lifecycle.h`). The player does not
subscribe to it yet: on Suspend it should stop audio and save, on MemoryPressure drop caches (the model and texture maps in the scene
renderer), and on Resume restart audio. A real backend feeds it from the activity callbacks.

**What can be tested on this machine.** Touch events and the pointer path to the UI (mock events through `Game`), the lifecycle subscription,
and the ASTC encoder and `.atex` round trip. The window and surface code, Vulkan on a phone GPU, and packaging cannot.

## Step 1 plan: pointer and touch input

Goal: mouse and touch reach the UI and gameplay through one path, built and tested on desktop first, so the Android work later only has to
produce touch events. Findings from the code that shape the plan:

- `ApplyWindowEvents` (`platform/window_events.cpp`) turns mouse movement into per-frame deltas (`Key::MouseX/MouseY`) and buttons into keys.
  `input::InputState` holds no absolute pointer position, so nothing downstream can know where the cursor is.
- The player's main loop (`player/main.cpp`) passes every window event through `ApplyWindowEvents` into `game.Input()` and nothing else. The UI
  gets keys and the pad (`UIInputBridge`), never the pointer.
- `UIInputRouter` already has `PointerMove/Down/Up(pixel, button, pointer)` with per-pointer capture, hover, focus and wheel, and says whether it
  used the input. It is only used by the editor preview and tests.
- `Window` has no cursor mode (hide or capture), and the player does not hide or capture the cursor today; mouse look uses raw deltas.

**Design**

1. *Events.* Add `WindowEventType::Touch` carrying a pointer id, a phase (began, moved, ended, cancelled) and x, y in pixels. Desktop mouse
   stays `MouseMove` plus the button keys.
2. *State.* Give `InputState` a small array of pointers (position, down, active), filled by `ApplyWindowEvents`: the mouse is pointer 0, each
   touch takes the next free slot. Keep `Key::MouseLeft` set for the first touch, so mouse-only code keeps working on a phone.
3. *Player.* `Game::Tick` compares each pointer with last frame and calls `UIInputRouter::PointerMove/Down/Up` (the router's result says whether
   the UI took it). If it did, clear that pointer's button key for this frame so gameplay does not also fire. The viewport size must match the
   swap chain, so add `Game::SetUiSize(w, h)` called by `main.cpp` at start and on resize (headless games keep the default).
4. *Virtual controls.* Two widget types in `ui/controls.h`: `VirtualStick` (writes the left-stick axes) and `TouchButton` (holds a key down).
   They write into `InputState` as the gamepad's keys (`GamepadLeftStickX/Y`, `GamepadA`), so existing mapping contexts, templates and scripts
   work untouched. A game ships them in its `.aui`; a project setting could show them only on touch devices.
5. *Cursor.* Later and separate: `Window::SetCursorMode` for mouse-look games. Until then the UI only reacts when the pointer is over an
   interactive widget, so clicks elsewhere reach gameplay as before.
6. *MCP.* Extend `game_input` (or add `game_pointer`) so an agent can press, move and release pointers, and let `ui_check` report widgets
   that are unreachable by touch (too small: the usual guideline is at least 44 x 44 points).

**Tests (all headless)**

- `ApplyWindowEvents`: touch events fill the pointer slots; two touches are two pointers; cancel releases.
- Player: a layout with a Button and a Slider; feeding pointer down and up at the button's centre gives a click the script sees through
  `UI.WasClicked`; dragging the slider changes its value; a press on empty space reaches gameplay and one on the button does not.
- Virtual stick: a drag from its centre sets `Input.GetAxis2D("Move")` as expected, and releasing returns it to zero.

**Size and risk.** Roughly: events and state (small), router wiring in the player (small), virtual controls (medium: new widgets and their
layout and save format), MCP changes (small). The main risk is multi-touch edge cases (a finger sliding off a button, two fingers on one
control), which the router's per-pointer capture is built for. None of it needs a device; real touch hardware only changes where the events come from.

## Re-check against master at `e1d6e9f` (engine upgrade #207)

Checked what the latest merge from `master` changes for mobile. The gaps above all still stand: Phase 45 in `UPGRADE_PLAN.md` is unchanged
(45.2 started, 45.3 to 45.7 not), and the merge touched no window, RHI, input, texture-cook or lifecycle code.

What it does add that helps a mobile game:

- **Async streaming with an apply budget** (`streaming/streamer.h`: `UpdateAsync`, `apply_budget_ms`). Cells load off the main thread and are
  applied within a per-frame time budget. That is the right shape for phones, where a frame spike shows as a hitch. A mobile profile would set
  a small budget; nothing in the player runs the streamer yet (see the world-partition tools), so this only matters once it does.
- **Replication bandwidth cap and priority function** (net). A shared cap per connection suits cellular links; mobile would pick low defaults.
- **Plugin packages** (`plugin/package.h`, `.apkg`). Useful for shipping an Android platform plugin with the SDK. Note the platform rules
  though: iOS does not allow downloading and running native code after install, so plugin packages can only be bundled at build time there.
- **MCP permissions and session log.** Unrelated to mobile, but destructive tools now ask first, which matters if an agent ever drives a device
  build.

## Effort and risk by step

Rough sizes (S: days, M: a couple of weeks, L: more) for one engineer who knows the code. "Device" means it cannot be finished without Android
or iOS hardware or the NDK.

| Step | Size | Risk | Needs a device or the NDK |
| --- | --- | --- | --- |
| 1. Pointer and touch input (plan above) | M | Low: multi-touch edge cases | No |
| 2. Android platform plugin (window, surface, input bridge, lifecycle) | L | High: native activity lifetime, surface loss on suspend | Yes (NDK, then a device) |
| 3. ASTC cooker and compressed texture upload | M | Medium: encoder quality and speed, two RHI backends | Cooker no; real GPUs yes |
| 4. Vulkan frame constants and a device-limit based texture table | M | Medium: Vulkan not built on the Windows dev box | CI covers compile; GPUs for behaviour |
| 5. Gradle, signing, CI, device farm | L | High: store rules, certificates, the paid farm | Yes |
| 6. Player lifecycle handling (suspend, memory pressure) | S | Low | No (mock backend) |
| 7. Mobile quality presets and resolution scale | M | Medium: needs profiling on devices | Yes, to tune |

Suggested cut for a first milestone: steps 1, 3 (cooker only) and 6, all verifiable on this machine, leaving 2, 4, 5 and 7 for when the NDK
and a test phone are available.

## Step 3 plan: ASTC textures (with a finding that changes its size)

Finding: the cooker already writes block-compressed `Cooked/<guid>.atex` files into the pak (`cook/cooker.cpp`), but **the player never reads
them**. `SceneRenderer::LoadTexture` (`player/src/scene_renderer.cpp`) reads the original image from the pak by its content path and decodes it
to RGBA8 (`assets::DecodeImageFromMemory`), then `IDevice::CreateTexture` uploads RGBA8. So today a cooked game ships both forms, and the
compressed one is unused. Adding ASTC therefore means finishing the BC path first, which also helps desktop memory.

1. *Cooker.* Add `ASTC_4x4`, `ASTC_6x6`, `ASTC_8x8` to `cook::TextureFormat` and `BlockBytes` (16 bytes per block). Encode with astcenc
   (Apache-2.0, fetchable like the repo's other dependencies). `TextureCookSettings` gains a target format family (`desktop` = BC, `mobile` =
   ASTC) chosen per cook, so one project cooks both ways. Bump the `.atex` version.
2. *RHI.* Add a compressed upload, `CreateCompressedTexture(format, width, height, mips)`, to `IDevice` for both backends (D3D12 BC formats map
   to DXGI; Vulkan maps BC and ASTC to `VkFormat`, and a device may lack one family, so add `SupportsTextureFormat(format)`).
3. *Player.* Load `.atex` when the pak has it and the device supports its format; otherwise fall back to the source image. Sort out the
   32-slot bindless table first (step 4), since mip chains do not change the count but more textures per scene will hit it sooner.
4. *Tests.* Round-trip `.atex` for the new formats (no GPU); a software ASTC decode compared with the source within a tolerance; cook a
   project for each family and check the formats chosen (normal maps, greys, colour). GPU upload tests only where a device exists.

## Step 6 plan: suspend and memory pressure in the player

Finding: `PlatformLifecycle` (`platform/lifecycle.h`) is not used anywhere in the player (the `Lifecycle` in `player/src/game.cpp` is the
entity OnStart/OnUpdate dispatcher, a different thing). Plan:

1. `Game` takes a `PlatformLifecycle&` (or owns one) and subscribes at load.
2. On *Suspend*: pause the frame loop's clock (no catch-up of the missed time on resume), stop audio output, flush pending async saves
   (`SaveSystem::Flush`), and optionally autosave through the existing save API.
3. On *Resume*: restart audio, reset the frame timer, re-apply settings.
4. On *MemoryPressure*: drop the renderer's decoded-image and model caches (`SceneRenderer`'s texture and model maps, rebuilt on demand);
   at *Critical*, also drop streamed cells that are not in use.
5. `main.cpp` wires a `MockLifecycleBackend` behind a `--simulate-lifecycle` option so it can be exercised on desktop and by the MCP server
   (a `game_lifecycle` tool).
6. Tests: with the mock backend, suspend then resume leaves game time unchanged and audio restarted; a memory-pressure event frees the
   renderer caches (checked through counters); an async save in flight completes before the suspend handler returns.

## Decisions needed from you

- Which mobile platforms matter first: Android only, or iOS too (iOS needs a Mac and signing, and rules out downloaded native plugins).
- Whether the first milestone is the desktop-verifiable set (steps 1, 3 cooker part, 6) or goes straight to an Android build.
- The device-farm job is a paid service (`UPGRADE_PLAN.md` 45.7): yes or no.
- Mobile as a product goal: today the docs list mobile as out of scope for the current game work, so this note is for planning only until you decide.

## Step 4 plan: frame constants on Vulkan, and device-based limits

Today only D3D12 implements frame constants (`IDevice::SupportsFrameConstants`); the scene shader then reads `g_Frame` at `register(b0, space2)`.
From reading `vulkan_backend.cpp`, a Vulkan version needs:

1. *Buffers.* `CreateBufferInternal` makes every buffer with `VERTEX_BUFFER | INDEX_BUFFER` usage only. Add `UNIFORM_BUFFER_BIT` (the file
   already says an unused usage bit on a host-visible buffer is harmless), and implement `CreateConstantBuffer` (zero-filled, 256-byte padded
   like D3D12, or padded to `minUniformBufferOffsetAlignment`) and `UpdateConstantBuffer` (a memcpy into the mapped memory, as
   `UpdateVertexBuffer` does).
2. *Descriptors.* Add a third set layout (one `UNIFORM_BUFFER` at binding 0, vertex and fragment stages) next to the bindless texture and
   sampler layouts. `register(b0, space2)` maps to set 2, binding 0 under DXC's default mapping, matching how `space0` and `space1` already map
   to the two existing sets. The descriptor pool is created with `maxSets = 2`; raise it and add a `UNIFORM_BUFFER` pool size.
3. *Binding.* `BindFrameConstants(buffer)` needs a descriptor set pointing at that buffer: allocate one set per constant buffer when it is
   created (the renderer makes 8) and write it once, then `vkCmdBindDescriptorSets` at set index 2 with the bound pipeline layout.
4. *Pipelines.* When `frame_constants_size_bytes > 0`, `CreatePipeline` appends the set-2 layout to `pSetLayouts` (the layout currently lists
   exactly the two bindless sets, and only when `enable_bindless_textures` is set).
5. *Report support.* `SupportsFrameConstants` returns true. The existing test `RHI_VulkanLeavesFrameConstantsUnsupportedForNow` becomes the
   positive test. Vulkan is not built on the Windows dev machine, so this needs the Linux or macOS CI jobs, or a local Vulkan SDK, to verify.

Limits to read from the device instead of constants: `kMaxBindlessTextures = 32` (query `maxPerStageDescriptorSampledImages` and
`maxDescriptorSetSampledImages`, which are small on some mobile GPUs), and the 128-byte push-constant block (`maxPushConstantsSize`; 128 is the
guaranteed minimum, so staying at 128 is safe, but moving data into the frame constants would give headroom).

## Step 2 plan: the Android platform plugin

Outline only; this step needs the NDK and a device, so it is the least certain.

- *Build.* An out-of-tree plugin directory (`platforms/android`) using `aether_platform_plugin()`, built with the NDK's CMake toolchain, producing
  a shared library loaded by a thin Java or Kotlin `NativeActivity`.
- *Window and surface.* The activity hands the plugin an `ANativeWindow`; either a new `window_android.cpp` backend or the injected-window mode
  (see "Implementation notes") wraps it, and the Vulkan backend creates its surface with `VK_KHR_android_surface`. Surface loss on
  suspend must tear down the swap chain and recreate it on resume, which the swap chain does not do today.
- *Input.* Touch events from `AInputEvent` become `WindowEventType::Touch` (step 1); the back button maps to `Key::Escape`; the soft keyboard
  feeds `Char` events for text boxes.
- *Lifecycle.* The activity callbacks (`onPause`, `onResume`, `onTrimMemory`) feed `PlatformLifecycle::Dispatch` (step 6).
- *Files.* `user_data_dir` returns the app's internal storage path; paks are read from the APK assets (an asset-manager VFS mount) or from
  `obbs`, so `GamePackage` needs a way to mount a pak from a file descriptor or in-memory span rather than only from a path.
- *Audio.* miniaudio selects AAudio or OpenSL ES itself; check latency and the audio-focus rules (pause on a phone call).
- *Packaging.* Gradle project generated from a template with the cooked paks as assets; debug keystore for development, release signing
  left as the user's decision (certificates).

## Status: step 1 built (local)

Pointer and touch input is implemented as planned, except where noted:

- `WindowEventType::Touch` and `TouchMove` (with a finger id), and `InputState` pointers (`GetPointer`, `TouchSlot`): the mouse is pointer 0 and
  fingers take slots 1 to 9; the first finger also holds `Key::MouseLeft`. `ApplyWindowEvents` fills them.
- `Game::UpdateUiPointers` forwards the pointers to `UIInputRouter` each Tick; a press the UI takes is hidden from gameplay. Without a renderer
  (tests, the MCP server) it lays the UI out itself, in 1920 x 1080 unless a draw set another size.
- `ui::VirtualStick` and `Button::hold_key` (layout fields `key_x`, `key_y`, `radius`, `fixed`, `hold_key`) write the keys they name, so existing
  `Move` and `Jump` bindings work with no change. Verified on Stardust Hop: a finger dragged on the stick walks the player, and holding the
  JUMP button jumps.
- MCP: `game_input` takes `mouse` and `touches`.
- Not done: no platform backend produces touch events yet (GLFW and Win32 do not), no `Window::SetCursorMode`, no multi-touch gestures. (`ui_check` now has a `touch_target` size check and validates
  the key names a button or stick drives.)

## Status: step 6 built (local)

The player handles the platform lifecycle (`Game::Platform()`, `Suspended()`, `MemoryPressureEvents()`), as planned in "Step 6 plan":

- *Suspend:* `Tick` returns at once with no time passing, audio output stops, and queued saves are written before the dispatch returns.
- *Resume:* audio restarts and the first `Tick` ignores the time spent away.
- *Memory pressure:* counted and passed to subscribers. **The game drops nothing yet.** The renderer's textures and models are GPU resources the RHI
  cannot free (there is no destroy call), and the 2D physics holds pointers into the tilemap caches, so clearing either is unsafe today. Releasing
  caches needs RHI resource destruction first; add it to step 3 or 4.
- MCP: `game_lifecycle` (suspend, resume, memory_pressure) so an agent can exercise it; the test uses `MockLifecycleBackend`.
- Not done: no real backend feeds it yet, and `--simulate-lifecycle` for the player executable was skipped (the MCP tool covers it).

## Status: step 3, cooker half built (local)

The cooker can now write ASTC textures:

- `cook::TextureFormat` gains `ASTC_4x4`, `ASTC_6x6` and `ASTC_8x8` (16-byte blocks; 8, 3.56 and 2 bits per pixel), `BlockWidth`, and block-size aware
  `MipBytes`. The `.atex` container carries them (same layout; the format byte extends, so an older reader refuses the file cleanly).
- Encoding and decoding use ARM's astc-encoder 5.4.0 (Apache-2.0), fetched like the repo's other dependencies and built scalar (no SIMD) so one
  binary runs on any CPU. Quality 0 to 4 maps to its fastest, fast, medium, thorough and exhaustive presets. sRGB colour uses the sRGB profile;
  normal maps use its normal-map mode and decode back to x, y and a rebuilt z like BC5. License text is in `third_party/licenses/astc-encoder.txt`.
- `TextureTarget` (desktop or mobile) steers Auto: mobile picks ASTC 4x4 for normal maps and images with alpha and ASTC 6x6 otherwise; desktop is
  unchanged. `cook_format` in an asset's settings also accepts `astc4x4|astc6x6|astc8x8`. `CookOptions::texture_target` and the MCP `asset_cook`
  `texture_target` option select it; it is part of the derived-data cache key.
- Tests: PSNR bounds for each format on a 53 x 37 image (not a multiple of any block), the container round trip, normal-map direction, and the
  automatic format choice.

Still open for step 3, as noted earlier: the player does not read `.atex` at all (it decodes the source image to RGBA8), and `IDevice::CreateTexture`
takes only RGBA8. So a mobile cook now contains ASTC data that nothing loads yet; the next piece is the RHI compressed-texture upload and the
player's `.atex` loading, with a CPU decode (astc-encoder has a decoder) as the fallback where the device lacks the format.

## Status: step 3, the player now uses cooked textures (local)

- `LoadPackageTexture` (`player/src/package_textures.h`): the scene renderer asks for a texture's pixels by content path; it returns the source image
  if the pak has it, else decodes the cooked `.atex` (BC or ASTC) to RGBA8. The renderer's loader uses it.
- `CookOptions::strip_source_textures` (MCP `asset_cook` option of the same name) leaves each cooked texture's source image out of the archive; a
  texture that could not be cooked keeps its source. Together with `texture_target: mobile` this gives an ASTC-only package.
- Verified on Stardust Hop: a mobile cook with the sources stripped (no `UI/coin.png` in the archive) renders the HUD's coin icon in the real player.
- Tests: the cook option (sources absent, ASTC formats, smaller archive), and loading from the cooked copy with a lossy-but-same-picture bound.

Still open: the textures reach the GPU as RGBA8 (decoded on the CPU at load), so there is no GPU memory saving yet. A true compressed upload needs
`IDevice::CreateCompressedTexture` and format-support queries in both backends (Vulkan cannot be built or checked on the Windows dev machine).

## Status: compressed textures on the GPU, D3D12 (local)

- RHI: `IDevice::SupportsCompressedTexture` and `CreateCompressedTexture(format, width, height, mips)` for BC1, BC3 and BC7 (`CompressedTextureFormat`,
  `CompressedMip` in `rhi/types.h`). The texture keeps its whole mip chain compressed on the GPU, in the same bindless table as RGBA8 textures.
  BC4 and BC5 are not offered, because a shader samples one or two channels from them and the scene shader expects colour. Devices that cannot
  (Vulkan, for now) answer false and the caller decodes to RGBA8.
- D3D12: a second `gfx::Texture` constructor that uploads per-mip block rows through footprints (`GetCopyableFootprints`).
- Player: when a package's source images are stripped, BC1, BC3 and BC7 colour textures stay compressed as cooked; everything else (normal maps,
  BC4 and BC5, ASTC) is decoded to RGBA8 as before. With the sources present nothing changes: the exact source is used.
- Tests: an RHI test on a real D3D12 device (a BC7 chain with levels below one block, BC1, and refused wrong-sized levels).

Not done: ASTC on the GPU (mobile Vulkan), and the Vulkan implementation of this call (the file cannot be built on the Windows dev machine).

## Status: Vulkan frame constants and compressed textures (local note, code pushed)

Both are implemented in `vulkan_backend.cpp`: frame constants as descriptor set 2 (a uniform buffer per constant buffer, from a pool of 64), and
`CreateCompressedTexture` for BC1, BC3 and BC7 (the device enables `textureCompressionBC` when the GPU has it; the image upload is one general
function with per-level copy regions). A GPU without BC, such as a phone's, answers `SupportsCompressedTexture` false. They were written without a
local Vulkan build (a syntax check against the Khronos headers passed); the Linux (lavapipe) and macOS (MoltenVK) CI jobs are the first real run.
For mobile Vulkan the missing piece is ASTC: a device-level ASTC feature check and an ASTC upload path would let ASTC-cooked textures stay
compressed on a phone's GPU.

## Status: ASTC on the GPU (built)

`rhi::CompressedTextureFormat` gained `ASTC_4x4`, `ASTC_6x6` and `ASTC_8x8`, with `CompressedBlockWidth` and `CompressedLevelBytes` replacing the hard-coded 4x4 block maths. D3D12 reports ASTC unsupported (empty handle). Vulkan enables `textureCompressionASTC_LDR` when the GPU has it, maps the formats to `VK_FORMAT_ASTC_*_UNORM_BLOCK` and checks support per format. The player uploads ASTC-cooked colour textures compressed for stripped packages where the device supports them; otherwise it decodes to RGBA8 as before.

Verified: 1023/1023 tests locally; the Vulkan code is syntax-checked only (Vulkan is not built on the dev machine), so CI's MoltenVK job is its first real run. Still open: a platform backend that produces touch events, memory-pressure texture release, a cursor mode, and Android packaging.

## Status: texture release, touch, cursor, quality presets (built)

- `IDevice::DestroyTexture` frees a GPU texture and its bindless slot (D3D12 and Vulkan; waits for the GPU). A Critical memory-pressure event makes the renderer free its textures: models draw untextured afterwards, UI images reload on demand.
- Windows `WM_TOUCH` becomes `Touch` / `TouchMove` events (the mouse messages Windows synthesizes from a finger are dropped). `Window::SetCursorMode` adds Hidden and Locked on Win32 and GLFW.
- Quality presets now do two things: **resolution scale** (the swap chain is created smaller than the window and D3D12's flip model stretches it; UI pointers are scaled to match; Vulkan keeps full size because its surface must match the window) and **view distance** (the camera's far plane is pulled in). A preset change made while playing is applied on the next frame. Shadows, MSAA and bloom are in the presets but the renderer has none of them, so they do nothing.
- Still open, needing a device or the NDK: Android/iOS native window and touch integration, a native-window buffer size for resolution scale on Vulkan/Android, and real profiling to tune the presets.

## Status: haptic request API (built)

- `input::Haptics` accepts semantic effects (`selection`, light/medium/heavy impact, success, warning and error) with a normalized intensity.
- `Game::HapticFeedback()` lets a platform host install the device handler. Luau scripts can query `Input.HasHapticOutput()` and request
  feedback with `Input.PlayHaptic(effect, intensity)`; the request returns false when no handler is connected.
- No Android or iOS haptic handler is connected yet. The mobile application shells still need native runtime integration before real devices
  can produce feedback.
