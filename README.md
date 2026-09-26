# Aether

A data-oriented, job-based game engine targeting Vulkan/DirectX 12. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) *(coming later)* for the full
design.

## Status: Phase 5 — Gameplay Framework

### Phase 1 — Foundation

- **Memory** (`engine/include/aether/memory`): `LinearAllocator` (bump-pointer
  arena), `PoolAllocator` (fixed-size free-list), `FrameAllocator`
  (double-buffered ring of linear allocators for transient per-frame data).
- **Logging** (`engine/include/aether/core/log.h`): leveled, thread-safe,
  printf-style macros (`AETHER_LOG_INFO`, etc).
- **Filesystem** (`engine/include/aether/platform/filesystem.h`): thin
  wrapper over `std::filesystem` for byte/text I/O.
- **Math** (`engine/include/aether/math`): SIMD (SSE) `Vec4`/`Mat4`,
  plain `Vec3`, `Quaternion`, column-major/right-handed to match Vulkan.

### Phase 2 — Core Runtime

- **Job System** (`engine/include/aether/job`): `WorkStealingQueue`, a
  lock-free Chase-Lev deque (owner pushes/pops LIFO from the bottom, other
  threads steal FIFO from the top). `JobSystem` runs one queue per
  participating thread (the calling thread plus N workers); `Schedule`/
  `ScheduleBatch` fan work out, and a caller-owned `JobCounter` is the
  explicit dependency primitive `Wait()` blocks on — including recursively,
  from inside a running job, for nested fork-join. Automatic read/write
  dependency inference from component access (the Task Graph in the original
  design) is a scheduling layer to build on top of this, not part of it yet.
- **ECS** (`engine/include/aether/ecs`): archetype-based, chunked SoA
  storage. Entities with the same component signature share an `Archetype`;
  each `Archetype` is a list of fixed-size `Chunk`s (16 KiB) holding
  contiguous per-component arrays. `World::CreateEntity`/`AddComponent`/
  `RemoveComponent` migrate an entity between archetypes (move-constructing
  shared components, swap-removing from the old archetype to keep storage
  dense); `World::ForEachChunk`/`ForEach` are the read side systems iterate,
  one chunk at a time — the natural unit to hand to `JobSystem` for parallel
  iteration (wiring that up is next).

### Phase 3 — Platform & Graphics Abstraction

Concrete backend is **D3D12** (Vulkan headers/SDK aren't installed in this
environment; the RHI is factored so a Vulkan backend can be added later
without touching call sites — see the Phase 3 discussion in project history).

- **Window** (`engine/include/aether/platform/window.h`): Win32 window +
  message pump, resize callback, no third-party dependency.
- **D3D12 RHI** (`engine/include/aether/gfx`): `Device` (device + one direct
  queue + a manually-advanced fence — no hidden per-draw sync), `SwapChain`
  (double-buffered, manual RTV management), `CommandList` (owns an
  allocator+list pair; hands out the raw `ID3D12GraphicsCommandList*` rather
  than re-wrapping the recording API).
- **Shader compiler** (`engine/include/aether/gfx/shader_compiler.h`): HLSL
  source to bytecode via D3DCompiler (fxc). A DXC/SPIR-V path is future work
  for the eventual Vulkan backend.

### Phase 4 — Advanced Graphics

- **Bindless textures** (`engine/include/aether/gfx/descriptor_heap.h`,
  `texture.h`): one shader-visible CBV/SRV/UAV `DescriptorHeap` is bound
  once; a `Texture`'s heap index *is* its bindless handle — shaders index
  `Texture2D g_Textures[] : register(t0, space1)` with an integer passed
  through a root constant, no per-draw descriptor table rebinding. Sized to
  a fixed capacity (256 in the sandbox) rather than a true SM6.6 unbounded
  range, to stay on shader model 5.1 with the classic D3DCompiler.
- **Render graph** (`engine/include/aether/gfx/render_graph.h`): passes
  declare which resources they read/write and in what D3D12 state;
  `RenderGraph::Execute` diffs each resource's last-known state against what
  the pass needs and inserts exactly the barriers required, so passes stop
  hand-rolling `ResourceBarrier` calls. Resource state is tracked by pointer
  identity and persists across frames (needed for correctness — e.g. a swap
  chain backbuffer must be known to start next frame in `PRESENT`).
  Dependency-driven pass *reordering* is still not implemented; passes run in
  the order they were registered, within their queue.
