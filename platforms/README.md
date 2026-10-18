# Platform plugins

Windows, Linux and macOS have built-in engine configurations, but that does not
mean each is a supported release target. Before promoting a console target,
complete its platform-specific gates alongside the
[console certification checklist](../docs/design/CONSOLE_CERTIFICATION.md).
Other platforms (Android, a console) can plug in from outside the engine as a
**platform plugin**: a directory with a `CMakeLists.txt` that calls `aether_platform_plugin()`
(from `cmake/AetherPlatform.cmake`), plus the code that registers it with
`AETHER_PLATFORM_PLUGIN`. Console plugins live in private repositories
(their SDKs are under NDA); only the extension point is here.

The player starts linked platform plugins after loading the game package and
shuts them down after the game exits. Its lifecycle events are connected to
the running `Game`; its `user_data_dir` supplies the save/settings root when
`--user-dir` and `AETHER_USER_DIR` are not set. See the
[console certification checklist](../docs/design/CONSOLE_CERTIFICATION.md) for
platform-neutral release gates. Native SDK and certification requirements
must be filled in by each licensed platform integration.

```sh
cmake -S . -B build -DAETHER_PLATFORM_PLUGINS="platforms/example;/path/to/my-console"
```

## What a plugin can provide

All optional, in `aether::PlatformPlugin` (`aether/platform/platform_plugin.h`):

- `startup` / `shutdown`: bring the platform's SDK up and down; run by
  `PlatformPlugins::Get().Startup()` / `Shutdown()` (shutdown in reverse).
- `connect_lifecycle`: receives the shared `PlatformLifecycle` before
  `startup`, so a backend can install a permission requester and forward native
  suspend, resume, and memory-pressure events. Call `Dispatch(...)` on the main
  thread; remove native callbacks and clear any permission
  requester that captures plugin state during `shutdown`. The player connects
  this service to `Game::Platform()` so suspend flushes saves, stops audio and
  pauses game time; resume restarts audio with a zero-delta first frame.
- `user_data_dir`: the exact root used for saves, settings and crash reports
  when command-line and environment overrides are absent. The player stores
  files in `Saves/` and `Config/` beneath this root.
- `create_device`: an RHI device (`gfx::rhi::IDevice`) for a graphics API
  the engine doesn't build in, created with
  `PlatformPlugins::Get().CreateDevice(name, debug)`.

Game and engine code can subscribe through
`PlatformPlugins::Get().Lifecycle()`. The lifecycle service keeps the platform
API boundary independent of Android or console SDKs, while
`MockLifecycleBackend` drives the same interface in headless tests.

## The example

`platforms/example` is a working template: it registers itself, counts
its startups and reports a user data directory. Linux CI builds it into the
tests, which check it registered.

## Android extension notes

The notes below describe potentially reusable engine pieces; they do not
represent a supported or validated Android target.

The pieces Android needs already exist:

- **ARM64**: the engine's SIMD math compiles through sse2neon, the same
  route Apple Silicon takes.
- **Vulkan**: the RHI's backend, with glslang for HLSL to SPIR-V.
- **Windows**: an Android plugin would provide the window and surface
  from the native activity.

It's built with the NDK's CMake toolchain file.

## Android build shell

`android/` contains a small Gradle application used to build and package the
engine's portable native core for `arm64-v8a`. Gradle configures the root CMake
project through Android's NDK toolchain, then packages the JNI shim and engine
static libraries in a debug APK. The CI job uploads that APK as an artifact.

Build locally with Android SDK command-line tools, JDK 17, Gradle 9.1.0, NDK
28.2.13676358, CMake 3.22.1 and API 36 installed:

```sh
gradle -p platforms/android :app:assembleDebug
```

For an optionally signed release APK, set `AETHER_ANDROID_KEYSTORE`,
`AETHER_ANDROID_STORE_PASSWORD`, `AETHER_ANDROID_KEY_ALIAS`, and
`AETHER_ANDROID_KEY_PASSWORD` in the environment and run
`gradle -p platforms/android :app:assembleRelease`. Keep keystore material in
your local secret store or CI secrets; the repository contains no signing keys.
This first build target packages the native engine core only; it does not yet
provide Android windowing, Vulkan, or a playable game shell.

## iOS build shell

`ios/` contains a minimal UIKit app used to cross-compile and link the native
engine core for arm64 iOS. It is built with CMake's Xcode generator and does not
require code signing for compile validation. On a Mac with Xcode and CMake:

```sh
cmake -S . -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DAETHER_BUILD_IOS_APP=ON \
  -DAETHER_BUILD_TESTS=OFF -DAETHER_BUILD_MCP=OFF \
  -DAETHER_BUILD_SANDBOX=OFF -DAETHER_BUILD_PHYSICS=OFF \
  -DAETHER_BUILD_EDITOR=OFF -DAETHER_BUILD_EDITOR_UI=OFF \
  -DAETHER_BUILD_VULKAN=OFF -DAETHER_BUILD_SCRIPTING=OFF \
  -DAETHER_BUILD_WINDOWING=OFF -DAETHER_BUILD_BENCH=OFF
cmake --build build-ios --config Release --target aether_ios
```

The macOS CI job uploads the unsigned `AetherEngine.app` bundle. This validates
the arm64 engine link and app packaging; distribution signing, launching on an
iOS device or simulator, and a production window/rendering backend are follow-up
work.
