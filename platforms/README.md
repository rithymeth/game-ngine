# Platform plugins

Windows, Linux and macOS have built-in engine configurations, but that does not
mean each is a supported release target. Current release priorities and the
gates for promoting a platform are in
[Platform Support Priorities](../docs/design/PLATFORM_SUPPORT_POLICY.md).
Other platforms (Android, a console) can plug in from outside the engine as a
**platform plugin**: a directory with a `CMakeLists.txt` that calls `aether_platform_plugin()`
(from `cmake/AetherPlatform.cmake`), plus the code that registers it with
`AETHER_PLATFORM_PLUGIN`. Console plugins live in private repositories
(their SDKs are under NDA); only the extension point is here.

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
  requester that captures plugin state during `shutdown`.
- `user_data_dir`: where saves and settings go.
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