- **GPU-driven culling** (`sandbox/main.cpp`): a compute pass extracts 6
  frustum planes from the view-projection matrix (Gribb-Hartmann) and tests
  each instance's bounding sphere, writing one indirect-draw command per
  instance (`InstanceCount` 0 or 1) into a UAV buffer; a graphics pass then
  issues all commands in a single `ExecuteIndirect` — the CPU never learns
  which instances survived culling. Verified via a one-time GPU→CPU
  readback used only for logging (20/64 instances visible in the sandbox
  scene, deterministic across runs), never to drive rendering.

### Phase 5 — Gameplay Framework

Two real third-party dependencies land here, both pulled via CMake
`FetchContent` (network required to configure): **Jolt Physics** (v5.6.0)
and **Dear ImGui** (v1.92.9, Win32 + DX12 backends). Both were the
higher-effort options offered at the time (a custom physics system and a
CLI-only inspector were the lower-risk alternatives) — see the Phase 5
discussion in project history.

- **Physics** (`physics/`, a separate `Aether::Physics` target so the core
  engine stays dependency-free unless you opt in): `PhysicsWorld` wraps a
  Jolt `PhysicsSystem` plus the allocator/broadphase-layer boilerplate Jolt
  requires, exposed through `aether::Vec3`/`Quaternion` rather than `JPH::`
  types for the common create-body/step/read-transform path. `Transform` and
  `RigidBody` (`physics/include/aether/physics/components.h`) are plain ECS
  components; `SyncPhysicsToTransforms(world, physics, dt)` steps the
  simulation and writes each `RigidBody` entity's simulated pose back into
  its `Transform`. Physics jobs run on `aether::JobSystem` — not a
  Jolt-owned thread pool — via `JoltJobSystemAdapter`
  (`physics/include/aether/physics/jolt_job_system_adapter.h`), which
  implements Jolt's `JPH::JobSystem` interface (job allocation via a
  `FixedSizeFreeList`, `QueueJob`/`QueueJobs` scheduling onto
  `aether::JobSystem`) on top of `JPH::JobSystemWithBarrier` for the
  Barrier/`WaitForJobs` machinery `PhysicsSystem::Update()` blocks on. Any
  thread that calls into `PhysicsSystem::Update` (i.e. `PhysicsWorld::Step`)
  must already be registered with the `aether::JobSystem` passed to
  `PhysicsWorld`'s constructor.
- **Scene serialization** (`engine/include/aether/scene/serialization.h`):
  `SaveScene`/`LoadScene` walk every archetype generically (via new
  type-erased `World::ForEachArchetype`/`CreateEntityRaw`/`GetComponentRaw`)
  and (de)serialize each component by **name**, not its runtime-assigned
  numeric id — the id is only stable within one process, so a scene file
  saved by one binary can still load correctly in another. Every component
  gets a default raw-byte (de)serializer for free the first time
  `GetComponentId<T>()` runs; `RigidBody` overrides it
  (`SetComponentSerializer`) to save shape/mass instead of its live Jolt body
  handle, which is meaningless once reloaded — the editor recreates the
  physics body on load.
- **Editor** (`editor/`): a Dear ImGui overlay on the Phase 3/4 D3D12 layer
  driving a live `World` + `PhysicsWorld` — entity list with **live-editable**
  position/radius/mass/static fields (position edits teleport the Jolt body
  via `PhysicsWorld::SetPosition`; radius/mass/static edits recreate it,
  since Jolt's shape and motion type are effectively immutable once a body
  exists), spawn/play/pause controls, Save/Load Scene buttons. Rendering is
  plain instanced `DrawInstanced` colored quads reading `Transform` each
  frame (no bindless textures or GPU culling here — Phase 4 already proved
  those out; keeping this phase's new surface area to ImGui + physics +
  serialization). Required one small engine change:
  `Window::native_message_hook` so ImGui's Win32 backend can see input
  messages without the engine's `Window` class exposing its private
  `WndProc`.

### Follow-up: transient render graph resources & async compute

- **Transient resources** (`RenderGraph::CreateTransientTexture`,
  `GetOrCreateRTV`/`GetOrCreateDSV`): unlike `ImportResource` (externally
  owned), the graph allocates and keeps these alive itself, keyed by name so
  calling it again (e.g. after a window resize) recreates the underlying
  resource in place under the *same* `ResourceHandle` — passes referencing it
  don't need to change. The sandbox uses this for a real depth buffer
  (`DXGI_FORMAT_D32_FLOAT`), replacing the depth-disabled PSOs every phase
  before this one shipped with. Distinct transient resources are **not**
  sub-allocated/aliased against a shared heap even when their lifetimes don't
  overlap — each gets its own committed allocation; true memory aliasing is
  still future work.
- **Async compute** (`Device::ComputeQueue`/`SubmitCompute`/
  `GraphicsQueueWaitOnCompute`, `RenderGraph`'s per-pass `QueueType`): a
  second, independent `ID3D12CommandQueue` (its own fence) that can execute
  concurrently with the direct/graphics queue on hardware that supports it.
  `RenderGraph::AddPass` takes an optional `QueueType` tag and
  `RenderGraph::Execute` records each pass into whichever of the two command
  lists the caller provides matches its queue — a Compute-tagged pass's
  barriers only ever need states legal on a compute list (`UNORDERED_ACCESS`,
  not `INDIRECT_ARGUMENT`); the state's final transition into something
  graphics-only happens on the graphics list instead, automatically, from the
  same barrier-diffing logic already in place. The sandbox's GPU-driven
  culling pass now runs on the compute queue, with the graphics queue
  GPU-side-waiting (`GraphicsQueueWaitOnCompute`, not a CPU stall) on its
  fence before consuming its output — verified stable (identical 20/64
  culling result) across 960 frames spanning 8 repeated runs.

### Follow-up: transient resource memory aliasing

`RenderGraph::CreateTransientTexture` now defers actual GPU allocation to
`Execute()`'s internal `Compile()` step, once the frame's full pass list (and
therefore each transient resource's lifetime — the span between its first
and last use) is known. Distinct transients whose lifetimes don't overlap
are placed into a shared `ID3D12Heap` via `CreatePlacedResource` (first-fit-
decreasing by size) instead of each getting its own committed allocation,
with a `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` barrier inserted automatically
at whichever pass first activates the later resource. A transient touched
from both queues is conservatively excluded from aliasing (its own dedicated
allocation) rather than risk an unsafe cross-queue alias. There's no public
D3D12 API to confirm two *texture* resources share physical memory from the
resource objects themselves (`GetGPUVirtualAddress` is only meaningful for
buffers) — verified instead via a diagnostic `RenderGraph::ShareHeapAllocation`
accessor checking the engine's own bucket-assignment bookkeeping, trusting
`CreatePlacedResource` to honor the heap+offset contract it documents; a
dedicated test also confirms overlapping-lifetime transients are correctly
*refused* aliasing. The sandbox's depth buffer now goes through this same
allocation path (a "bucket of one," exercising it without literally needing
a second transient to alias against) — reverified stable across 1,500+
frames spanning 5 repeated runs.

### Follow-up: dependency-driven pass reordering (attempted, reverted)

Worth recording honestly rather than hiding: a same-queue topological sort
over pass resource usages was built, then removed after its own test caught
that it was a mathematically guaranteed no-op. With only a resource handle
and a state per usage — no notion of a `Write` producing a new "version" a
`Read` can selectively bind to — every dependency edge that construction can
produce necessarily points from an earlier registration index to a later
one. Feeding that into a topological sort with "smallest ready index first"
as the tie-break reproduces the original registration order for *any*
input; it can neither reorder anything nor (since a cycle would require a
backward edge) ever detect one. Genuine reordering needs resource
versioning, a distinctly larger feature than what was scoped here — see
`ComputeExecutionOrder`'s comment in `render_graph.cpp` for the full
reasoning. Passes execute in plain registration order per queue.

### Follow-up: Vulkan backend (swappable RHI)

`engine/include/aether/gfx/rhi` is a genuinely backend-swappable abstraction
— `IDevice`/`ISwapChain`/`ICommandList` — for the layer that actually *can*
be made identical across D3D12 and Vulkan: device/queue creation, a swap
chain, command list recording lifecycle, submission, and fence-based
GPU/CPU sync. `rhi_demo/` runs the exact same frame loop, written entirely
against these interfaces, on **either** backend depending on an environment
variable — proof it's real swapping, not two code paths pretending to share
an interface.

Deliberately **not** abstracted: shader compilation, pipeline state, and
descriptor/resource binding. HLSL root signatures and SPIR-V descriptor sets
differ enough that unifying them is a distinctly larger, separate piece of
work — by default `rhi_demo` just clears the backbuffer to a color and
presents (no shaders, no draw calls). The full sandbox/editor (bindless
textures, compute culling, ImGui) stay D3D12-only, using the original
(non-abstracted) `Device`/`SwapChain`/`CommandList` classes directly — the
RHI backends adapt those by composition (`d3d12_backend::D3D12Device` wraps a
`gfx::Device`, etc.) rather than modifying them, so none of Phases 3-5's
tested code changed.

**Built without the Vulkan SDK**: this environment has a Vulkan-capable GPU
driver (which ships the loader, `vulkan-1.dll`) but not the ~1GB LunarG SDK,
and so no validation layers either — `VulkanDevice`'s debug-layer flag is
accepted for symmetry with the D3D12 backend but is currently always a
no-op, and everything was verified by actually running it (thousands of
frames across both backends and repeated runs, including a programmatic
`ISwapChain::Resize()` exercise) rather than via validation output. Headers
come from a fast `FetchContent` of `KhronosGroup/Vulkan-Headers`; the import
library is generated at configure time from the driver's own loader DLL
(`tools/generate_vulkan_import_lib.ps1`: `dumpbin /EXPORTS` → a linker
`.def` file → `lib.exe`) — `find_package(Vulkan)` is tried first and used
instead if a real SDK is present. Timeline semaphores (core Vulkan 1.2)
stand in for D3D12-style monotonic fence values.

### Follow-up: Vulkan hello-triangle (real shaders/pipeline, same HLSL source)

`AETHER_RHI_DEMO_DRAW_TRIANGLE=1` switches `rhi_demo` from clear-to-color
into an actual rotating draw call, on **either** backend, compiled from the
*same* HLSL source (`rhi_demo/main.cpp`'s `kTriangleShaderSource`) — no
vertex buffer, indices come from `SV_VertexID`, a push/root constant drives
the rotation. This is the "shaders/pipelines aren't abstracted" boundary the
RHI header comments call out: triangle mode doesn't go through
`ICommandList::TransitionTexture`/`ClearRenderTarget` at all (those exist
only for the clear-to-color mode's calling convention — see
`ToVkImageLayout`'s comment for why `RenderTarget` maps to Vulkan's
`TRANSFER_DST_OPTIMAL`, the wrong layout for a render-pass attachment).
Instead it records genuinely backend-specific commands via each object's
`NativeHandle()` escape hatch:

- **D3D12**: `D3DCompile` (fxc) to DXIL, a one-root-constant root signature,
  a graphics PSO with no input layout (procedural vertices), raw
  `ID3D12GraphicsCommandList` recording (`OMSetRenderTargets`,
  `DrawInstanced`).
- **Vulkan**: the *same* HLSL source through DXC's `-spirv` flag
  (`CompileHLSLToSPIRV`, `engine/include/aether/gfx/shader_compiler.h`) to
  SPIR-V, a real `VkRenderPass`/`VkFramebuffer`/`VkPipeline` (dynamic
  viewport/scissor state, so the pipeline itself is resize-independent), a
  push-constant range in place of the root constant.

**Real bug found and fixed by actually running the resize test**, not by
inspection: `VulkanSwapChain::Resize()` unconditionally destroys and
recreates every `VkImageView`, even when the surface's
min/maxImageExtent clamps the requested size straight back to what it
already was (exactly what happens here — the demo resizes the swapchain
without resizing the real HWND). A framebuffer cache keyed only on
width/height/image-count therefore missed the rebuild and `vkCmdBeginRenderPass`
crashed against a `VkFramebuffer` built from already-destroyed image views.
Fixed by keying the cache on the actual `VkImageView` handles instead
(`VulkanTriangleResources::EnsureFramebuffers` in `rhi_demo/main.cpp`).

**Dependency found and fixed by actually running it, not by inspection**:
the Windows SDK's own `dxcompiler.dll` is a build with SPIR-V codegen
disabled (`SPIR-V CodeGen not available. Please recompile with
-DENABLE_SPIRV_CODEGEN=ON.`) — fine for `CompileHLSL`'s unrelated
D3DCompile/fxc path, useless for `CompileHLSLToSPIRV`. `engine/CMakeLists.txt`
now fetches Microsoft's own official DXC GitHub release build (hash-pinned
via `file(DOWNLOAD ... EXPECTED_HASH)`) when `AETHER_BUILD_VULKAN` is on,
which does have SPIR-V codegen enabled and exports the same
`DxcCreateInstance` entry point the SDK's import library already resolves at
link time — no relink needed, only the runtime DLL changes. `rhi_demo`'s
CMakeLists copies whichever `dxcompiler.dll` was resolved next to its
executable as a post-build step.

### Follow-up: real vertex/index buffers + depth testing (cube demo)

`AETHER_RHI_DEMO_DRAW_CUBE=1` (takes priority over `DRAW_TRIANGLE` if both are
set) upgrades `rhi_demo` from the procedural, buffer-less triangle into an
actual indexed, depth-tested cube — exercising real GPU buffer creation and
upload identically on both backends: `gfx::Buffer` (upload-heap, persistently
mapped) for D3D12, a host-visible/host-coherent `VkBuffer`/`VkDeviceMemory`
for Vulkan. One depth buffer *per swapchain image*, not a single shared one:
this demo's frame loop only fences a slot right before reusing it, so two
backbuffers' GPU work can genuinely overlap — a single shared depth buffer
would be a real write/write hazard between concurrent frames. The MVP matrix
is built with the engine's existing math (`aether::Mat4`, the same
`PerspectiveRH`/`LookAtRH` `sandbox/main.cpp` already uses) and passed as a
D3D12 root constant / Vulkan push constant; a `FlipY` constant baked into the
same push-constant block compensates for D3D12/Vulkan's opposite NDC Y
convention directly in the one shared HLSL source, rather than needing two
different projection matrices.

### Follow-up: async compute on the Vulkan backend

`IDevice` gained `CreateComputeCommandList`/`SubmitCompute`/
`WaitForComputeFence`/`IsComputeFenceComplete`/`ComputeQueueWaitOnGraphics`/
`GraphicsQueueWaitOnCompute` — the same async-compute primitives `gfx::Device`
already had for the D3D12-only sandbox, now genuinely implemented on both RHI
backends (this is squarely inside the "device/queue/submission" layer the
RHI abstraction covers, unlike shaders/pipelines). `VulkanDevice` picks a
**dedicated** async-compute queue family (`VK_QUEUE_COMPUTE_BIT` without
`VK_QUEUE_GRAPHICS_BIT`) when the hardware exposes one — confirmed via
`tests/aether_tests.exe`'s log output to actually be found on this machine's
NVIDIA GPU (queue family 2) — falling back to a second queue instance in the
graphics family, and finally (logged loudly) to sharing the same queue if
neither is available. D3D12's `ID3D12CommandQueue::Wait` is a true
queue-level primitive independent of any submission; Vulkan has no
equivalent in core 1.2, so `ComputeQueueWaitOnGraphics`/
`GraphicsQueueWaitOnCompute` queue the wait semaphore and attach it to
whichever `SubmitCompute`/`Submit` call comes next on that queue. Verified
via lifecycle and non-deadlock tests mirroring the existing RHI test style
(59/59 tests passing) — genuine cross-queue race correctness isn't
practically testable without validation layers, matching this whole RHI
effort's established verification approach.

Not yet started: unifying shader/pipeline/draw-call recording across
backends into a real cross-API abstraction (the triangle/cube demos are
backend-specific code selected at runtime, not a unified API) — bindless
descriptors and multiple simultaneous draw calls/objects are still open.

### Follow-up: asset pipeline (file-based texture loading, cached)

The first item of the post-Phase-5 roadmap: `aether::assets` adds real
file-based asset loading on top of the existing engine, replacing the
sandbox's procedurally-generated checkerboard textures with actual PNG files
loaded from disk (`assets/textures/`).

- `assets::DecodeImageFile` (`engine/include/aether/assets/image.h`) decodes
  PNG/JPG/BMP/TGA into RGBA8 pixels via `stb_image` (`nothings/stb`,
  `FetchContent`-fetched, single header, no build system of its own — wrapped
  by one `.cpp` providing `STB_IMAGE_IMPLEMENTATION`). Platform-independent;
  built unconditionally, not gated on `WIN32`.
- `assets::AssetManager` (WIN32-only, needs `gfx::Device`/`DescriptorHeap`)
  adds path-keyed caching on top: `LoadTexture(path, upload_cmd)` decodes and
  uploads on a cache miss and returns the cached bindless index on a hit —
  loading the same path twice does **not** re-decode or re-upload, verified
  directly (`AssetManager_LoadingSamePathTwiceReturnsCachedIndexWithoutReupload`),
  not just assumed.
- Named `DecodeImageFile`, not `LoadImage`: the obvious name collides with
  the WinAPI macro `LoadImage` → `LoadImageA`/`LoadImageW` from `Windows.h`,
  which any TU including the D3D12 headers pulls in — caught at link time
  (`unresolved external symbol ... LoadImageA`), not by inspection.
- **Real bug found by actually running `aether_sandbox`/`aether_editor`
  after wiring this in** (not by inspection): both were already silently
  broken before this change, crashing on launch with `STATUS_DLL_NOT_FOUND`.
  `shader_compiler.cpp`'s `CompileHLSLToSPIRV` (added for the Vulkan
  hello-triangle follow-up) references `DxcCreateInstance` unconditionally,
  so any executable that pulls that object file into its link — anything
  that calls `CompileHLSL`, like the sandbox — gets `dxcompiler.dll` as an
  implicit load-time dependency, even though it never calls the SPIR-V path.
  `aether_tests.exe` happened to never trigger this (no test calls
  `CompileHLSL`, so the linker never pulls that object file in), which is
  why it went unnoticed. `rhi_demo` already had the fix (it does call
  `CompileHLSLToSPIRV`); `sandbox`/`editor` didn't, since nothing in either
  had needed `dxcompiler.dll` before. Both `CMakeLists.txt` now copy
  `dxcompiler.dll` next to their executable as a post-build step, same as
  `rhi_demo`.

Not yet started: reference counting/unloading, async/streamed loading, and
non-texture asset types (meshes, materials, ...) — see the glTF loader and
material system items later in the roadmap.

### Follow-up: PBR renderer (`pbr_demo/`)

Roadmap item 2: a real physically-based renderer — Cook-Torrance specular
(GGX normal distribution, Smith geometry term, Schlick Fresnel), Lambertian
diffuse, metallic-roughness workflow, matching the standard textbook
derivation (the same one LearnOpenGL/Sascha Willems' widely-used reference
implementations use). A separate project from `sandbox/` (same reasoning as
`rhi_demo/`): `sandbox/` stays focused on bindless textures + GPU-driven
culling, `pbr_demo/` on the lighting model, rather than merging both into one
increasingly tangled demo.

Scene: the classic "material ball grid" used to validate a PBR
implementation — a 7x7 grid of spheres, metallic varying 0→1 along one axis
and roughness 0.05→1 along the other, one directional light plus a small
ambient term. This was actually **looked at**, not just run without
crashing: a mathematically wrong BRDF still renders *something*, so
`AETHER_PBR_DEMO_SCREENSHOT=<path>` dumps the final frame to a PNG (via
`stb_image_write`, same `stb` fetch as the asset pipeline) for exactly this
kind of visual check. The rendered grid shows the expected qualitative
trends: rough spheres look matte/chalky regardless of metallic, smooth
spheres show a tight specular highlight that's white on dielectrics
(metallic=0) and tinted by the albedo on metals (metallic=1), and metals
darken toward black in their diffuse response as expected.

**Real bug found by actually capturing a screenshot, not by inspection**: a
single-frame capture (`AETHER_PBR_DEMO_MAX_FRAMES=1`) came back blank. DXGI's
flip-model swap chain advances `GetCurrentBackBufferIndex()` as soon as
`Present()` is called, so after the loop's *final* `Present()`,
`SwapChain::CurrentBackBuffer()` already points at the *next*,
never-rendered buffer — not the one the last frame actually drew into.
Fixed by having the caller track the last-rendered buffer index explicitly
and read that specific buffer back (`SwapChain::BackBuffer(index)`) instead
of trusting "current".

Not yet started: normal mapping, image-based lighting/environment
reflections, and multiple/colored lights — the single hardcoded directional
light is enough to validate the BRDF but not a full lighting pipeline.

## Building

Requires CMake 3.20+, a C++20 compiler with SSE4/AVX2 support (MSVC, Clang,
or GCC), and network access the first time you configure (to fetch Jolt
Physics, Dear ImGui, Vulkan-Headers, stb_image, and — on a Vulkan-enabled
Windows build — a SPIR-V-capable `dxcompiler.dll` release).

```bash
cmake -S . -B build
cmake --build build --config RelWithDebInfo
./build/sandbox/aether_sandbox.exe          # Windows only; Phase 3/4 bindless + GPU-culling demo
./build/editor/aether_editor.exe            # Windows only; Phase 5 ImGui editor + physics
AETHER_RHI_BACKEND=vulkan ./build/rhi_demo/aether_rhi_demo.exe   # or =d3d12 (default); swappable RHI proof
ctest --test-dir build --output-on-failure
```

Set `-DAETHER_BUILD_PHYSICS=OFF`, `-DAETHER_BUILD_EDITOR=OFF`, and/or
`-DAETHER_BUILD_VULKAN=OFF` to skip the corresponding fetches (e.g. for a
quick engine-only build with no network).
